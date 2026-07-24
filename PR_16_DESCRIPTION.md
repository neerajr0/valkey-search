## Motivation

The existing `LanguageProcessor` / `Lexer` architecture tightly couples text processing logic (tokenization, normalization, stemming, stop word filtering) into monolithic classes. This makes it difficult to:

- Add new language families (e.g., CJK via custom libraries) without modifying core interfaces
- Reuse individual processing steps independently (e.g., normalization-only for wildcard queries)
- Test components in isolation
- Reason about the processing pipeline's behavior per language

This PR refactors the `LanguageProcessor` into a **composable pipeline of modular, standalone primitives** — Segmenters and TokenFilters. The resulting architecture is:

- **Modular**: Each primitive (segmenter, filter) is an independent, testable class
- **Composable**: Adding a new language means composing different segmenters/filters — no interface changes needed
- **Stateless**: `Process()` is idempotent with no side effects
- **Language-agnostic**: Neither ingestion nor query paths contain language-specific logic outside the pipeline

This also eliminates the `Lexer` class entirely, replacing it with direct use of the `LanguageProcessor` pipeline.

### Pipeline Execution Semantics

`Process(text)`:

1. **Segmentation phase** (`Segment()`): Sequential 1 to many transforms. Feed `text` to the first segmenter. For each subsequent segmenter in the list, feed every token from the previous step as input. This chains segmenters so that each one further splits the output of the previous.

2. **Filter phase** (`ApplyFilters()`): Sequential 1 to 1 or 1 to 0 (elimination) transforms. For each token in the final segmented output, apply each token filter in order. If any filter returns false, the token is eliminated. Otherwise, the (possibly mutated) token is passed to the next filter.

3. Return the final list of surviving tokens.

### Ingestion Path (full pipeline)

```
Input text
    │
    ▼
┌───────────────────────────┐
│ Segment()                 │
│  Segmenter 1              │  (e.g., PunctuationSegmenter)
│  text → [tokens]          │
│                           │
│  Segmenter 2 (optional)   │  (e.g., Decompounder)
│  [tokens] → [tokens]     │
└───────────────────────────┘
    │
    ▼
┌───────────────────────────┐
│ ApplyFilters()            │
│  NormalizeCaseFoldFilter  │  token → token (NFC + casefold)
│  StopWordFilter           │  token → token|∅
└───────────────────────────┘
    │
    ▼
Final token list
```

> **Note**: Stemming (`SnowballStemFilter`) is NOT part of the main pipeline. Tokens are
> indexed in their normalized (unstemmed) form. The stemmer is stored separately on the
> `LanguageProcessor` and accessed via `GetStemmer()` for callers that need it (stem map
> building, query expansion, delete path).

### Query Path (pluggable tokenization strategy)

The query parser handles query grammar (`@`, `(`, `)`, `|`, `"`, `*`, `%`, `-`) and delegates
word-boundary detection to a **pluggable `QueryTokenizer`** interface. This decouples the parser
from any specific tokenization approach:

- **`DelimiterQueryTokenizer`** (European/Snowball): walks codepoints, breaks on punctuation
  delimiters via `PunctuationSegmenter::IsDelimiter()`
- **Future CJK tokenizer**: extracts raw text spans between query syntax characters and
  delegates to `Segment()` for dictionary-based word splitting

The `QueryTokenizer` is stored on the `LanguageProcessor` and accessed via `GetQueryTokenizer()`.

```
Query expression
    │
    ▼
┌────────────────────────────────────────────────────┐
│ Parser walks query syntax chars                    │
│  • Delegates word extraction to QueryTokenizer     │
│    ├── DelimiterQueryTokenizer (European)          │
│    │   walks codepoints, breaks on IsDelimiter()   │
│    └── [future] CJK tokenizer                     │
│        extracts span, delegates to Segment()       │
│  • Handles escape sequences (\<char>)             │
└────────────────────────────────────────────────────┘
    │
    ├── Regular term ──────────────────────┐
    │   NormalizeCaseFoldFilter            │
    │   StopWordFilter (if stop → drop)   │
    │   → TermPredicate                    │
    │                                      │
    ├── Wildcard / Fuzzy ──────────────────┤
    │   NormalizeCaseFoldFilter only       │
    │   → Prefix/Suffix/FuzzyPredicate    │
    │                                      │
    └── Exact phrase (quoted) ─────────────┘
        NormalizeCaseFoldFilter only
        → TermPredicate (exact=true)
```

