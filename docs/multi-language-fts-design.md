# Multi-Language Full Text Search — Low Level Design

## Design Philosophy

The internal algorithm varies by language, but the interface is identical.

**Design:** Single `LanguageProcessor` class, configured with function pointers for the variable parts.

- No inheritance
- No virtual methods
- No class hierarchy
- No factory pattern

---

## Core Class: `LanguageProcessor`

```cpp
// language_processor.h — ONE class, not a hierarchy
class LanguageProcessor {
 public:
  // The two things that vary by language:
  using SegmentFn = std::function<std::vector<std::string_view>(absl::string_view chunk)>;
  using StemFn = std::function<std::string_view(absl::string_view word)>;

  LanguageProcessor(SegmentFn segment, StemFn stem,
                    const std::string& punctuation,
                    const std::vector<std::string>& stop_words);

  // THE single entry point — replaces both Lexer and old Processor
  absl::StatusOr<std::vector<std::string>> Tokenize(
      absl::string_view text, bool stemming_enabled,
      uint32_t min_stem_size, InProgressStemMap* stem_map) const;

 private:
  SegmentFn segment_fn_;     // library-specific: jieba.Cut / kiwi.Analyze / identity
  StemFn stem_fn_;           // library-specific: sb_stemmer_stem / identity
  PunctuationSet punct_set_; // shared
  absl::flat_hash_set<std::string> stop_words_set_;  // shared
};
```

---

## Language Registration: Factory Functions

Each language is plugged in via a simple creation function that configures the same class differently.

```cpp
// language_processor_registry.cc — just functions that create configured instances

std::unique_ptr<LanguageProcessor> CreateEnglishProcessor(
    const std::string& punct, const std::vector<std::string>& stops) {
  auto stemmer = GetThreadLocalStemmer("english");
  return std::make_unique<LanguageProcessor>(
      /*segment=*/ [](string_view chunk) { return vector{chunk}; },  // identity
      /*stem=*/ [stemmer](string_view word) { return snowball_stem(stemmer, word); },
      punct, stops);
}

std::unique_ptr<LanguageProcessor> CreateChineseProcessor(
    const std::string& punct, const std::vector<std::string>& stops) {
  auto jieba = GetJiebaInstance();
  return std::make_unique<LanguageProcessor>(
      /*segment=*/ [jieba](string_view chunk) { return jieba->Cut(chunk); },
      /*stem=*/ [](string_view word) { return word; },  // identity
      punct, stops);
}

std::unique_ptr<LanguageProcessor> CreateKoreanProcessor(
    const std::string& punct, const std::vector<std::string>& stops) {
  auto kiwi = GetKiwiInstance();
  return std::make_unique<LanguageProcessor>(
      /*segment=*/ [kiwi](string_view chunk) { return kiwi->Analyze(chunk); },
      /*stem=*/ [](string_view word) { return word; },  // identity
      punct, stops);
}

std::unique_ptr<LanguageProcessor> CreateJapaneseProcessor(
    const std::string& punct, const std::vector<std::string>& stops) {
  auto mecab = TryLoadMeCab();  // dlopen
  if (mecab) {
    return std::make_unique<LanguageProcessor>(
        /*segment=*/ [mecab](string_view chunk) { return mecab->Parse(chunk); },
        /*stem=*/ [](string_view word) { return word; },
        punct, stops);
  }
  // Fallback to ICU
  auto breaker = GetICUBreaker("ja");
  return std::make_unique<LanguageProcessor>(
      /*segment=*/ [breaker](string_view chunk) { return icu_break(breaker, chunk); },
      /*stem=*/ [](string_view word) { return word; },
      punct, stops);
}
```

---

## Adding a New Language

No new class. No new file. No inheritance. Just a new 5–10 line function that configures the same class differently.

```cpp
// Someone wants to add Hindi? Write ONE function:
std::unique_ptr<LanguageProcessor> CreateHindiProcessor(
    const std::string& punct, const std::vector<std::string>& stops) {
  auto breaker = GetICUBreaker("hi");
  return std::make_unique<LanguageProcessor>(
      /*segment=*/ [breaker](string_view chunk) { return icu_break(breaker, chunk); },
      /*stem=*/ [](string_view word) { return word; },
      punct, stops);
}
```

