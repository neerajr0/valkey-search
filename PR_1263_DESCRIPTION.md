## Overview

Introduces FTS for 11 new languages in addition to English that are supported by the Snowball Stemmer: French, German, Spanish, Italian, Portuguese, Russian, Swedish, Turkish, Dutch, Indonesian and Arabic. Additionally refactors the existing ingestion and query paths so that language-specific behavior (segmentation, normalization, stemming, stop-word filtering) is a composable pipeline of standalone primitives, providing a clean extension point for future languages without modifying the core FTS paths.

The feature ships behind the `MetadataVersion` bump to `1.4`. Non-English languages are gated by `IsLanguageSupported(...)` (`src/multi_language.h`) so mixed-version clusters fail closed — nodes running < 1.4 will not receive index metadata carrying a non-English `LANGUAGE`.

## Summary of Changes

### High-level architecture

Text handling is currently a monolithic `Lexer` class that hard-coded English tokenization, ASCII-only punctuation, and English Snowball stemming. This PR replaces it with a `LanguageProcessor` — a pipeline built from three swappable primitive types:

- **`Segmenter`** — splits a text span into tokens (1→N). Default implementation `PunctuationSegmenter` is used by all 12 Snowball languages and parameterized per-language via a `PunctuationSet` sourced from Common Locale Data Repository (CLDR) v46.
- **`TokenFilter`** — transforms or drops a token (1→1 or 1→0). Default implementations `NormalizeCaseFoldFilter` (NFC + Unicode casefold, locale-aware for Turkish dotless-i) and `StopWordFilter` (per-language default lists sourced from Apache Lucene) are shared across all 12 Snowball languages and parameterized per-language via their inputs (locale, stop-word set).
- **`Stemmer` / `QueryTokenizer`** — accessors stored on the processor for callers that need them outside the ingestion loop (stem-map building, delete path, query-time word extraction). `SnowballStemFilter` is per-language (one `sb_stemmer` per language); `DelimiterQueryTokenizer` is shared across Snowball languages and delegates delimiter detection to `PunctuationSegmenter::IsDelimiter()`.

Segmenters run sequentially (each further splits the previous output), then every surviving token is threaded through the filter chain. Stemming is deliberately **not** part of the ingestion pipeline — tokens are indexed in their normalized-but-unstemmed form; the stemmer is kept aside and invoked separately when a stem-map or query expansion is needed.

#### Ingestion pipeline

```
Input text
    │
    ▼
┌────────────────────────────┐
│ Segment()                  │
│   PunctuationSegmenter     │  UTF-8 codepoint aware; per-language
│   text → [tokens]          │  punctuation set (e.g. Arabic, French,
│                            │  German, ...)
└────────────────────────────┘
    │
    ▼
┌────────────────────────────┐
│ ApplyFilters()             │
│   NormalizeCaseFoldFilter  │  token → token (NFC + Unicode
│                            │  casefold; locale-aware for Turkish)
│   StopWordFilter           │  token → token | ∅
└────────────────────────────┘
    │
    ▼
Indexed tokens  ──┐
                  │  (separately) GetStemmer()->BuildStemMap()
                  ▼           stem_root → {surface_forms}
             Stem map
```

#### Query pipeline

