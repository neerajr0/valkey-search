# Multi-Language LanguageProcessor — Low Level Design

## Problem Statement

The current architecture splits text processing between two classes:
- **`Lexer`** — owns tokenization (punct split, normalize, stop words, stem map)
- **`LanguageProcessor`** (abstract) / **`SnowballProcessor`** — owns stemming

This split is structurally broken for multi-language support:

1. **Lexer's tokenization assumes word delimiters.** Chinese, Japanese, Korean, Thai have no whitespace between words. The Lexer's "scan until punctuation" algorithm produces one giant token for an entire sentence. This is unfixable without replacing the algorithm entirely.

2. **Stemming is hardcoded to Snowball.** For Japanese/Korean, morphological analysis produces both segmentation AND base forms in a single integrated pass. There's no separate "stem this isolated word" step.

3. **Split-brain for new languages.** Adding Chinese would require either hacking jieba into the Lexer (polluting it with conditionals) or routing around it (creating two tokenization paths for callers to choose between).

4. **Query path is orphaned.** The Lexer exposes `IsPunctuation()`, `IsStopWord()`, `NormalizeLowerCaseInPlace()` for the `FilterParser` to parse structured queries. Any replacement must preserve these primitives.

---

## Design Philosophy

> **Configure once at `FT.CREATE`. Run the fixed configuration millions of times without any per-token decision logic.**

- One concrete class, no hierarchy
- No virtual methods
- No factory pattern / class-per-language
- All language decisions captured in closures at construction time
- Same object serves both ingestion (document → tokens) and query (structured query → search terms)

---

## Core Class: `LanguageProcessor`

```cpp
// language_processor.h — ONE concrete class, no hierarchy

class LanguageProcessor {
 public:
  // === Function types for configurable behavior ===

  // Full document tokenization: text → tokens + optional stem map.
  // Used by ingestion (StageAttributeData) and FT.TAGVALS.
  using TokenizeFn = std::function<absl::StatusOr<std::vector<std::string>>(
      absl::string_view text, bool stemming_enabled,
      uint32_t min_stem_size, InProgressStemMap* stem_map)>;

  // Query clause tokenization: extract search terms from a parsed query clause.
  // Different from TokenizeFn: no stem map, input is already extracted from
  // query syntax (not raw document text), handles CJK segmentation at query time.
  using TokenizeQueryFn = std::function<std::vector<std::string>(
      absl::string_view query_clause)>;

  // Stem a single word in place. Used by query expansion (GetAllStemVariants)
  // and key deletion path. Null for non-stemming languages.
  using StemWordFn = std::function<void(std::string& word, uint32_t min_stem_size)>;

  // Normalize a word to its canonical index form. Used by query parser to
  // ensure query terms match what was indexed. Includes NFC normalization,
  // case folding, and language-specific transforms (e.g., Arabic diacritic removal).
  using NormalizeFn = std::function<void(std::string& word)>;

  // Check if a Unicode code point is a word boundary for this language.
  // Used by query parser for character-level scanning.
  using IsPunctuationFn = std::function<bool(uint32_t cp)>;

  // Check if a normalized word is a stop word. Used by query parser.
  using IsStopWordFn = std::function<bool(absl::string_view word)>;

  // === Construction ===

  // Single factory: language + configuration → configured processor.
  // Called once at FT.CREATE time. The returned processor lives as long as the index.
  static std::shared_ptr<LanguageProcessor> Create(
      data_model::Language language,
      const std::string& punctuation,
      const std::vector<std::string>& stop_words);

  // === Public API — Ingestion Path ===

  // Tokenize document text during HSET/indexing.
  // Produces normalized tokens and populates stem map.
  absl::StatusOr<std::vector<std::string>> Tokenize(
      absl::string_view text, bool stemming_enabled,
      uint32_t min_stem_size, InProgressStemMap* stem_map) const {
    return tokenize_fn_(text, stemming_enabled, min_stem_size, stem_map);
  }

  // === Public API — Query Path ===

  // Tokenize a query clause into search terms.
  // For English: normalize + split on punct (what FilterParser does today inline).
  // For CJK: calls jieba/MeCab/kiwi on the query clause.
  std::vector<std::string> TokenizeQuery(absl::string_view query_clause) const {
    return tokenize_query_fn_(query_clause);
  }

  // Stem a word for query expansion (GetAllStemVariants).
  // No-op for languages without stemming.
  void StemWordInPlace(std::string& word, uint32_t min_stem_size = 0) const {
    if (stem_word_fn_) stem_word_fn_(word, min_stem_size);
  }

  // Normalize a word to match indexed form. Called by FilterParser.
  void NormalizeLowerCaseInPlace(std::string& word) const {
    normalize_fn_(word);
  }

  // Is this code point a word boundary? Called by FilterParser for
  // character-level query scanning.
  bool IsPunctuation(uint32_t cp) const {
    return is_punctuation_fn_(cp);
  }

  // Is this normalized word a stop word? Called by FilterParser.
  bool IsStopWord(absl::string_view word) const {
    return is_stop_word_fn_(word);
  }

  // === Metadata ===

  // Whether this language supports stemming (controls stem tree construction).
  bool SupportsStemming() const { return supports_stemming_; }

 private:
  LanguageProcessor() = default;
  friend std::shared_ptr<LanguageProcessor> CreateSnowballProcessor(
      data_model::Language, const std::string&, const std::vector<std::string>&);
  friend std::shared_ptr<LanguageProcessor> CreateICUProcessor(
      data_model::Language, const std::string&, const std::vector<std::string>&);
  friend std::shared_ptr<LanguageProcessor> CreateChineseProcessor(
      const std::string&, const std::vector<std::string>&);
  friend std::shared_ptr<LanguageProcessor> CreateKoreanProcessor(
      const std::string&, const std::vector<std::string>&);
  friend std::shared_ptr<LanguageProcessor> CreateJapaneseProcessor(
      const std::string&, const std::vector<std::string>&);

  TokenizeFn tokenize_fn_;
  TokenizeQueryFn tokenize_query_fn_;
  StemWordFn stem_word_fn_;          // null for non-stemming languages
  NormalizeFn normalize_fn_;
  IsPunctuationFn is_punctuation_fn_;
  IsStopWordFn is_stop_word_fn_;
  bool supports_stemming_ = false;
};
```

