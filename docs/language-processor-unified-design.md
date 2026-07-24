# Unified LanguageProcessor — Low Level Design

## Goals

1. **Shared logic** — Ingestion and query paths use the same normalization, stop-word, and stemming implementations. No duplicated loops.
2. **Composable pipeline** — Adding a processing step (e.g., Arabic diacritic removal, German decompounding) means writing one transform and slotting it in. No touching every factory function.
3. **Pluggable per-language** — Languages that need integrated analysis (CJK morphological analyzers) can replace the entire pipeline without fighting the decomposition.
4. **Query-path primitives** — FilterParser gets the character-level and word-level APIs it needs without calling the full ingestion pipeline.

---

## Design: Layered Architecture

The key insight: the ingestion and query paths share **word-level transforms** (normalize, stop-word, stem) but differ in **how they produce words** from input text.

```
┌─────────────────────────────────────────────────────┐
│              LanguageProcessor (one class)           │
├─────────────────────────────────────────────────────┤
│                                                     │
│  ┌───────────────────────────────────────────────┐  │
│  │  Word Transform Pipeline (shared)             │  │
│  │  NormalizeFn → StopWordFn → StemFn            │  │
│  │  Configured once at FT.CREATE                 │  │
│  └──────────────┬────────────────────────────────┘  │
│                 │                                    │
│     ┌───────────┼────────────┐                      │
│     │           │            │                      │
│  Ingestion   Query term   Query clause              │
│  (Tokenize)  (ProcessWord) (TokenizeQuery)          │
│     │           │            │                      │
│  Produces    Single word   CJK: segment             │
│  words via   already       then run pipeline        │
│  punct-scan  extracted     on each segment          │
│  or segment  by parser                              │
│     │           │            │                      │
│     └───────────┴────────────┘                      │
│         All feed into the same pipeline             │
│                                                     │
├─────────────────────────────────────────────────────┤
│  Character-level primitives (for FilterParser)      │
│  IsPunctuation(cp), IsStopWord(word)                │
└─────────────────────────────────────────────────────┘
```

---

## Core Class