Query grammar characters (`@`, `(`, `)`, `|`, `"`, `*`, `%`, `-`, `\`) are handled by the parser, and word-boundary detection is delegated to a pluggable `QueryTokenizer` bound to the index's language:

```
Query expression
    │
    ▼
┌────────────────────────────────────────────────────┐
│ FilterParser walks query syntax chars              │
│   Word extraction → QueryTokenizer                 │
│     DelimiterQueryTokenizer (Snowball languages)   │
│     [future] CJK-specific tokenizers               │
│   Escape handling → \<char>                        │
└────────────────────────────────────────────────────┘
    │
    ├── Regular term ──────────────────────┐
    │   NormalizeCaseFoldFilter            │
    │   StopWordFilter (if stop → drop)    │
    │   Stemmer.GetStemRoot()              │→ TermPredicate
    │                                      │
    ├── Wildcard / Fuzzy ──────────────────┤
    │   NormalizeCaseFoldFilter only       │→ Prefix/Suffix/FuzzyPredicate
    │                                      │
    └── Exact phrase (quoted) ─────────────┘
        NormalizeCaseFoldFilter only        → TermPredicate(exact=true)
```

`Fuzzy` DP also became codepoint-aware so `min_stem_size` and edit distance work correctly for multi-byte scripts (Arabic, Russian, Turkish). The change lives in `fuzzy.h` via `Scanner::NextUtf8()`, and includes buffering across `Rax` edge boundaries so a codepoint split by radix-tree edge compression decodes correctly. `Proximity` and `OrProximity` operate on word-level positions in an already-tokenized stream, so they're unaffected by multi-byte and are unchanged by this PR.

#### File layout principle

One file per major class hierarchy. Split only when a new external dependency or algorithm family shows up (e.g. Snowball is isolated into `snowball_stem.*` so `libstemmer` linkage stays localized). Future CJK support would add `icu_segmenter.*`, `jieba_segmenter.*`, `mecab_analyzer.*`(as examples) without touching `language_processor.*`.

### Per-file breakdown

#### New files

| File | Purpose |
|---|---|
| `src/multi_language.h` | Version-guard helpers: `IsNonEnglishLanguage`, `IsMultiLanguageSupported`, `IsLanguageSupported`. Enforces the 1.4 minimum for non-English `LANGUAGE`. |
| `src/indexes/text/language_processor.h` | `LanguageProcessor` class + Builder + all interfaces (`Segmenter`, `TokenFilter`, `Normalizer`, `Stemmer`, `QueryTokenizer`) + default implementations that are shared across all 12 Snowball languages and parameterized per-language via their inputs (`PunctuationSegmenter`, `NormalizeCaseFoldFilter`, `StopWordFilter`, `DelimiterQueryTokenizer`). |
| `src/indexes/text/language_processor.cc` | Implementations + `CreateSnowballProcessor(language)` factory that composes the Snowball pipeline. |
| `src/indexes/text/snowball_stem.h` | `SnowballStemFilter` declaration: `GetStemRoot()` for single-word stem, `BuildStemMap()` for ingestion-time stem→surface-form map. Isolates the `libstemmer` dependency. |
| `src/indexes/text/snowball_stem.cc` | `SnowballStemFilter` implementation using thread-local `sb_stemmer` instances per language. |
| `src/indexes/text/punctuation.h` | Per-language punctuation character sets (CLDR v46), `PunctuationSet` lookup structure (ASCII bitset + non-ASCII hash set), `GetDefaultPunctuation()` factory. |
| `src/indexes/text/stop_words.h` | Per-language default stop-word lists (Apache Lucene), `GetDefaultStopWords()` factory, `BuildStopWordsSet()` utility. |
| `third_party/snowball/src_c/stem_UTF_8_{LANGUAGE}.{c,h}` | 11 generated UTF-8 stemmer implementations from upstream Snowball. 

#### Modified files

| File | Change |
|---|---|
| `src/index_schema.proto` | Adds `Language` enum (12 values) + `language`, `punctuation`, `stop_words`, `min_stem_size`, `with_offsets`, `skip_initial_scan`, `score_field` fields on `IndexSchema`. |
| `src/index_schema.cc` | Wires `Language` through create/serialize/deserialize; enforces `IsLanguageSupported` at ingest. |
| `src/coordinator/search_converter.{cc,h}` | Coordinator-side malformed-UTF-8 gate for inter-node predicate transmission. Rejects malformed bytes at `emulate-release >= 1.4`; under legacy emulation, substitutes U+FFFD in text tokens while leaving tag/numeric values raw so exact match still works. Emits the `compatibility-grpc_predicate_invalid_utf8` INFO counter on the legacy path. |
| `src/version.h` | Bumps `kModuleVersion` to `1.4.0`; adds `kRelease14` constant used by the metadata min-version machinery. |
| `THIRD_PARTY_NOTICES` | Adds licensing notices for **Unicode CLDR** (Unicode License V3) covering the per-language punctuation exemplars in `punctuation.h`, and **Apache Lucene** (Apache 2.0) covering the per-language stop-word lists in `stop_words.h`. |
| `.config/typos.toml` | Adds language-specific exclusions (`stop_words.h`, `language_processor_test.cc`, `snowball_stem_test.cc`, `data_sets.py`) and non-English/multi-language words (`caf`, `café`, `caf\\xC3\\xA9`, `mak`, `continuent`) so previously-flagged typos pass in the new content. |
| `vmsdk/src/command_parser.h` | Renames `GENERATE_CLEAR_CONTAINER_PARSER` to `GENERATE_EMPLACE_PARSER` and switches its body from `.clear()` to `.emplace()`. Used by the `NOSTOPWORDS` clause in `ft_create_parser.cc` to explicitly materialize an empty stop-word set (distinguishing "no stop words" from "use per-language default", which leaves the optional unset). |
| `src/commands/ft_create_parser.{h,cc}` | Parses the `LANGUAGE`, `LANGUAGE_FIELD`, `STOPWORDS`, `PUNCTUATION`, `MINSTEMSIZE` clauses and validates them against `IsLanguageSupported`. |
| `src/commands/filter_parser.{h,cc}` | Replaces inline byte-level codepoint walking with delegation to `QueryTokenizer::ExtractNextToken()`. Escape and quoted-string handling moved into `DelimiterQueryTokenizer`. Wildcard/fuzzy paths call the normalizer accessor directly. |
| `src/indexes/text/fuzzy.h` | Fuzzy DP now sized in codepoints, not bytes; multi-byte codepoints split across radix-tree edges are buffered and decoded across edges. |
| `src/indexes/text/text_index.{h,cc}` | `StageAttributeData` now calls `LanguageProcessor::Process()` + `BuildStemMap()`; `DeleteKeyData` and `GetAllStemVariants` use `GetStemmer()->GetStemRoot()`. |
| `src/indexes/text/textinfocmd.cc` | Replaces `Lexer::Tokenize()` calls with `Process()`. |
| `src/indexes/text/unicode_normalizer.{h,cc}` | Implements `Normalize()` (NFC by default; NFKC path used for Arabic to fold presentation forms). Wired into the tokenization pipeline. |
| `src/utils/scanner.h` | Comment/reference updates from `Lexer` to `LanguageProcessor`. |
| `src/indexes/CMakeLists.txt`, `src/utils/CMakeLists.txt`, `testing/CMakeLists.txt` | Build wiring for the new pipeline sources, the shared `scanner` utility, and the new test binaries; drops `lexer.cc` / `lexer_test.cc`. |
| `third_party/snowball/CMakeLists.txt` | Adds the 11 new `stem_UTF_8_*.c` translation units to the build. |
| `third_party/icu/README.md` | Documentation for the ICU static-library dependency used by `UnicodeNormalizer` (NFC/NFKC) and locale-aware casefold. |
| `third_party/snowball/libstemmer/modules.h` | Registers the 11 new algorithm entries. |
| `third_party/snowball/add_language.sh` | Rewritten to be idempotent and to generate UTF-8 stemmers only. |
| `third_party/snowball/OWNERS` | Adds one ownership entry to keep the file consistent with the new Snowball language sources. |

#### Deleted files

| File | Reason |
|---|---|
| `src/indexes/text/lexer.h` / `src/indexes/text/lexer.cc` | Monolithic English-only lexer replaced by `LanguageProcessor` pipeline. |

## Unit & Integration Testing

The refactor moved a lot of behavior from a single monolithic class into small composable primitives, so the test strategy was to (a) unit-test each primitive in isolation, (b) unit-test the composed pipeline end-to-end for every supported language, and (c) integration-test the whole feature against a running server. Areas that needed new coverage:

- Per-language tokenization, stemming, stop-word filtering, and punctuation handling (12 languages).
- Multi-byte UTF-8 correctness in fuzzy DP, prefix/suffix matching, and query-parser codepoint walking.
- Unicode normalization (NFC across the board; NFKC folding for Arabic presentation forms).
- Locale-aware casefolding (Turkish dotless-i).
- `FT.CREATE` parser acceptance of `LANGUAGE`, `PUNCTUATION`, `STOPWORDS`, `MINSTEMSIZE`.
- `LANGUAGE` round-tripping through RDB save/restore and forward/backward RDB compatibility.
- `LANGUAGE` propagation and consistency across cluster nodes.
- Version gate (nodes < 1.4 must reject non-English metadata) — both unit and integration.
- Cross-language isolation (French + German indexes on the same server must not bleed into each other).
- Intentional behavioral divergences from RediSearch (German ß→ss folding, Turkish dotted/dotless-I, ligature decomposition, strict `NOSTEM`, Arabic fuzzy behavior, Dutch KP stemmer) — locked in so future refactors don't silently regress them.

### Unit tests

| Test file | Coverage |
|---|---|
| `testing/language_processor_test.cc` (**new**) | `LanguageProcessor` pipeline end-to-end; per-primitive tests for `PunctuationSegmenter`, `NormalizeCaseFoldFilter`, `StopWordFilter`, `DelimiterQueryTokenizer`; composition tests. |
| `testing/snowball_stem_test.cc` (**new**) | `SnowballStemFilter` + `BuildStemMap` + `GetStemRoot` across all 12 languages; German compound word (`Donaudampfschifffahrtsgesellschaft`); French apostrophe elision (`l'école`); Turkish locale-aware stem length; cross-language processor independence. |
| `testing/unicode_normalizer_test.cc` (**new**) | NFC round-trips, NFKC Arabic presentation-form folding, casefold behavior. |
| `testing/filter_test.cc` (**new**) | Query-parser word extraction via `QueryTokenizer`, escape handling, wildcard/fuzzy normalization, quoted phrases. |
| `testing/ft_create_parser_test.cc` (**modified**) | `LANGUAGE`, `LANGUAGE_FIELD`, `STOPWORDS`, `PUNCTUATION`, `MINSTEMSIZE` clause parsing and validation at the parser level. |
| `testing/ft_create_test.cc` (**modified**) | End-to-end `FT.CREATE` acceptance of the new clauses. |
| `testing/index_schema_test.cc` (**modified**) | `Language` field round-trips through serialize/deserialize; per-index schema stop-word / punctuation defaults. |
| `testing/search_test.cc` (**modified**) | Search paths through the `LanguageProcessor` pipeline, including the coordinator-side malformed-UTF-8 gating. |
| `testing/utils/scanner_test.cc` (**modified**) | Rewrite of the shared `Scanner` utility test to cover multi-byte codepoint decoding, punctuation-driven delimiters, and the fast-path ASCII branch. |
| `testing/text_test.cc` (**modified**) | Migrated onto new pipeline API; added filter/segmenter interaction cases. |
| `testing/lexer_test.cc` (**removed**) | Superseded by `language_processor_test.cc` + `snowball_stem_test.cc`. |

### Integration tests

| Test file | Coverage |
|---|---|
| `integration/test_multi_language_search.py` (**new**) | `TestMultiLanguageAllAccepted` — all 12 languages accepted on `FT.CREATE`; `TestPerLanguageStemming` — parametrized per-language stemming roundtrip; `TestMultiLanguageSearch` — stop words, NFC, Arabic NFKC, non-ASCII punctuation; `TestLanguageInFTInfo` — `FT.INFO` reports configured and default language; `TestLanguageSaveRestore` — RDB persistence across restart; `TestLanguageCrossContamination` — French + German isolation on the same server; `TestGermanCompoundWord` — full-word searchable + prefix; `TestFrenchApostropheElision` — apostrophe splits tokens; `TestQueryParserNonAscii` — multi-byte UTF-8 byte-limit handling; `TestLanguageClusterMetadata` — language consistency across cluster nodes; `TestLanguageRDBBackwardCompat` — indexes without `LANGUAGE` default to English; `TestDeliberateDivergences` — locks in intentional behavioral differences vs. RediSearch: German ß→ss folding (`utf8Fold`), Turkish dotted/dotless-I locale-aware lowercasing, ligature case-fold decomposition, strict `NOSTEM` enforcement on bare queries, Arabic fuzzy on original form only, Dutch KP-stemmer prefix stripping. |
| `integration/test_multi_language_guard.py` (**new**) | Version gate: non-English `LANGUAGE` rejected on nodes running with `IsMultiLanguageSupported() == false`. |
| `integration/test_rdb_cross_version_compat.py` (**new**) | Forward/backward RDB compatibility across 1.2 / 1.4 module versions with and without `LANGUAGE`. |
| `integration/compatibility/generate_text.py` (**new**) + `text-search-multilang-answers.pickle.gz` (**new**) | Regeneratable expected-answer corpus for the multi-language text-search compat suite. |
| `integration/compatibility/aggregate-answers.pickle.gz`, `text-search-answers.pickle.gz` (**regenerated, not manually edited**) | Rewritten via `regenerate.sh` after the upstream merge so the pickles reflect both the multi-language additions to `data_sets.py` and the compat fixes upstream landed on the base scenarios. Not authored by hand — both sides had incompatible regenerations that could not be textually merged. |
| `integration/compatibility_test.py`, `integration/compatibility/data_sets.py`, `integration/compatibility/__init__.py`, `integration/compatibility/regenerate.sh` (**modified**) | Compat harness extended with per-generator answer files, `GENERATOR_FILES_UNIQUE` for the regenerator, and `compute_sources_hash()` so stale pickles are detected automatically. |
| `integration/test_fulltext.py`, `integration/utils.py` (**modified**) | Existing FTS suites extended for multi-byte input, non-ASCII punctuation, and the cross-version RDB compat helpers (`start_server_with_old_module` / `cleanup_module_binary`). |
| `integration/module/1.2.0-libsearch.so.zip` (**new**) | Binary fixture: pinned 1.2.0 module `.so` archive, used exclusively by `test_rdb_cross_version_compat.py` to load an older RDB and verify forward/backward compatibility. |
| `integration/run.sh` (**modified**) | Exports `ROOT_DIR` so the compat regeneration flow can locate itself when run from within the integration harness. |
| `integration/test_rdb_load_on_module_v1_0.py` (**removed**) | Replaced by the cross-version compat suite. |

## Incremental PRs

Merged into the multi-language-support branch chronologically (`VoletiRam/valkey-search` unless noted):

1. **[#1](https://github.com/VoletiRam/valkey-search/pull/1)** — Add release 1.4 and feature flag for multi-language support. Introduces `kRelease14`, `IsLanguageSupported`, and the version-guard in `src/multi_language.h`.
2. **[#6](https://github.com/VoletiRam/valkey-search/pull/6)** — Make text tokenization, fuzzy search, and query parsing multi-byte aware. `PunctuationSet` keyed by codepoint; `min_stem_size` compared in codepoints; fuzzy DP matrix sized in codepoints; partial UTF-8 sequences buffered across radix-tree edges; `FilterParser` codepoint loops.
3. **[#7](https://github.com/VoletiRam/valkey-search/pull/7)** — Add `LanguageProcessor` interface, `SnowballProcessor` class, and `Language` enum. Foundational abstractions with no existing code paths modified.
4. **[#11](https://github.com/VoletiRam/valkey-search/pull/11)** — Implement `UnicodeNormalizer::Normalize()` and wire NFC into tokenization. NFKC for Arabic presentation-form folding.
5. **[#8](https://github.com/VoletiRam/valkey-search/pull/8)** — Add default stop words for Snowball languages (Apache Lucene lists) in `stop_words.h`.
6. **[#9](https://github.com/VoletiRam/valkey-search/pull/9)** — Add default punctuation for Snowball languages (Unicode Common Locale Data Repository) in `punctuation.h`.
7. **[#13](https://github.com/VoletiRam/valkey-search/pull/13)** — Run `add_language.sh` to generate the 11 new UTF-8 Snowball algorithm `.c/.h` files and register them in `libstemmer/modules.h`.
8. **[#16](https://github.com/VoletiRam/valkey-search/pull/16)** — Refactor `LanguageProcessor` into composable architecture (`Segmenter` / `TokenFilter` / `Normalizer` / `Stemmer` / `QueryTokenizer` primitives). Delete `Lexer`. Refactor query parser to use pluggable `QueryTokenizer`. This is the "processor PR" referenced above.
9. **[#17](https://github.com/VoletiRam/valkey-search/pull/17)** — Remove the debug-only config gate; multi-language is now controlled purely by the 1.4 version guard.
10. **[#19](https://github.com/VoletiRam/valkey-search/pull/19)** — Add RDB forward/backward compatibility tests for multi-language support (`test_rdb_cross_version_compat.py`).
11. **[#18](https://github.com/VoletiRam/valkey-search/pull/18)** — Extend the FTS compatibility test suite to cover new languages (multi-language corpora + regeneratable pickled expected answers).
12. **[#20](https://github.com/VoletiRam/valkey-search/pull/20)** — Move the `snowballstemmer` Python dependency behind pickle regeneration only, so the runtime compat test suite doesn't require it.

## Performance Testing and Benchmarking

The following report covers in detail compatibility divergences introduced from RediSearch, information retrieval benchmarking results, and performance testing reults:

[snowball_language_benchmarking.md](https://github.com/user-attachments/files/30328048/snowball_language_benchmarking.md)