---

## The Universal Tokenization Pipeline

The pipeline inside `LanguageProcessor::Tokenize()`:

```cpp
StatusOr<vector<string>> LanguageProcessor::Tokenize(
    string_view text, bool stemming_enabled, uint32_t min_stem_size,
    InProgressStemMap* stem_map) const {

  if (!IsValidUtf8(text)) return InvalidArgumentError("Invalid UTF-8");

  vector<string> tokens;

  // 1. Punct split (same for all languages)
  for (auto chunk : PunctSplit(text, punct_set_)) {
    // 2. Language-specific segmentation
    for (auto sub_token : segment_fn_(chunk)) {
      string word(sub_token);

      // 3. Normalize + lowercase (same for all)
      NormalizeLowerCaseInPlace(word);

      // 4. Stop word check (same for all)
      if (stop_words_set_.contains(word)) continue;

      // 5. Stemming (language-specific)
      if (stemming_enabled && stem_map) {
        auto stemmed = stem_fn_(word);
        if (stemmed != word) {
          (*stem_map)[string(stemmed)].push_back(word);
        }
      }

      tokens.push_back(std::move(word));
    }
  }
  return tokens;
}
```

### Pipeline Steps

| Step | Behavior | Varies by language? |
|------|----------|---------------------|
| 1. Punct split | Split text on punctuation characters | No (shared) |
| 2. Segmentation | Break chunks into sub-tokens | **Yes** — `segment_fn_` |
| 3. Normalize + lowercase | Unicode normalization, lowercasing | No (shared) |
| 4. Stop word check | Filter out stop words | No (shared, configured per language) |
| 5. Stemming | Reduce words to stems | **Yes** — `stem_fn_` |

---

## What Disappears

| Gone | Why |
|------|-----|
| `Lexer` class | Absorbed into `LanguageProcessor::Tokenize()` |
| `SnowballProcessor` class | Replaced by `CreateEnglishProcessor()` function |
| `CppjiebaProcessor` class | Replaced by `CreateChineseProcessor()` function |
| `KiwiProcessor` class | Replaced by `CreateKoreanProcessor()` function |
| `MeCabProcessor` class | Replaced by `CreateJapaneseProcessor()` function |
| `ICUProcessor` class | Replaced by `CreateVietnameseProcessor()` function |
| `LanguageProcessor` interface (abstract) | Replaced by ONE concrete class |
| Factory pattern | Replaced by a simple `Create*()` function lookup |
| Virtual dispatch overhead | Replaced by `std::function` (same cost, simpler code) |
| 5+ header files for processors | ONE header for `LanguageProcessor` |

---

## What Remains

```
src/indexes/text/
  language_processor.h        — ONE class with Tokenize(), configured by SegmentFn + StemFn
  language_processor.cc       — Tokenize() implementation (the universal pipeline)
  language_registry.cc        — Create*Processor() functions for each language
  snowball_stemmer.h/cc       — thread-local sb_stemmer cache (utility, not a class hierarchy)
```

---

## Runtime Branching: Fixed at `FT.CREATE` Time

With the `std::function` approach, the "branching" happens **once** — when `FT.CREATE` is called and the `LanguageProcessor` is constructed. After that, the function pointers are baked in. There's no `if (language == CHINESE)` check on every token during ingestion or query.

### How it works

```
FT.CREATE time (once):
  language = CHINESE
  → CreateChineseProcessor(punct, stops)
  → LanguageProcessor constructed with:
      segment_fn_ = [jieba](chunk) { return jieba->Cut(chunk); }  ← CAPTURED at creation
      stem_fn_ = [](word) { return word; }                        ← CAPTURED at creation
  → Stored in TextIndexSchema for the lifetime of the index

Every HSET / search query (millions of times):
  TextIndexSchema::StageAttributeData(...)
    → processor_->Tokenize(text, ...)
      → calls segment_fn_(chunk)  ← NO branching, direct function pointer call
      → calls stem_fn_(word)      ← NO branching, direct function pointer call
```

There is **no** `switch(language)` at runtime. The decision was made at `FT.CREATE`, captured in a closure, and executed directly thereafter.

---

## Performance Characteristics

