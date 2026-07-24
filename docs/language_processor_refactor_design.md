# Language Processor Pipeline Refactor Design

## Overview

This document captures the design for refactoring the `LanguageProcessor` interface into a composable pipeline of modular, standalone primitives. The goal is to abstract text processing fully across all supported languages (including CJK) so that neither the ingestion path nor the query path requires language-specific logic outside the pipeline.

## Core Primitives

### Segmenter

A **Segmenter** converts a chunk of text into multiple tokens. It is a 1→many function responsible for word boundary detection.

Examples:
- `PunctuationSegmenter` — splits on configured punctuation characters (used by Snowball languages)
- `ICUSegmenter` — dictionary/rule-based segmentation (for Chinese, Japanese, Korean, Vietnamese)
- A decompounder could be a second segmenter in the chain (e.g., German compound splitting)

```cpp
class Segmenter {
 public:
  virtual ~Segmenter() = default;
  virtual absl::StatusOr<std::vector<std::string>> Segment(absl::string_view text) const = 0;
  // Whether this segmenter treats the given code point as a word boundary.
  // Used by the query parser for escape handling.
  virtual bool IsDelimiter(uint32_t cp) const = 0;
};
```

**Escape contract**: Segmenters handle `\<char>` in the input text as "include `<char>` literally, do not treat it as a boundary." This supports the query language escape convention. Document text at ingestion does not typically contain escapes.

### TokenFilter

A **TokenFilter** converts a single token into a nullable token. It is a 1→1 or 1→0 function.

Examples:
- `NormalizeCaseFoldFilter` — NFKC/NFC normalization + UTF-8 case folding
- `StopWordFilter` — returns `nullopt` for stop words
- `SnowballStemFilter` — applies Snowball stemming

```cpp
class TokenFilter {
 public:
  virtual ~TokenFilter() = default;
  virtual std::optional<std::string> Apply(absl::string_view token) const = 0;
};
```

**Key property**: TokenFilters are stateless and idempotent. They have strict per-token semantics (no access to the surrounding token list).

## Composed Pipeline: LanguageProcessor

The `LanguageProcessor` is a composition of segmenters and token filters. It owns the components and orchestrates execution.

```cpp
class LanguageProcessor {
 public:
  virtual ~LanguageProcessor() = default;

  // Full pipeline: apply segmenters sequentially, then apply filters to each token.
  // Stateless and idempotent.
  absl::StatusOr<std::vector<std::string>> Process(absl::string_view text) const;

  // Access individual components for direct use by callers that need them.
  const std::vector<std::shared_ptr<Segmenter>>& GetSegmenters() const;
  const std::vector<std::shared_ptr<TokenFilter>>& GetTokenFilters() const;

  // Query a specific filter type. Returns nullptr if not present.
  template <typename T>
  T* FindFilter() const;

  // Factory
  static std::shared_ptr<LanguageProcessor> Create(
      data_model::Language language, const std::string& punctuation,
      const std::vector<std::string>& stop_words);

 private:
  std::vector<std::shared_ptr<Segmenter>> segmenters_;
  std::vector<std::shared_ptr<TokenFilter>> filters_;
};
```

### Pipeline Execution Semantics

`Process(text)`:

1. **Segmentation phase**: Feed `text` to the first segmenter. For each subsequent segmenter in the list, feed every token from the previous step as input. This chains segmenters so that each one further splits the output of the previous.

2. **Filter phase**: For each token in the final segmented output, apply each token filter in order. If any filter returns `nullopt`, the token is eliminated. Otherwise, the filter's output replaces the token for the next filter.

3. Return the final list of surviving tokens.