## File Layout

### Design Principle

One file per major class hierarchy or cohesive component. Split only when: new external
dependency or new language family with distinct algorithm.

### Current Layout

```
language_processor.h/.cc  — LanguageProcessor + Builder + all abstract interfaces
                            (Segmenter, TokenFilter, Normalizer, Stemmer,
                            QueryTokenizer) + all language-agnostic impls
                            (PunctuationSegmenter, NormalizeCaseFoldFilter,
                            StopWordFilter, DelimiterQueryTokenizer)

snowball_stem.h/.cc       — SnowballStemFilter (isolates libstemmer dependency)
```

### Future Growth (CJK)

```
language_processor.h/.cc  — unchanged
snowball_stem.h/.cc       — unchanged
icu_segmenter.h/.cc       — NEW: would pull in libicu
jieba_segmenter.h/.cc     — NEW: would pull in cppjieba
mecab_analyzer.h/.cc      — NEW: would pull in libmecab
```

## Summary of Changes

### New Files

| File | Description |
|------|-------------|
| `src/indexes/text/language_processor.h` | Redefined as composed pipeline owning segmenters + token filters. Contains all interfaces (`Segmenter`, `TokenFilter`, `Normalizer`, `Stemmer`, `QueryTokenizer`) + all language-agnostic implementations (`PunctuationSegmenter`, `NormalizeCaseFoldFilter`, `StopWordFilter`, `DelimiterQueryTokenizer`) + `LanguageProcessor` class with Builder pattern. `Segment()` applies all segmenters sequentially (each further splits the output of the previous). `ApplyFilters()` applies all token filters sequentially to each token. `Process()` orchestrates the full pipeline: `Segment()` then `ApplyFilters()`. Individual components (normalizer, stemmer, segmenter, query tokenizer) are exposed via accessor functions for callers that need them outside the pipeline (e.g., wildcard normalization, stem root computation, delimiter detection, query token extraction) |
| `src/indexes/text/language_processor.cc` | All implementations + `CreateSnowballProcessor` factory that composes `PunctuationSegmenter` + `NormalizeCaseFoldFilter` + `StopWordFilter` + `SnowballStemFilter` via the Builder |
| `src/indexes/text/snowball_stem.h` | `SnowballStemFilter` class declaration — Snowball stemming algorithm + `GetStemRoot()` for single-word stem computation + `BuildStemMap()` for ingestion-time stem→surface-form mapping. Isolates `libstemmer` dependency |
| `src/indexes/text/snowball_stem.cc` | `SnowballStemFilter` implementation — extracted from the old `SnowballProcessor::StemWordInPlace()` and `BuildStemMap()`. Uses thread-local `sb_stemmer` instances per language |

### Modified Files

| File | Description |
|------|-------------|
| `src/indexes/text/punctuation.h` | Per-language punctuation character sets sourced from Unicode CLDR v46, `PunctuationSet` lookup structure (ASCII bitset + non-ASCII hash set), `BuildPunctuationSet()` utility, `GetDefaultPunctuation()` factory |
| `src/indexes/text/stop_words.h` | Per-language default stop word lists sourced from Apache Lucene, `GetDefaultStopWords()` factory, `BuildStopWordsSet()` utility |
| `src/commands/filter_parser.cc` | Refactored to delegate word extraction to `QueryTokenizer` interface (via `GetQueryTokenizer()`) instead of inline codepoint walking. Escape handling moved into `DelimiterQueryTokenizer`. Wildcard/fuzzy use the normalizer accessor to apply normalization directly |
| `src/commands/filter_parser.h` | Removed `ExtractNextToken*` methods — logic moved to `QueryTokenizer` |
| `src/indexes/text/text_index.cc` | Updated `StageAttributeData` to use `Process()` + separate `BuildStemMap()`. Updated `DeleteKeyData` and `GetAllStemVariants` to use the stemmer accessor's `GetStemRoot()` |
| `src/indexes/text/text_index.h` | Updated interface to reflect new pipeline usage |
| `src/indexes/text/textinfocmd.cc` | Replaced `Tokenize()` calls with `Process()` |
| `src/utils/scanner.h` | Updated comment to reference `LanguageProcessor` instead of the deleted `Lexer` class |
| `src/indexes/CMakeLists.txt` | Updated source file list for new layout |