| Mechanism | Cost per call |
|-----------|---------------|
| Virtual dispatch (old design with inheritance) | ~1 pointer indirection through vtable |
| `std::function` (this design) | ~1 pointer indirection through type-erased callable |
| Direct function call | 0 indirection |

Both virtual dispatch and `std::function` have essentially the same cost — one indirect call. But the key insight: **neither has branching**. There's no `if`/`else`/`switch` in the hot path. The CPU branch predictor never sees multiple targets — it's always the same function pointer for the lifetime of the index.

### Why not templates?

You could use templates to monomorphize at compile time. But that would require templating `TextIndexSchema` on language — which is impractical since language is a runtime parameter from `FT.CREATE`. The `std::function` indirection is the minimum possible cost for a runtime-configured pipeline.

### Comparison with current Lexer code

The current code has implicit branching that's arguably worse:

```cpp
// Current: branch on every token
sb_stemmer* stemmer = stemming_enabled ? GetStemmer() : nullptr;  // branch
...
if (stemming_enabled) {  // branch on every token
  UpdateStemMap(word, stemmer, min_stem_size, *stem_mappings);
}
```

With the new design, if a language doesn't stem, `stem_fn_` is `[](word) { return word; }` — it still gets called, but it's a trivial identity function that the compiler may inline. No branch prediction miss.

Alternatively, capture the stemming flag at creation time:

```cpp
// At creation time:
if (language_supports_stemming) {
  stem_fn_ = [stemmer](word) { return snowball_stem(stemmer, word); };
} else {
  stem_fn_ = nullptr;  // don't even call it
}

// At runtime in Tokenize():
if (stem_fn_ && stemming_enabled) {  // one predictable branch
  ...
}
```

---

## Summary

- **No processor-per-library.** One class, configured differently per language.
- **No Lexer-calls-Processor.** `LanguageProcessor` IS the tokenizer. It replaces `Lexer` entirely.
- **Plugging in a new language** = writing a 5–10 line function that provides a segment lambda and a stem lambda.
- **All shared logic** (punct split, normalize, stop words, stem map) lives in ONE place — `LanguageProcessor::Tokenize()`.
- **All branching** (which library, which stemmer, which stop words) happens **ONCE** at `FT.CREATE`.
- **At runtime:** no `switch`/`if` per language — just function pointer calls that are constant for the index's lifetime.
- **Branch predictor** sees one target per callsite — optimal for CPU prediction.
- **Cost:** one indirection per `segment_fn_` call + one per `stem_fn_` call — same as virtual dispatch, unavoidable for runtime-configured behavior.

The design is: **configure once at index creation, run the fixed pipeline millions of times without any per-token decision logic.**


---

## Proposed Extensions

The base design handles the known requirements (segment + stem per language). The following extensions address two scenarios that may arise as language support grows.

---

### Extension 1: Adding New Processing Steps (e.g., Lemmatization, Decompounding)

**Problem:** Each new processing step requires adding another `std::function` member, updating the constructor, and threading identity lambdas through every factory function that doesn't use it.

**Solution:** Replace individual named function pointers with a composable pipeline of transform steps. Segmentation remains special (it operates on raw text → tokens), but everything after segmentation becomes a list of token transforms.

```cpp
// language_processor.h — extended design

class LanguageProcessor {
 public:
  // Segmentation: raw text chunk → sub-tokens (1→N, only for languages that need it)
  using SegmentFn = std::function<std::vector<std::string_view>(absl::string_view chunk)>;

  // A token transform: takes a token, returns transformed token (or nullopt to filter it out)
  using TokenTransformFn = std::function<std::optional<std::string>(
      std::string word, InProgressStemMap* stem_map)>;

  // Standard constructor: optional segmentation + transform pipeline
  // Pass nullptr for segment if no sub-word segmentation is needed.
  LanguageProcessor(SegmentFn segment,
                    const std::string& punctuation,
                    std::vector<TokenTransformFn> pipeline);

  absl::StatusOr<std::vector<std::string>> Tokenize(
      absl::string_view text, bool stemming_enabled,
      uint32_t min_stem_size, InProgressStemMap* stem_map) const;

 private:
  void ProcessToken(absl::string_view sub_token, InProgressStemMap* stem_map,
                    std::vector<std::string>& tokens) const;

  SegmentFn segment_fn_;               // nullable — null means no sub-word segmentation
  PunctuationSet punct_set_;
  std::vector<TokenTransformFn> pipeline_;  // ordered list of 1→1 transforms
};
```