```cpp
// language_processor.h

class LanguageProcessor {
 public:
  // ═══════════════════════════════════════════════════════════════
  // BUILDING BLOCKS — composable transforms (1→1 on a single word)
  // ═══════════════════════════════════════════════════════════════

  // Normalize a word to its canonical index form.
  // Includes NFC, case-fold, and language-specific transforms.
  using NormalizeFn = std::function<void(std::string& word)>;

  // Check if a normalized word is a stop word.
  using IsStopWordFn = std::function<bool(absl::string_view word)>;

  // Stem a word in place. Returns whether the word was modified.
  using StemFn = std::function<bool(std::string& word, uint32_t min_stem_size)>;

  // Check if a code point is a word boundary.
  using IsPunctuationFn = std::function<bool(uint32_t cp)>;

  // ═══════════════════════════════════════════════════════════════
  // SEGMENTATION — language-specific word extraction (1→N)
  // ═══════════════════════════════════════════════════════════════

  // Segment a text chunk into sub-tokens. For languages that need it
  // (CJK, compound-splitting). Null for whitespace-delimited languages.
  using SegmentFn = std::function<std::vector<std::string>(
      absl::string_view text)>;

  // ═══════════════════════════════════════════════════════════════
  // FULL PIPELINE OVERRIDE — for integrated analyzers
  // ═══════════════════════════════════════════════════════════════

  // Optional: replaces the entire Tokenize() pipeline when a library
  // does segmentation + normalization + morphological analysis in one pass.
  using FullPipelineFn = std::function<absl::StatusOr<std::vector<std::string>>(
      absl::string_view text, bool stemming_enabled,
      uint32_t min_stem_size, InProgressStemMap* stem_map)>;

  // ═══════════════════════════════════════════════════════════════
  // CONSTRUCTION
  // ═══════════════════════════════════════════════════════════════

  static std::shared_ptr<LanguageProcessor> Create(
      data_model::Language language,
      const std::string& punctuation,
      const std::vector<std::string>& stop_words);

  // ═══════════════════════════════════════════════════════════════
  // PUBLIC API — Ingestion Path
  // ═══════════════════════════════════════════════════════════════

  // Full document tokenization: text → tokens + stem map.
  // Uses either the decomposed pipeline or the full override.
  absl::StatusOr<std::vector<std::string>> Tokenize(
      absl::string_view text, bool stemming_enabled,
      uint32_t min_stem_size, InProgressStemMap* stem_map) const;

  // ═══════════════════════════════════════════════════════════════
  // PUBLIC API — Query Path (shared word transforms)
  // ═══════════════════════════════════════════════════════════════

  // Process a single word through the shared pipeline.
  // Called by FilterParser after it extracts a token from query syntax.
  // Applies: normalize → stop-word check.
  // Returns false if the word is a stop word (caller should skip it).
  bool ProcessWord(std::string& word) const {
    normalize_fn_(word);
    return !is_stop_word_fn_(word);
  }

  // Stem a single word in place. Called by query expansion
  // (GetAllStemVariants) separately from ProcessWord because stemming
  // at query time is a lookup operation, not part of token extraction.
  void StemWordInPlace(std::string& word, uint32_t min_stem_size = 0) const {
    if (stem_fn_) stem_fn_(word, min_stem_size);
  }

  // Segment a query clause into searchable terms, each run through
  // the shared pipeline. For Snowball languages: punct-split + normalize.
  // For CJK: calls the segmenter then normalizes each segment.
  std::vector<std::string> TokenizeQuery(absl::string_view clause) const;

  // ═══════════════════════════════════════════════════════════════
  // PUBLIC API — Character-level primitives (for FilterParser)
  // ═══════════════════════════════════════════════════════════════

  // Is this code point a word boundary? Used by FilterParser for
  // character-level scanning of query expressions.
  bool IsPunctuation(uint32_t cp) const {
    return is_punctuation_fn_(cp);
  }

  // Is this normalized word a stop word? Exposed for FilterParser's
  // direct use when it needs the check without full ProcessWord.
  bool IsStopWord(absl::string_view word) const {
    return is_stop_word_fn_(word);
  }

  // Normalize without stop-word check. Exposed for callers that need
  // normalization alone (e.g., tag comparison, debug commands).
  void NormalizeLowerCaseInPlace(std::string& word) const {
    normalize_fn_(word);
  }

  // ═══════════════════════════════════════════════════════════════
  // METADATA
  // ═══════════════════════════════════════════════════════════════

  bool SupportsStemming() const { return stem_fn_ != nullptr; }

 private:
  LanguageProcessor() = default;

  // --- Shared building blocks (used by both paths) ---
  NormalizeFn normalize_fn_;
  IsStopWordFn is_stop_word_fn_;
  StemFn stem_fn_;                 // null for non-stemming languages
  IsPunctuationFn is_punctuation_fn_;

  // --- Segmentation (null for whitespace-delimited languages) ---
  SegmentFn segment_fn_;

  // --- Full pipeline override (null for decomposed languages) ---
  FullPipelineFn full_pipeline_fn_;

  // Allow factory functions to set private members
  friend std::shared_ptr<LanguageProcessor> CreateSnowballProcessor(
      data_model::Language, const std::string&, const std::vector<std::string>&);
  friend std::shared_ptr<LanguageProcessor> CreateChineseProcessor(
      const std::string&, const std::vector<std::string>&);
  friend std::shared_ptr<LanguageProcessor> CreateJapaneseProcessor(
      const std::string&, const std::vector<std::string>&);
};
```

---

## The Shared Pipeline: `Tokenize()` and `TokenizeQuery()`

The ingestion path and the query-clause path **share the same word-processing logic**. The only difference is how words are extracted from text.