### Deleted Files

| File | Description |
|------|-------------|
| `src/indexes/text/lexer.cc` | Removed — functionality replaced by `LanguageProcessor::Process()` pipeline |
| `src/indexes/text/lexer.h` | Removed — interface replaced by `LanguageProcessor` |
| `testing/lexer_test.cc` | Removed — tests migrated and expanded in `snowball_processor_test.cc` |

### Test Files

| File | Description |
|------|-------------|
| `testing/snowball_processor_test.cc` | Expanded coverage: pipeline integration tests, German compound word test, French apostrophe elision test, cross-language independence test, per-component unit tests |
| `testing/text_test.cc` | Updated to use new pipeline API; added tests for new filter/segmenter interactions |
| `testing/filter_test.cc` | Updated to align with new filter parser API (QueryTokenizer-based word extraction) |
| `testing/CMakeLists.txt` | Removed `lexer_test.cc`, updated dependencies |
| `integration/test_multi_language_search.py` | **New**: Comprehensive integration test suite — per-language stemming roundtrip (12 languages parametrized), RDB save/restore, cross-contamination guard, FT.INFO validation, German compound word, French apostrophe elision, cluster metadata consistency, query parser non-ASCII handling, Arabic NFKC normalization, non-ASCII punctuation, stop word filtering |

## Testing

### Unit Tests

- **`snowball_processor_test.cc`** — Expanded from basic stemming checks to comprehensive pipeline tests covering:
  - All 12 Snowball languages with per-language stemming verification
  - Individual component tests (PunctuationSegmenter, NormalizeCaseFoldFilter, StopWordFilter, SnowballStemFilter)
  - Pipeline composition tests (full `Process()` flow)
  - Edge cases: German compound words (`Donaudampfschifffahrtsgesellschaft`), French apostrophe elision (`l'école`), cross-language processor independence
- **`text_test.cc`** — Updated for new pipeline API, verifies ingestion/deletion/query expansion paths
- **`filter_test.cc`** — Updated for new filter parser API, verifies QueryTokenizer-based token extraction

### Integration Tests

- **`test_multi_language_search.py`** — Comprehensive integration suite:
  - `TestMultiLanguageAllAccepted` — All 12 Snowball languages accepted in FT.CREATE
  - `TestPerLanguageStemming` — Parametrized over all 12 languages with language-specific doc text and stem queries
  - `TestMultiLanguageSearch` — Stop words, NFC normalization, Arabic NFKC, non-ASCII punctuation
  - `TestLanguageInFTInfo` — FT.INFO correctly reports configured and default language
  - `TestLanguageSaveRestore` — RDB persistence of LANGUAGE field across restart
  - `TestLanguageCrossContamination` — French + German indexes on same server don't bleed
  - `TestGermanCompoundWord` — Full word searchable, prefix works, no decompounding
  - `TestFrenchApostropheElision` — Apostrophe splits tokens correctly
  - `TestQueryParserNonAscii` — Multi-byte UTF-8 byte-limit handling
  - `TestLanguageClusterMetadata` — Language consistency across cluster nodes
  - `TestLanguageRDBBackwardCompat` — Indexes without LANGUAGE default to English

### Benchmarking & Performance

- Information retrieval benchmarking will be performed using [valkey-search-multilang-bench](https://github.com/neerajr0/valkey-search-multilang-bench). For benchmark methodology and results, refer to [Multi-Language Support: Benchmarking Search Performance](https://quip-amazon.com/U7s4AwRZsLqc/Multi-Language-Support-Benchmarking-Search-Performance).
- Performance testing will be run using [valkey-perf-benchmark](https://github.com/valkey-io/valkey-perf-benchmark/).