---

## Why This Shape

### Why `TokenizeFn` is the full pipeline (not segment + stem)

The previous design decomposed tokenization into `SegmentFn` + `StemFn`. This has two problems:

1. **Performance:** For Snowball languages, "segmentation" IS the punct-scan loop — forcing it into a function that returns `vector<string_view>` adds a heap allocation per chunk that doesn't exist today.

2. **Semantic mismatch:** For Japanese/Korean, the morphological analyzer produces BOTH segments and base forms in one pass. Decomposing into separate segment/stem steps either duplicates work or forces an artificial separation.

With `TokenizeFn` capturing the full pipeline, each language implements its natural algorithm:
- English: inline loop (same as current Lexer::Tokenize)
- Chinese: `jieba->Cut()` + normalize + stop words
- Japanese: `mecab->Analyze()` → segments with base forms already produced

### Why separate `TokenizeQuery` from `Tokenize`

`Tokenize()` is for documents — it handles stem maps, full UTF-8 validation on potentially large text, etc.

`TokenizeQuery()` is for query clauses — short strings already extracted from query syntax by the FilterParser. It:
- Doesn't produce stem maps (query expansion uses `StemWordInPlace` separately)
- Doesn't need the stem-enabled flag (controlled by the caller)
- Is critical for CJK where the parser can't character-scan for word boundaries

### Why `IsPunctuation`, `IsStopWord`, `NormalizeLowerCaseInPlace` are separate

The `FilterParser` parses structured query syntax character-by-character. It needs these primitives independently — it can't call `Tokenize()` on `@title:hello|world` because that would ignore query operators.

---

## Language Registration: Factory Functions

Each language family is a function that configures the same class differently.

### Snowball Languages (English, French, German, Arabic, etc.)