```cpp
// language_processor.cc

absl::StatusOr<std::vector<std::string>> LanguageProcessor::Tokenize(
    absl::string_view text, bool stemming_enabled,
    uint32_t min_stem_size, InProgressStemMap* stem_map) const {

  // Full override: integrated analyzer owns everything
  if (full_pipeline_fn_) {
    return full_pipeline_fn_(text, stemming_enabled, min_stem_size, stem_map);
  }

  if (!IsValidUtf8(text)) {
    return absl::InvalidArgumentError("Invalid UTF-8");
  }

  std::vector<std::string> tokens;
  std::string word;
  word.reserve(64);

  // Punct-scan loop: extract chunks between punctuation
  size_t pos = 0;
  while (pos < text.size()) {
    // Skip punctuation codepoints...
    // Build word until next punctuation (with escape handling)...

    if (!word.empty()) {
      if (segment_fn_) {
        // Language needs sub-word segmentation (CJK, decompounding)
        for (auto& segment : segment_fn_(word)) {
          ProcessAndEmit(segment, stemming_enabled, min_stem_size,
                         stem_map, tokens);
        }
      } else {
        // Word IS the token (English, French, etc.)
        ProcessAndEmit(word, stemming_enabled, min_stem_size,
                       stem_map, tokens);
      }
      word.clear();
    }
  }
  return tokens;
}

// Shared helper: ProcessWord (normalize + stop-word) → stem → emit
void LanguageProcessor::ProcessAndEmit(
    std::string& word, bool stemming_enabled, uint32_t min_stem_size,
    InProgressStemMap* stem_map, std::vector<std::string>& tokens) const {

  // Reuse the same normalize + stop-word logic as the query path
  if (!ProcessWord(word)) return;

  if (stemming_enabled && stem_fn_ && stem_map) {
    std::string stemmed = word;
    if (stem_fn_(stemmed, min_stem_size)) {
      // Word was modified by stemming — record the mapping
      auto& variants = (*stem_map)[stemmed];
      if (std::find(variants.begin(), variants.end(), word) == variants.end()) {
        variants.push_back(word);
      }
    }
  }

  tokens.push_back(std::move(word));
}


std::vector<std::string> LanguageProcessor::TokenizeQuery(
    absl::string_view clause) const {

  std::vector<std::string> terms;

  if (segment_fn_) {
    // CJK: segment the clause, then run each through shared pipeline
    for (auto& segment : segment_fn_(clause)) {
      // Reuse the same normalize + stop-word logic
      if (ProcessWord(segment)) {
        terms.push_back(std::move(segment));
      }
    }
  } else {
    // Whitespace-delimited: punct-split the clause, normalize each
    std::string word;
    size_t pos = 0;
    while (pos < clause.size()) {
      // Skip punctuation, build word...
      if (!word.empty()) {
        if (ProcessWord(word)) {
          terms.push_back(std::move(word));
        }
        word.clear();
      }
    }
  }
  return terms;
}
```

### Where sharing happens

| Operation | Ingestion (`Tokenize`) | Query term (`ProcessWord`) | Query clause (`TokenizeQuery`) |
|-----------|----------------------|--------------------------|-------------------------------|
| Normalize | ✅ via `ProcessAndEmit` → `ProcessWord` | ✅ directly | ✅ via `ProcessWord` |
| Stop-word | ✅ via `ProcessAndEmit` → `ProcessWord` | ✅ directly | ✅ via `ProcessWord` |
| Stemming | ✅ via `ProcessAndEmit` + stem map | ✅ via `StemWordInPlace` (separate call) | ❌ (done later at eval time) |
| Segmentation | ✅ via `segment_fn_` | N/A (parser extracts words) | ✅ via `segment_fn_` |
| Punct-split | ✅ inline loop | N/A (parser handles syntax) | ✅ inline loop |

The normalize and stop-word logic is **literally the same function objects** on all three paths. `ProcessAndEmit` calls `ProcessWord` internally — zero duplication, one code path.

---

## How FilterParser Uses It

```cpp
// filter_parser.cc — BEFORE (current code)
const auto& lexer = text_index_schema->GetLexer();
if (lexer.IsPunctuation(pk.cp)) break;
lexer.NormalizeLowerCaseInPlace(processed_content);
if (lexer.IsStopWord(processed_content)) { /* skip */ }

// filter_parser.cc — AFTER
const auto& proc = text_index_schema->GetProcessor();
if (proc.IsPunctuation(pk.cp)) break;
// ... parser finishes extracting the token ...
if (!proc.ProcessWord(processed_content)) { /* stop word, skip */ }
// ProcessWord did normalize + stop-word in one call
```

For CJK exact phrases where the parser has extracted `"我喜欢编程"` from quotes:
```cpp
// Parser extracted the clause from syntax — now segment it
auto terms = proc.TokenizeQuery("我喜欢编程");
// terms = ["喜欢", "编程"] (normalized, stop-words removed)
// Build predicate tree from terms...
```

---

## Factory Functions: Composable Configuration

### Snowball (English, French, German, Arabic, etc.)