#### The updated Tokenize() pipeline

```cpp
StatusOr<vector<string>> LanguageProcessor::Tokenize(
    string_view text, bool stemming_enabled, uint32_t min_stem_size,
    InProgressStemMap* stem_map) const {

  if (!IsValidUtf8(text)) return InvalidArgumentError("Invalid UTF-8");

  vector<string> tokens;

  // 1. Punct split (always runs)
  for (auto chunk : PunctSplit(text, punct_set_)) {

    if (segment_fn_) {
      // 2a. Language needs sub-word segmentation (Chinese, Korean, Japanese, German, etc.)
      for (auto sub_token : segment_fn_(chunk)) {
        ProcessToken(sub_token, stem_map, tokens);
      }
    } else {
      // 2b. No segmentation needed — chunk IS the token (English, French, etc.)
      ProcessToken(chunk, stem_map, tokens);
    }
  }
  return tokens;
}

// Shared helper — runs the 1→1 transform pipeline on a single token
void LanguageProcessor::ProcessToken(
    string_view sub_token, InProgressStemMap* stem_map,
    vector<string>& tokens) const {

  std::optional<std::string> word = std::string(sub_token);

  for (const auto& transform : pipeline_) {
    if (!word.has_value()) break;  // token was filtered out
    word = transform(std::move(*word), stem_map);
  }

  if (word.has_value() && !word->empty()) {
    tokens.push_back(std::move(*word));
  }
}
```

Key points:
- **No identity segmentation functions.** Languages that don't need sub-word segmentation pass `nullptr` — no vector allocation, no wasted function call.
- **The `segment_fn_` null check runs once per chunk** (not per token), and is highly predictable since it's constant for the lifetime of the index.
- **The `ProcessToken` helper** avoids duplicating the transform pipeline logic across both branches.

#### Factory functions with pipeline composition

```cpp
// Each Create*() builds the pipeline it needs — no identity lambdas anywhere

std::unique_ptr<LanguageProcessor> CreateEnglishProcessor(
    const std::string& punct, const std::vector<std::string>& stops) {
  auto stemmer = GetThreadLocalStemmer("english");
  auto stop_set = MakeStopWordSet(stops);

  return std::make_unique<LanguageProcessor>(
      /*segment=*/ nullptr,  // no sub-word segmentation needed
      punct,
      /*pipeline=*/ {
          NormalizeTransform(),                    // lowercase + unicode normalize
          StopWordTransform(std::move(stop_set)),  // filter stop words
          SnowballStemTransform(stemmer),          // stem
      });
}

std::unique_ptr<LanguageProcessor> CreateChineseProcessor(
    const std::string& punct, const std::vector<std::string>& stops) {
  auto jieba = GetJiebaInstance();
  auto stop_set = MakeStopWordSet(stops);

  return std::make_unique<LanguageProcessor>(
      /*segment=*/ [jieba](string_view chunk) { return jieba->Cut(chunk); },
      punct,
      /*pipeline=*/ {
          NormalizeTransform(),
          StopWordTransform(std::move(stop_set)),
          // No stemming — simply not in the pipeline
      });
}

// German: decompounding is a 1→N operation, so it's composed into the SegmentFn.
std::unique_ptr<LanguageProcessor> CreateGermanProcessor(
    const std::string& punct, const std::vector<std::string>& stops) {
  auto stemmer = GetThreadLocalStemmer("german");
  auto stop_set = MakeStopWordSet(stops);
  auto decompounder = GetDecompounder("de");

  return std::make_unique<LanguageProcessor>(
      /*segment=*/ [decompounder](string_view chunk) {
        // Decompound each word in the chunk
        vector<string_view> results;
        for (auto word : WhitespaceSplit(chunk)) {
          for (auto part : decompounder->Split(word)) {
            results.push_back(part);
          }
        }
        return results;
      },
      punct,
      /*pipeline=*/ {
          NormalizeTransform(),
          StopWordTransform(std::move(stop_set)),
          SnowballStemTransform(stemmer),
      });
}
```

#### Reusable transform building blocks