```
Input text
    │
    ▼
┌──────────────────┐
│ Segmenter 1      │  (e.g., PunctuationSegmenter)
│ text → [tokens]  │
└──────────────────┘
    │
    ▼
┌──────────────────┐
│ Segmenter 2      │  (e.g., Decompounder — optional)
│ [tokens] → [tokens] │
└──────────────────┘
    │
    ▼
┌──────────────────┐
│ TokenFilter 1    │  (e.g., NormalizeCaseFold)
│ token → token    │
└──────────────────┘
    │
    ▼
┌──────────────────┐
│ TokenFilter 2    │  (e.g., StopWordFilter)
│ token → token|∅  │
└──────────────────┘
    │
    ▼
┌──────────────────┐
│ TokenFilter 3    │  (e.g., SnowballStemFilter)
│ token → token    │
└──────────────────┘
    │
    ▼
Final token list
```

## Concrete Compositions

### SnowballProcessor (English, French, German, etc.)

```
Segmenters: [PunctuationSegmenter(punct_set)]
Filters:    [NormalizeCaseFoldFilter(NFC), StopWordFilter(stop_words), SnowballStemFilter(language)]
```

### ICU Processor (Chinese, Japanese, Korean — future)

```
Segmenters: [ICUSegmenter(locale)]
Filters:    [NormalizeCaseFoldFilter(NFKC), StopWordFilter(stop_words)]
```

## Caller Usage Patterns

### Ingestion Path (TextIndexSchema::StageAttributeData)

```cpp
// Full pipeline — produces final tokens
auto tokens = processor->Process(document_text);

// Stem map building — ingestion-specific, uses stem filter directly
if (auto* stem_filter = processor->FindFilter<SnowballStemFilter>()) {
  stem_filter->BuildStemMap(*tokens, min_stem_size, stem_map);
}
```

### Query Path — Regular Text Predicates

The query parser handles query grammar (`@`, `(`, `)`, `|`, `"`, `*`, `%`, `-`, etc.) and extracts raw text spans. These spans are fed to `Process()` for language-agnostic segmentation and filtering.

```cpp
// Parser extracts raw text span between query syntax delimiters
std::string raw_text_span = ExtractTextSpan();

// Full pipeline — same as ingestion
auto tokens = processor->Process(raw_text_span);

// Build predicates from resulting tokens
for (auto& token : *tokens) {
  predicates.push_back(MakeTermPredicate(token));
}
```

**Key change**: The query parser no longer uses `IsPunctuation()` for word boundary detection. It only recognizes query syntax characters. Language-specific word boundaries are delegated entirely to `Process()`.

### Query Path — Wildcard and Fuzzy Predicates

Current behavior for wildcard/fuzzy: normalize + case fold only (no stopword removal, no stemming). Maintain backward compatibility.

```cpp
// Parser detects wildcard/fuzzy markers, extracts inner text
std::string inner_text = ExtractBetweenMarkers();

// Apply only normalization (not the full filter chain)
auto* norm_filter = processor->FindFilter<NormalizeCaseFoldFilter>();
auto normalized = norm_filter->Apply(inner_text);

// Build wildcard/fuzzy predicate from the normalized single token
```

### Delete Path (TextIndexSchema::DeleteKeyData)

Needs to compute the stem root of an already-processed token for stem tree cleanup.

```cpp
auto* stem_filter = processor->FindFilter<SnowballStemFilter>();
if (stem_filter) {
  std::string root = stem_filter->GetStemRoot(token, min_stem_size);
  // Use root to clean up stem tree entry
}
```

### Query Expansion (GetAllStemVariants)

Same pattern — compute stem root for tree lookup.

```cpp
auto* stem_filter = processor->FindFilter<SnowballStemFilter>();
if (stem_filter) {
  std::string stemmed = stem_filter->GetStemRoot(search_term, min_stem_size);
  // Look up stem tree for variants
}
```

### Query Parser — Escape Boundary Detection

For escape handling (`\<char>`), the parser needs to know if a character is a delimiter for the segmenter. Access the segmenter directly:

```cpp
auto& segmenters = processor->GetSegmenters();
bool is_boundary = segmenters[0]->IsDelimiter(cp);
```

## SnowballStemFilter Extended Interface

The `SnowballStemFilter` has domain-specific methods beyond the `TokenFilter` interface because stemming has use cases outside the pipeline (delete path, query expansion):