```cpp
std::shared_ptr<LanguageProcessor> CreateSnowballProcessor(
    data_model::Language language,
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());

  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);

  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) {
    return punct_set.Contains(cp);
  };

  proc->normalize_fn_ = [](std::string& word) {
    NormalizeLowerCaseInPlace(word);  // NFC + case-fold
  };

  proc->is_stop_word_fn_ = [stop_set](absl::string_view word) {
    return stop_set.contains(word);
  };

  proc->stem_fn_ = [language](std::string& word, uint32_t min_stem_size) -> bool {
    sb_stemmer* stemmer = GetThreadLocalStemmer(language);
    auto stemmed = DoStemming(word, stemmer, min_stem_size);
    if (stemmed != word) {
      word.assign(stemmed);
      return true;
    }
    return false;
  };

  proc->segment_fn_ = nullptr;        // no sub-word segmentation
  proc->full_pipeline_fn_ = nullptr;  // use decomposed pipeline

  return proc;
}
```

### Arabic (Snowball + diacritic stripping)

```cpp
std::shared_ptr<LanguageProcessor> CreateArabicProcessor(
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());

  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);

  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) {
    return punct_set.Contains(cp);
  };

  // Arabic-specific: normalize includes diacritic removal
  proc->normalize_fn_ = [](std::string& word) {
    StripArabicDiacritics(word);
    NormalizeAlef(word);
    NormalizeLowerCaseInPlace(word);
  };

  proc->is_stop_word_fn_ = [stop_set](absl::string_view word) {
    return stop_set.contains(word);
  };

  proc->stem_fn_ = [](std::string& word, uint32_t min_stem_size) -> bool {
    sb_stemmer* stemmer = GetThreadLocalStemmer(data_model::ARABIC);
    auto stemmed = DoStemming(word, stemmer, min_stem_size);
    if (stemmed != word) { word.assign(stemmed); return true; }
    return false;
  };

  proc->segment_fn_ = nullptr;
  proc->full_pipeline_fn_ = nullptr;

  return proc;
}
```

Both ingestion AND query paths automatically get diacritic stripping because they share `normalize_fn_`. No separate logic.

### Chinese (Jieba segmentation, no stemming)

```cpp
std::shared_ptr<LanguageProcessor> CreateChineseProcessor(
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());

  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);
  auto jieba = GetJiebaInstance();

  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) {
    return punct_set.Contains(cp);
  };

  proc->normalize_fn_ = [](std::string& word) {
    NormalizeLowerCaseInPlace(word);
  };

  proc->is_stop_word_fn_ = [stop_set](absl::string_view word) {
    return stop_set.contains(word);
  };

  proc->stem_fn_ = nullptr;  // Chinese doesn't stem

  // Segmentation: jieba handles word breaking
  proc->segment_fn_ = [jieba](absl::string_view text) -> std::vector<std::string> {
    return jieba->Cut(text);
  };

  proc->full_pipeline_fn_ = nullptr;  // decomposed pipeline works fine

  return proc;
}
```

`TokenizeQuery("我喜欢编程")` automatically calls `segment_fn_` then `ProcessWord` on each segment — no custom query logic needed.

### Japanese (MeCab — integrated analyzer override)

```cpp
std::shared_ptr<LanguageProcessor> CreateJapaneseProcessor(
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());

  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);
  auto mecab = TryLoadMeCab();

  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) {
    return punct_set.Contains(cp);
  };

  proc->normalize_fn_ = [](std::string& word) {
    NormalizeLowerCaseInPlace(word);
  };

  proc->is_stop_word_fn_ = [stop_set](absl::string_view word) {
    return stop_set.contains(word);
  };

  if (mecab) {
    // MeCab produces base forms — expose as stemming for query expansion
    proc->stem_fn_ = [mecab](std::string& word, uint32_t) -> bool {
      auto result = mecab->AnalyzeSingle(word);
      if (!result.base_form.empty() && result.base_form != word) {
        word = result.base_form;
        return true;
      }
      return false;
    };

    // Segmentation via MeCab (surface forms)
    proc->segment_fn_ = [mecab](absl::string_view text) -> std::vector<std::string> {
      std::vector<std::string> segments;
      for (auto& m : mecab->Analyze(text)) {
        segments.push_back(std::move(m.surface));
      }
      return segments;
    };

    // Full pipeline override: MeCab does segment + lemmatize in one pass,
    // and we need to populate stem_map correctly from morpheme analysis.
    proc->full_pipeline_fn_ = [mecab, stop_set, &normalize = proc->normalize_fn_](
        absl::string_view text, bool stemming_enabled,
        uint32_t min_stem_size, InProgressStemMap* stem_map)
        -> absl::StatusOr<std::vector<std::string>> {

      if (!IsValidUtf8(text)) return absl::InvalidArgumentError("Invalid UTF-8");

      auto morphemes = mecab->Analyze(text);
      std::vector<std::string> tokens;
      for (auto& m : morphemes) {
        normalize(m.surface);
        if (stop_set.contains(m.surface)) continue;

        if (stemming_enabled && stem_map &&
            !m.base_form.empty() && m.base_form != m.surface) {
          auto& variants = (*stem_map)[m.base_form];
          if (std::find(variants.begin(), variants.end(), m.surface) ==
              variants.end()) {
            variants.push_back(m.surface);
          }
        }
        tokens.push_back(std::move(m.surface));
      }
      return tokens;
    };
  } else {
    // Fallback: ICU BreakIterator, no stemming
    auto breaker = GetICUBreaker("ja");
    proc->stem_fn_ = nullptr;

    proc->segment_fn_ = [breaker](absl::string_view text) -> std::vector<std::string> {
      return ICUBreak(breaker, text);
    };

    proc->full_pipeline_fn_ = nullptr;  // decomposed pipeline with ICU segments
  }

  return proc;
}
```