```cpp
// language_registry.cc

std::shared_ptr<LanguageProcessor> CreateSnowballProcessor(
    data_model::Language language,
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());

  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);

  proc->supports_stemming_ = true;

  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) {
    return punct_set.Contains(cp);
  };

  proc->is_stop_word_fn_ = [stop_set](absl::string_view word) {
    return stop_set.contains(word);
  };

  proc->normalize_fn_ = [](std::string& word) {
    NormalizeLowerCaseInPlace(word);  // shared utility
  };

  proc->stem_word_fn_ = [language](std::string& word, uint32_t min_stem_size) {
    sb_stemmer* stemmer = GetThreadLocalStemmer(language);
    auto stemmed = DoStemming(word, stemmer, min_stem_size);
    if (stemmed != word) word.assign(stemmed);
  };

  proc->tokenize_fn_ = [punct_set, stop_set, language](
      absl::string_view text, bool stemming_enabled,
      uint32_t min_stem_size, InProgressStemMap* stem_map)
      -> absl::StatusOr<std::vector<std::string>> {

    if (!IsValidUtf8(text)) return absl::InvalidArgumentError("Invalid UTF-8");

    sb_stemmer* stemmer = stemming_enabled ?
        GetThreadLocalStemmer(language) : nullptr;
    std::vector<std::string> tokens;
    std::string word;
    word.reserve(64);

    // Punct-scan tokenization loop (same algorithm as current Lexer::Tokenize)
    size_t pos = 0;
    while (pos < text.size()) {
      // Skip punctuation...
      // Build word...
      // Escape handling...
      if (!word.empty()) {
        NormalizeLowerCaseInPlace(word);
        if (stop_set.contains(word)) { word.clear(); continue; }
        if (stemming_enabled) {
          UpdateStemMap(word, stemmer, min_stem_size, *stem_map);
        }
        tokens.push_back(std::move(word));
        word.clear();
      }
    }
    return tokens;
  };

  proc->tokenize_query_fn_ = [punct_set, stop_set](
      absl::string_view query_clause) -> std::vector<std::string> {
    // For Snowball: normalize + split on punctuation
    // Same logic FilterParser uses today, but encapsulated
    std::vector<std::string> terms;
    std::string word;
    size_t pos = 0;
    while (pos < query_clause.size()) {
      // Skip punct, build word, normalize
      if (!word.empty()) {
        NormalizeLowerCaseInPlace(word);
        if (!stop_set.contains(word)) {
          terms.push_back(std::move(word));
        }
        word.clear();
      }
    }
    return terms;
  };

  return proc;
}
```

### Chinese (Jieba)

```cpp
std::shared_ptr<LanguageProcessor> CreateChineseProcessor(
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());

  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);
  auto jieba = GetJiebaInstance();  // shared singleton

  proc->supports_stemming_ = false;
  proc->stem_word_fn_ = nullptr;  // no stemming for Chinese

  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) {
    return punct_set.Contains(cp);
  };

  proc->is_stop_word_fn_ = [stop_set](absl::string_view word) {
    return stop_set.contains(word);
  };

  proc->normalize_fn_ = [](std::string& word) {
    NormalizeLowerCaseInPlace(word);
  };

  proc->tokenize_fn_ = [jieba, stop_set](
      absl::string_view text, bool /*stemming_enabled*/,
      uint32_t /*min_stem_size*/, InProgressStemMap* /*stem_map*/)
      -> absl::StatusOr<std::vector<std::string>> {

    if (!IsValidUtf8(text)) return absl::InvalidArgumentError("Invalid UTF-8");

    std::vector<std::string> tokens;
    for (auto& segment : jieba->Cut(text)) {
      NormalizeLowerCaseInPlace(segment);
      if (stop_set.contains(segment)) continue;
      if (segment.empty()) continue;
      tokens.push_back(std::move(segment));
    }
    return tokens;
  };

  proc->tokenize_query_fn_ = [jieba, stop_set](
      absl::string_view query_clause) -> std::vector<std::string> {
    // CJK: segment the query clause with jieba
    std::vector<std::string> terms;
    for (auto& segment : jieba->CutForSearch(query_clause)) {
      NormalizeLowerCaseInPlace(segment);
      if (!stop_set.contains(segment) && !segment.empty()) {
        terms.push_back(std::move(segment));
      }
    }
    return terms;
  };

  return proc;
}
```

### Japanese (MeCab with ICU fallback)