```cpp
// language_transforms.h — shared transform factories

// Normalize: lowercase + unicode normalization
TokenTransformFn NormalizeTransform() {
  return [](std::string word, InProgressStemMap*) -> std::optional<std::string> {
    NormalizeLowerCaseInPlace(word);
    return word;
  };
}

// Stop words: returns nullopt to filter the token
TokenTransformFn StopWordTransform(absl::flat_hash_set<std::string> stop_set) {
  return [stop_set = std::move(stop_set)](std::string word, InProgressStemMap*)
      -> std::optional<std::string> {
    if (stop_set.contains(word)) return std::nullopt;
    return word;
  };
}

// Snowball stemming
TokenTransformFn SnowballStemTransform(sb_stemmer* stemmer) {
  return [stemmer](std::string word, InProgressStemMap* stem_map)
      -> std::optional<std::string> {
    auto stemmed = snowball_stem(stemmer, word);
    if (stem_map && stemmed != word) {
      (*stem_map)[std::string(stemmed)].push_back(word);
    }
    return std::string(stemmed);
  };
}

// Decompounding is a 1→N operation — it belongs in the SegmentFn, not here.
// See the German example above for how to compose segmentation + decompounding.
```

#### Note: 1→N operations (decompounding, multi-word splitting)

Some processing steps produce **multiple** output tokens from one input token (e.g., German decompounding: `"Donaudampfschiff"` → `["Donau", "dampf", "Schiff"]`). These are 1→N operations and **do not belong in the `TokenTransformFn` pipeline**, which is strictly 1→1.

Instead, 1→N operations should be composed into the `SegmentFn`:

- **Segmentation** is the single designated slot for 1→N operations
- Decompounding is conceptually segmentation — breaking a compound word into parts is the same operation as breaking a sentence into words, just at a different granularity
- Compose multiple 1→N operations (whitespace split + decompounding) inside one `SegmentFn` lambda

This keeps the 1→1 transform pipeline tight (no intermediate vector allocations per step) while still supporting languages that need multi-token expansion. For languages that need an entirely different flow, use the `FullPipelineFn` escape hatch.

---

### Extension 2: Libraries That Own the Entire Pipeline

**Problem:** Some NLP libraries (e.g., a morphological analyzer that does segmentation + normalization + POS tagging + lemmatization as one integrated pass) don't decompose into "segment, then run shared transforms." Forcing their output through the shared pipeline either duplicates work or produces incorrect results.

**Solution:** Add an optional `FullPipelineFn` that, when set, replaces the entire `Tokenize()` implementation. The decomposed path remains the default for most languages.

```cpp
class LanguageProcessor {
 public:
  using SegmentFn = std::function<std::vector<std::string_view>(absl::string_view chunk)>;
  using TokenTransformFn = std::function<std::optional<std::string>(
      std::string word, InProgressStemMap* stem_map)>;

  // Optional: replaces the entire Tokenize() pipeline
  using FullPipelineFn = std::function<absl::StatusOr<std::vector<std::string>>(
      absl::string_view text, bool stemming_enabled,
      uint32_t min_stem_size, InProgressStemMap* stem_map)>;

  // Standard constructor: optional segmentation + transform pipeline
  // Pass nullptr for segment if no sub-word segmentation is needed.
  LanguageProcessor(SegmentFn segment,
                    const std::string& punctuation,
                    std::vector<TokenTransformFn> pipeline);

  // Override constructor: library owns everything
  explicit LanguageProcessor(FullPipelineFn full_pipeline);

  absl::StatusOr<std::vector<std::string>> Tokenize(
      absl::string_view text, bool stemming_enabled,
      uint32_t min_stem_size, InProgressStemMap* stem_map) const;

 private:
  void ProcessToken(absl::string_view sub_token, InProgressStemMap* stem_map,
                    std::vector<std::string>& tokens) const;

  // If set, Tokenize() delegates entirely to this
  FullPipelineFn full_pipeline_fn_;

  // Used only when full_pipeline_fn_ is not set
  SegmentFn segment_fn_;               // nullable — null means no sub-word segmentation
  PunctuationSet punct_set_;
  std::vector<TokenTransformFn> pipeline_;
};
```

#### Updated Tokenize() with override check