```cpp
class SnowballStemFilter : public TokenFilter {
 public:
  // TokenFilter interface — applies stemming as part of the pipeline
  std::optional<std::string> Apply(absl::string_view token) const override;

  // Compute the stem root without modifying the token.
  // Used by delete path and query expansion for stem tree lookups.
  std::string GetStemRoot(absl::string_view token, uint32_t min_stem_size = 0) const;

  // Build stem map from already-processed tokens.
  // Ingestion-specific: maps stem roots to their original surface forms.
  void BuildStemMap(const std::vector<std::string>& tokens,
                    uint32_t min_stem_size, InProgressStemMap& stem_mappings) const;
};
```

## Design Principles

1. **Modularity**: Segmenters and TokenFilters are standalone classes, independently instantiable and testable. They do not depend on the pipeline.

2. **Composability**: The pipeline is purely a composition of independent components. Adding a new language means composing different segmenters/filters — no pipeline interface changes.

3. **Statelessness**: `Process()` is stateless and idempotent. No side effects. Stem map building is a caller responsibility invoked separately.

4. **Language agnosticism**: Neither the ingestion path nor the query path contains language-specific logic. All language-specific behavior is encapsulated in the segmenter/filter composition.

5. **Direct component access**: Callers with needs beyond the pipeline (wildcard normalization, stem root computation, boundary detection) access components directly via `GetSegmenters()`, `GetTokenFilters()`, or `FindFilter<T>()`. This is explicit and avoids polluting the pipeline interface with utility methods.

6. **Backward compatibility**: Wildcard/fuzzy predicates maintain current behavior (normalize only). Escape handling convention is preserved in the segmenter contract.

## Migration Notes

### Files to Change

| File | Change |
|------|--------|
| `language_processor.h` | Redefine as composed pipeline with `Process()` + component access |
| `language_processor.cc` | Implement `Process()` pipeline execution |
| `snowball_processor.h/cc` | Refactor into `PunctuationSegmenter` + `NormalizeCaseFoldFilter` + `StopWordFilter` + `SnowballStemFilter` composed via `LanguageProcessor::Create()` |
| `text_index.cc` | Update `StageAttributeData` to use `Process()` + separate `BuildStemMap`. Update `DeleteKeyData` and `GetAllStemVariants` to use `FindFilter<SnowballStemFilter>()->GetStemRoot()` |
| `filter_parser.cc` | Refactor `ParseQuotedTextToken` / `ParseUnquotedTextToken` to extract raw text spans and call `Process()`. Wildcard/fuzzy use `FindFilter<NormalizeCaseFoldFilter>()->Apply()` directly |
| `textinfocmd.cc` | Replace `Tokenize()` with `Process()` |

### New Files

| File | Purpose |
|------|---------|
| `segmenter.h` | `Segmenter` abstract interface |
| `token_filter.h` | `TokenFilter` abstract interface |
| `punctuation_segmenter.h/cc` | Punctuation-based segmenter (extracted from SnowballProcessor) |
| `normalize_case_fold_filter.h/cc` | NFKC/NFC + case fold filter (extracted from SnowballProcessor) |
| `stop_word_filter.h/cc` | Stop word elimination filter |
| `snowball_stem_filter.h/cc` | Snowball stemming filter + GetStemRoot + BuildStemMap |

### Deleted Methods

- `LanguageProcessor::Tokenize()` → replaced by `Process()`
- `LanguageProcessor::StemWordInPlace()` → replaced by `SnowballStemFilter::GetStemRoot()`
- `LanguageProcessor::BuildStemMap()` → moved to `SnowballStemFilter::BuildStemMap()`
- `LanguageProcessor::NormalizeLowerCaseInPlace()` → replaced by `NormalizeCaseFoldFilter::Apply()`
- `LanguageProcessor::IsStopWord()` → replaced by `StopWordFilter::Apply()` returning nullopt
- `LanguageProcessor::IsPunctuation()` → replaced by `Segmenter::IsDelimiter()`
- `LanguageProcessor::SupportsStemming()` → replaced by `FindFilter<SnowballStemFilter>() != nullptr`