```cpp
std::shared_ptr<LanguageProcessor> CreateJapaneseProcessor(
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());

  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);
  auto mecab = TryLoadMeCab();  // dlopen, may return nullptr

  // MeCab produces base forms → we expose that as "stemming" for query expansion
  proc->supports_stemming_ = (mecab != nullptr);

  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) {
    return punct_set.Contains(cp);
  };

  proc->is_stop_word_fn_ = [stop_set](absl::string_view word) {
    return stop_set.contains(word);
  };

  proc->normalize_fn_ = [](std::string& word) {
    NormalizeLowerCaseInPlace(word);
  };

  if (mecab) {
    // MeCab available: morphological analysis does segment + lemmatize in one pass
    proc->stem_word_fn_ = [mecab](std::string& word, uint32_t) {
      auto result = mecab->AnalyzeSingle(word);
      if (!result.base_form.empty() && result.base_form != word) {
        word = result.base_form;
      }
    };

    proc->tokenize_fn_ = [mecab, stop_set](
        absl::string_view text, bool stemming_enabled,
        uint32_t min_stem_size, InProgressStemMap* stem_map)
        -> absl::StatusOr<std::vector<std::string>> {

      if (!IsValidUtf8(text)) return absl::InvalidArgumentError("Invalid UTF-8");

      auto morphemes = mecab->Analyze(text);
      std::vector<std::string> tokens;
      for (auto& m : morphemes) {
        NormalizeLowerCaseInPlace(m.surface);
        if (stop_set.contains(m.surface)) continue;

        if (stemming_enabled && stem_map && m.base_form != m.surface) {
          auto it = stem_map->find(m.base_form);
          if (it == stem_map->end()) {
            it = stem_map->try_emplace(m.base_form).first;
          }
          auto& variants = it->second;
          if (std::find(variants.begin(), variants.end(), m.surface) ==
              variants.end()) {
            variants.emplace_back(m.surface);
          }
        }
        tokens.push_back(std::move(m.surface));
      }
      return tokens;
    };

    proc->tokenize_query_fn_ = [mecab, stop_set](
        absl::string_view query_clause) -> std::vector<std::string> {
      auto morphemes = mecab->Analyze(query_clause);
      std::vector<std::string> terms;
      for (auto& m : morphemes) {
        NormalizeLowerCaseInPlace(m.surface);
        if (!stop_set.contains(m.surface) && !m.surface.empty()) {
          terms.push_back(std::move(m.surface));
        }
      }
      return terms;
    };
  } else {
    // Fallback to ICU BreakIterator
    auto breaker = GetICUBreaker("ja");
    proc->stem_word_fn_ = nullptr;

    proc->tokenize_fn_ = [breaker, stop_set](
        absl::string_view text, bool, uint32_t, InProgressStemMap*)
        -> absl::StatusOr<std::vector<std::string>> {

      if (!IsValidUtf8(text)) return absl::InvalidArgumentError("Invalid UTF-8");

      std::vector<std::string> tokens;
      for (auto& word : ICUBreak(breaker, text)) {
        NormalizeLowerCaseInPlace(word);
        if (stop_set.contains(word)) continue;
        tokens.push_back(std::move(word));
      }
      return tokens;
    };

    proc->tokenize_query_fn_ = [breaker, stop_set](
        absl::string_view query_clause) -> std::vector<std::string> {
      std::vector<std::string> terms;
      for (auto& word : ICUBreak(breaker, query_clause)) {
        NormalizeLowerCaseInPlace(word);
        if (!stop_set.contains(word) && !word.empty()) {
          terms.push_back(std::move(word));
        }
      }
      return terms;
    };
  }

  return proc;
}
```

### Adding a New Language

No new class. No new file. Write a factory function:

```cpp
// Adding Hindi — 15 lines
std::shared_ptr<LanguageProcessor> CreateHindiProcessor(
    const std::string& punctuation,
    const std::vector<std::string>& stop_words) {

  auto proc = std::shared_ptr<LanguageProcessor>(new LanguageProcessor());
  auto punct_set = BuildPunctuationSet(punctuation);
  auto stop_set = BuildStopWordsSet(stop_words);
  auto breaker = GetICUBreaker("hi");

  proc->supports_stemming_ = false;
  proc->stem_word_fn_ = nullptr;
  proc->is_punctuation_fn_ = [punct_set](uint32_t cp) { return punct_set.Contains(cp); };
  proc->is_stop_word_fn_ = [stop_set](absl::string_view w) { return stop_set.contains(w); };
  proc->normalize_fn_ = [](std::string& w) { NormalizeLowerCaseInPlace(w); };

  proc->tokenize_fn_ = [breaker, stop_set](...) -> StatusOr<vector<string>> {
    // ICU BreakIterator for Hindi word boundaries
    // ... normalize, filter stop words ...
  };

  proc->tokenize_query_fn_ = [breaker, stop_set](...) -> vector<string> {
    // Same segmentation for query terms
  };

  return proc;
}
```

---

## Integration: How Callers Change

### TextIndexSchema (text_index.h)