Note: Japanese uses `full_pipeline_fn_` for ingestion (because MeCab produces base forms alongside segments, and splitting that into two calls would duplicate work), but `TokenizeQuery()` still uses `segment_fn_` + `ProcessWord` for query clauses. The `segment_fn_` is set regardless — it's used by `TokenizeQuery()` even when `full_pipeline_fn_` handles ingestion.

### German (decompounding as segmentation)

```cpp
std::shared_ptr<LanguageProcessor> CreateGermanProcessor(
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());

  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);
  auto decompounder = GetDecompounder("de");

  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) {
    return punct_set.Contains(cp);
  };

  proc->normalize_fn_ = [](std::string& word) {
    NormalizeLowerCaseInPlace(word);
  };

  proc->is_stop_word_fn_ = [stop_set](absl::string_view word) {
    return stop_set.contains(word);
  };

  proc->stem_fn_ = [](std::string& word, uint32_t min_stem_size) -> bool {
    sb_stemmer* stemmer = GetThreadLocalStemmer(data_model::GERMAN);
    auto stemmed = DoStemming(word, stemmer, min_stem_size);
    if (stemmed != word) { word.assign(stemmed); return true; }
    return false;
  };

  // Decompounding: split compound words into parts
  proc->segment_fn_ = [decompounder](absl::string_view text)
      -> std::vector<std::string> {
    return decompounder->Split(text);
  };

  proc->full_pipeline_fn_ = nullptr;

  return proc;
}
```

German gets decompounding for free on both ingestion AND query paths because `segment_fn_` is used by both `Tokenize()` and `TokenizeQuery()`.

---

## Adding a New Language

```cpp
// Hindi — ICU segmentation, no stemming, no special normalization
std::shared_ptr<LanguageProcessor> CreateHindiProcessor(
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());
  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);
  auto breaker = GetICUBreaker("hi");

  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) { return punct_set.Contains(cp); };
  proc->normalize_fn_ = [](std::string& w) { NormalizeLowerCaseInPlace(w); };
  proc->is_stop_word_fn_ = [stop_set](absl::string_view w) { return stop_set.contains(w); };
  proc->stem_fn_ = nullptr;
  proc->segment_fn_ = [breaker](absl::string_view t) { return ICUBreak(breaker, t); };
  proc->full_pipeline_fn_ = nullptr;

  return proc;
}
```

10 lines. Gets correct behavior on ingestion, query terms, query clauses, prefix search, fuzzy search — all automatically.

---

## Integration with FilterParser

```cpp
// Current code (with Lexer):
const auto& lexer = text_index_schema->GetLexer();
// ... scanning loop ...
if (lexer.IsPunctuation(pk.cp)) break;
// ... after extracting token ...
lexer.NormalizeLowerCaseInPlace(processed_content);
if (lexer.IsStopWord(processed_content)) { /* skip */ }

// New code (with LanguageProcessor):
const auto& proc = text_index_schema->GetProcessor();
// ... scanning loop (unchanged) ...
if (proc.IsPunctuation(pk.cp)) break;
// ... after extracting token ...
if (!proc.ProcessWord(processed_content)) { /* stop word, skip */ }
// One call instead of two — same effect, guaranteed consistency

// For CJK exact phrases:
// Parser extracts raw clause from syntax, then:
auto terms = proc.TokenizeQuery(raw_clause);
for (auto& term : terms) {
  // Build TermPredicate for each...
}
```