```cpp
StatusOr<vector<string>> LanguageProcessor::Tokenize(
    string_view text, bool stemming_enabled, uint32_t min_stem_size,
    InProgressStemMap* stem_map) const {

  // Escape hatch: library owns the whole pipeline
  if (full_pipeline_fn_) {
    return full_pipeline_fn_(text, stemming_enabled, min_stem_size, stem_map);
  }

  // Standard decomposed pipeline
  if (!IsValidUtf8(text)) return InvalidArgumentError("Invalid UTF-8");

  vector<string> tokens;
  for (auto chunk : PunctSplit(text, punct_set_)) {
    if (segment_fn_) {
      for (auto sub_token : segment_fn_(chunk)) {
        ProcessToken(sub_token, stem_map, tokens);
      }
    } else {
      ProcessToken(chunk, stem_map, tokens);
    }
  }
  return tokens;
}

void LanguageProcessor::ProcessToken(
    string_view sub_token, InProgressStemMap* stem_map,
    vector<string>& tokens) const {
  std::optional<std::string> word = std::string(sub_token);
  for (const auto& transform : pipeline_) {
    if (!word.has_value()) break;
    word = transform(std::move(*word), stem_map);
  }
  if (word.has_value() && !word->empty()) {
    tokens.push_back(std::move(*word));
  }
}
```

#### Example: Library that owns everything

```cpp
std::unique_ptr<LanguageProcessor> CreateAdvancedJapaneseProcessor(
    const std::string& punct, const std::vector<std::string>& stops) {
  auto analyzer = GetFullJapaneseAnalyzer();  // hypothetical integrated library
  auto stop_set = MakeStopWordSet(stops);

  return std::make_unique<LanguageProcessor>(
      /*full_pipeline=*/ [analyzer, stop_set](
          string_view text, bool stemming_enabled,
          uint32_t min_stem_size, InProgressStemMap* stem_map)
          -> StatusOr<vector<string>> {
        if (!IsValidUtf8(text)) return InvalidArgumentError("Invalid UTF-8");

        // Library handles segmentation + normalization + morphological analysis
        auto analyzed = analyzer->Analyze(text);

        vector<string> tokens;
        for (auto& morpheme : analyzed) {
          if (stop_set.contains(morpheme.surface)) continue;

          if (stemming_enabled && stem_map && morpheme.base != morpheme.surface) {
            (*stem_map)[morpheme.base].push_back(morpheme.surface);
          }
          tokens.push_back(std::move(morpheme.base));
        }
        return tokens;
      });
}
```

#### When to use the override vs. the decomposed pipeline

| Scenario | Use |
|----------|-----|
| Library does segmentation only (jieba, kiwi) | Decomposed pipeline with `segment_fn_` |
| Library does segmentation + normalization | Decomposed pipeline; skip `NormalizeTransform()` in that language's pipeline |
| Library does the entire analysis (segment + normalize + lemmatize + POS) | `FullPipelineFn` override |
| Standard Snowball-based language | Decomposed pipeline with `SnowballStemTransform` |

---

### Updated File Structure

```
src/indexes/text/
  language_processor.h          — LanguageProcessor class (SegmentFn + pipeline + optional FullPipelineFn)
  language_processor.cc         — Tokenize() implementation (dispatch + decomposed pipeline)
  language_transforms.h/cc      — Reusable transform building blocks (Normalize, StopWord, Stem, etc.)
  language_registry.cc          — Create*Processor() functions for each language
  snowball_stemmer.h/cc         — thread-local sb_stemmer cache (utility)
```

---

### Design Principles Preserved

| Principle | Still holds? |
|-----------|-------------|
| One class, no hierarchy | ✅ Still one `LanguageProcessor` class |
| No virtual dispatch | ✅ Still `std::function` / direct calls |
| No identity functions | ✅ `segment_fn_` is nullable (not called if null); pipeline only contains steps the language uses |
| Branching fixed at FT.CREATE | ✅ Pipeline composed once, reused forever |
| Adding a language = one function | ✅ Write a `Create*Processor()` that picks its transforms |
| Adding a processing step = one transform | ✅ Write a `TokenTransformFn`, add it to relevant pipelines |
| Library can own the whole pipeline | ✅ `FullPipelineFn` escape hatch |
| Shared logic in one place | ✅ Reusable transforms in `language_transforms.h` |