```cpp
class TextIndexSchema {
 public:
  TextIndexSchema(data_model::Language language, const std::string& punctuation,
                  bool with_offsets, const std::vector<std::string>& stop_words,
                  uint32_t min_stem_size);

  // Replaces GetLexer()
  const LanguageProcessor& GetProcessor() const { return *processor_; }

 private:
  std::shared_ptr<LanguageProcessor> processor_;
  // ... rest unchanged ...
};
```

### Ingestion (StageAttributeData)

```cpp
// Before:
auto tokens = lexer_.Tokenize(data, stem, min_stem_size_, stem_mappings_ptr);

// After:
auto tokens = processor_->Tokenize(data, stem, min_stem_size_, stem_mappings_ptr);
```

### Query Expansion (GetAllStemVariants)

```cpp
// Before:
lexer_.StemWordInPlace(stemmed, lexer_.GetStemmer());

// After:
processor_->StemWordInPlace(stemmed);
```

### Query Parser (FilterParser)

```cpp
// Before:
const auto& lexer = text_index_schema->GetLexer();
if (lexer.IsPunctuation(pk.cp)) break;
lexer.NormalizeLowerCaseInPlace(processed_content);
if (lexer.IsStopWord(processed_content)) { ... }

// After:
const auto& proc = text_index_schema->GetProcessor();
if (proc.IsPunctuation(pk.cp)) break;
proc.NormalizeLowerCaseInPlace(processed_content);
if (proc.IsStopWord(processed_content)) { ... }
```

### FT.DEBUG TOKENIZE (textinfocmd.cc)

```cpp
// Before:
auto lexer = index_schema->GetTextIndexSchema()->GetLexer();
auto result = lexer.Tokenize(text, false, 0);

// After:
const auto& proc = index_schema->GetTextIndexSchema()->GetProcessor();
auto result = proc.Tokenize(text, false, 0, nullptr);
```

---

## Runtime Behavior

### Decision Flow

```
FT.CREATE time (once):
  language = CHINESE, punctuation = "...", stop_words = [...]
  → LanguageProcessor::Create(CHINESE, punct, stops)
  → CreateChineseProcessor(punct, stops)
  → LanguageProcessor constructed with:
      tokenize_fn_       = [jieba, stop_set](...) { ... }    ← CAPTURED
      tokenize_query_fn_ = [jieba, stop_set](...) { ... }    ← CAPTURED
      stem_word_fn_      = nullptr                            ← CAPTURED
      normalize_fn_      = [](word) { NormalizeLowerCase(word); }  ← CAPTURED
      is_punctuation_fn_ = [punct_set](cp) { ... }           ← CAPTURED
      is_stop_word_fn_   = [stop_set](word) { ... }          ← CAPTURED
  → Stored in TextIndexSchema for the lifetime of the index

Every HSET (millions of times):
  processor_->Tokenize(text, ...)
    → calls tokenize_fn_ directly (one indirection, constant target)
    → internally calls jieba->Cut(), normalize, filter — no branching

Every FT.SEARCH (millions of times):
  processor_->TokenizeQuery(query_clause)
    → calls tokenize_query_fn_ directly (one indirection, constant target)
  processor_->StemWordInPlace(term)
    → calls stem_word_fn_ directly (or no-op if null)
```

**No `switch(language)` at runtime.** No `if (language == CHINESE)`. The decision was made at `FT.CREATE`, captured in closures, and executed directly thereafter.

---

## Performance Characteristics

| Mechanism | Cost per call | Notes |
|-----------|---------------|-------|
| Virtual dispatch (old design) | ~1 vtable indirection | Per method call |
| `std::function` (this design) | ~1 type-erased indirection | Per method call |
| Per-chunk vector allocation | 0 (Snowball), 1 (CJK) | Previous design forced vector per chunk for all languages |

### Why this is faster than the previous (segment_fn + stem_fn) design

The previous design called `segment_fn_` per punctuation-delimited chunk, allocating `vector<string_view>` each time. For English text with 50 words, that's ~50 vector allocations.

This design's `tokenize_fn_` runs the entire tokenization loop inside one closure — same as the current Lexer does — with a single output vector. The closure captures everything it needs; no intermediate allocations.

For CJK, both designs call the segmenter once on the full text. No difference.

### Branch prediction

The CPU branch predictor sees one constant target per `std::function` callsite for the entire lifetime of the index. Optimal prediction — same as virtual dispatch, same as direct calls after warmup.

---

## File Structure