---

## Design Decision Matrix

| Design choice | Rationale |
|---------------|-----------|
| `NormalizeFn` as a shared building block | Both paths need identical normalization. One implementation, captured once. |
| `ProcessWord()` combines normalize + stop-word | FilterParser's most common operation. Eliminates forgetting to call one. |
| `segment_fn_` separate from full pipeline | `TokenizeQuery()` needs segmentation even when `full_pipeline_fn_` handles ingestion. |
| `full_pipeline_fn_` as escape hatch | MeCab/integrated analyzers can't be decomposed without duplicating work. But only for ingestion — query still uses `segment_fn_` + `ProcessWord`. |
| `StemWordInPlace` as separate API | Stemming at query time is for expansion (GetAllStemVariants), not for token extraction. Called by evaluator, not parser. |
| `IsPunctuation` exposed directly | FilterParser needs per-codepoint checks during its own scanning loop. Can't be hidden behind `ProcessWord`. |
| No `TokenizeFn` typedef | The ingestion logic lives in `Tokenize()` itself (with decomposed or override path). Not externally configurable — the factory functions configure the *building blocks*. |

---

## What's Shared vs. What's Separate

```
SHARED (same function objects on all paths):
  normalize_fn_      — identical normalization everywhere
  is_stop_word_fn_   — identical stop-word set everywhere
  stem_fn_           — identical stemming algorithm everywhere
  is_punctuation_fn_ — identical boundary detection everywhere
  segment_fn_        — identical segmentation everywhere

INGESTION ONLY:
  full_pipeline_fn_  — override for integrated analyzers
  stem_map update    — only ingestion populates the stem tree

QUERY ONLY:
  ProcessWord()      — convenience wrapper (normalize + stop-word)
  TokenizeQuery()    — segments a clause for CJK, splits on punct for others
  StemWordInPlace()  — used by GetAllStemVariants at evaluation time
```

---

## Performance

| Path | Cost |
|------|------|
| Ingestion (decomposed) | One `std::function` call per word for normalize, one for stop-word, one for stem. Same as current Lexer's inline calls. |
| Ingestion (full override) | One `std::function` call total. Better than decomposed for integrated analyzers. |
| Query (ProcessWord) | One `std::function` call for normalize + one for stop-word. Same as current `NormalizeLowerCaseInPlace` + `IsStopWord`. |
| Query (TokenizeQuery) | One `segment_fn_` call + ProcessWord per segment. No allocation difference from current. |
| FilterParser scanning | One `is_punctuation_fn_` call per codepoint. Same as current `IsPunctuation`. |

No new overhead vs. current design. Branch prediction sees constant targets for the index's lifetime.

---

## File Structure

```
src/indexes/text/
  language_processor.h       — LanguageProcessor class definition
  language_processor.cc      — Tokenize(), TokenizeQuery(), ProcessAndEmit()
  language_registry.cc       — CreateSnowballProcessor(), CreateChineseProcessor(), etc.
  snowball_stemmer.h/cc      — Thread-local stemmer cache (GetThreadLocalStemmer, DoStemming)
  unicode_normalizer.h/cc    — NormalizeLowerCaseInPlace (shared utility)
```

### What Dies

| Deleted | Replaced by |
|---------|-------------|
| `lexer.h` / `lexer.cc` | `LanguageProcessor` (Tokenize + ProcessWord + primitives) |
| `snowball_processor.h/cc` | `CreateSnowballProcessor()` in `language_registry.cc` |
| `language_processor.h` (abstract) | `language_processor.h` (concrete, one class) |

---

## Summary

| Property | Value |
|----------|-------|
| Classes | ONE: `LanguageProcessor` (concrete, no hierarchy) |
| Shared logic | `normalize_fn_`, `is_stop_word_fn_`, `stem_fn_` used by ALL paths |
| Ingestion API | `Tokenize()` — decomposed pipeline or full override |
| Query APIs | `ProcessWord()`, `TokenizeQuery()`, `StemWordInPlace()`, `IsPunctuation()`, `IsStopWord()` |
| Composability | Configure building blocks independently per language; `segment_fn_` for 1→N; `full_pipeline_fn_` for integrated analyzers |
| Adding a language | Set the building blocks you need (10-30 lines) |
| Per-token branching | ZERO at runtime — all captured at FT.CREATE |
| Duplication between paths | ZERO — same function objects serve both |