```
src/indexes/text/
  language_processor.h       — ONE concrete class (LanguageProcessor)
  language_processor.cc      — Create() dispatch + shared utilities
  language_registry.cc       — CreateSnowballProcessor(), CreateChineseProcessor(), etc.
  snowball_stemmer.h/cc      — Thread-local stemmer cache (GetThreadLocalStemmer, DoStemming)
  unicode_normalizer.h/cc    — NormalizeLowerCaseInPlace (shared utility, unchanged)
```

### What Dies

| Deleted | Replaced by |
|---------|-------------|
| `lexer.h` / `lexer.cc` | `LanguageProcessor` with `tokenize_fn_` + query primitives |
| `snowball_processor.h` / `snowball_processor.cc` | `CreateSnowballProcessor()` in `language_registry.cc` |
| `language_processor.h` (abstract base class) | `language_processor.h` (concrete class) |
| `language_processor.cc` (switch-based factory) | `language_processor.cc` + `language_registry.cc` |

---

## Query-Side Design: Why Separate APIs Matter

The `FilterParser` handles structured query syntax: `@field:term`, `term1|term2`, `-negated`, `"exact phrase"`, `prefix*`, `%fuzzy%`, parentheses, etc. It cannot simply call `Tokenize()` on the raw query because that would destroy the syntax.

### What the parser needs from the processor:

| API | Used for | Example |
|-----|----------|---------|
| `IsPunctuation(cp)` | Detecting word boundaries while scanning query | `@title:hello world` → "hello" ends at space |
| `NormalizeLowerCaseInPlace(word)` | Making extracted terms match indexed form | `"Hello"` → `"hello"` |
| `IsStopWord(word)` | Filtering common words from query | `"the"` → skip |
| `StemWordInPlace(word)` | Query expansion against stem tree | `"running"` → `"run"` |
| `TokenizeQuery(clause)` | CJK: segmenting a query clause into searchable terms | `"我喜欢编程"` → `["我", "喜欢", "编程"]` |

### For Snowball languages

The parser continues to work as today — scan characters, break on punctuation, normalize each term. `TokenizeQuery()` is equivalent to what the parser does inline.

### For CJK languages

The parser extracts the raw clause from syntax (e.g., `"我喜欢编程"` from `@body:"我喜欢编程"`), then calls `proc.TokenizeQuery("我喜欢编程")` which segments it via jieba/MeCab into individual searchable terms.

---

## Future Extensions

### Arabic Normalization

Arabic needs diacritic (tashkeel) stripping and alef normalization. This goes into the `normalize_fn_` for Arabic:

```cpp
proc->normalize_fn_ = [](std::string& word) {
  StripArabicDiacritics(word);  // Remove  َ  ُ  ِ  ّ  ً  ٌ  ٍ
  NormalizeAlef(word);          // أ إ آ ٱ → ا
  NormalizeLowerCaseInPlace(word);  // Standard NFC + case fold
};
```

Both ingestion and query paths call `normalize_fn_` — consistency is structural.

### Fuzzy Search Adaptation

CJK may need different edit-distance semantics. Add:

```cpp
using FuzzyExpandFn = std::function<std::vector<std::string>(
    absl::string_view term, uint32_t max_distance, const Rax& tree)>;
```

Not needed now. Add when CJK fuzzy search requirements are defined.

### Synonyms / Dictionary-Based Expansion

Some languages benefit from synonym expansion at query time. Add:

```cpp
using ExpandSynonymsFn = std::function<std::vector<std::string>(
    absl::string_view term)>;
```

Not needed now. The closure-based design makes this trivial to add later — just another captured function pointer.

---

## Summary

| Property | Value |
|----------|-------|
| Classes | ONE: `LanguageProcessor` (concrete, no hierarchy) |
| Construction | Once at `FT.CREATE` via `LanguageProcessor::Create()` |
| Ingestion API | `Tokenize()` — full document → tokens + stem map |
| Query APIs | `TokenizeQuery()`, `StemWordInPlace()`, `NormalizeLowerCaseInPlace()`, `IsPunctuation()`, `IsStopWord()` |
| Per-token branching | ZERO — all decisions captured in closures at construction |
| Adding a language | Write one factory function (15–30 lines) |
| Performance | Same as current Lexer for Snowball; better than segment_fn decomposition |
| Consistency guarantee | Structural — one object serves both ingestion and query |
| Virtual dispatch | None |
| File count | 4–5 files total |
