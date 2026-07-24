# Multi-Language Support: Architecture Changes

## Overview

This document describes how the valkey-search text indexing architecture changes with the introduction of `ILanguageProcessor`, `SnowballProcessor`, and `ICUSegmenter` to support 12+ languages beyond English.

Reference: [Multi-language support plan - low level details](https://quip-amazon.com/03HdAmpInFuD/Multi-language-support-plan-low-level-details)

---

## Current Architecture

Today, the tokenization pipeline is tightly coupled to English. The `Lexer` class handles everything — punctuation splitting, normalization, stop word filtering, and stemming — in a single hardcoded code path.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        CURRENT ARCHITECTURE                                  │
└─────────────────────────────────────────────────────────────────────────────┘

 FT.CREATE ("LANGUAGE ENGLISH")
       │
       ▼
 ┌──────────────────┐       ┌──────────────────────────────────────────────┐
 │  ft_create_parser │──────▶│  TextIndexSchema                             │
 │                    │       │    - owns Lexer(language, punct, stop_words) │
 │  kLanguageByStr:   │       │    - owns stem_tree_ (Rax)                  │
 │  {"ENGLISH" only   │       │    - owns text_index_ (Rax prefix/suffix)   │
 │   wired through}   │       └───────────────┬────────────────────────────┘
 └──────────────────┘                         │
                                              │ uses
                                              ▼
                              ┌───────────────────────────────────┐
                              │           Lexer                     │
                              │                                     │
                              │  Tokenize(text, stem, min_stem_size)│
                              │    1. byte-by-byte char iteration   │
                              │    2. Split on punctuation bitmap   │
                              │    3. NormalizeLowerCaseInPlace()   │
                              │       ├─ ASCII → absl::AsciiLower  │
                              │       └─ Non-ASCII → ICU CaseFold  │
                              │    4. Stop word filter              │
                              │    5. sb_stemmer (English only)     │
                              │       └─ thread-local cache         │
                              └───────────────┬────────────────────┘
                                              │
                    ┌─────────────────────────┼────────────────────────┐
                    │                         │                        │
                    ▼                         ▼                        ▼
        ┌──────────────────┐   ┌──────────────────┐    ┌──────────────────┐
        │StageAttributeData│   │GetAllStemVariants │    │  DeleteKeyData   │
        │(index-time)      │   │(query-time stem   │    │(cleanup stems)   │
        │                  │   │ expansion)        │    │                  │
        └──────────────────┘   └──────────────────┘    └──────────────────┘

 Query Path:
 ┌──────────────────┐         ┌──────────────────┐
 │  filter_parser.cc │────────▶│  Lexer            │
 │  ParseUnquoted/   │  uses   │  .IsPunctuation() │
 │  ParseQuoted      │         │  .NormalizeLower() │
 │  Token            │         │  .IsStopWord()     │
 └──────────────────┘         └──────────────────┘
```

### Key Issues with Current Architecture

- `Lexer` processes text byte-by-byte (incorrect for multi-byte UTF-8)
- `sb_stemmer` hardcoded to English via `GetLanguageString()`
- No NFC normalization (`UnicodeNormalizer::Normalize()` is empty)
- No word segmentation for non-space-delimited languages (CJK)
- Fuzzy search DP matrix sized by bytes, not code points
- `min_stem_size` checks `word.length()` which counts bytes, not characters

---

## New Architecture (with ILanguageProcessor)

The key change is introducing a **strategy pattern** via `ILanguageProcessor` that decouples language-specific behavior from the `Lexer`. The Lexer retains language-neutral orchestration (UTF-8 validation, stop word check) but delegates tokenization and stemming to a polymorphic processor.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        NEW ARCHITECTURE                                      │
└─────────────────────────────────────────────────────────────────────────────┘

 FT.CREATE ("LANGUAGE french" / "LANGUAGE zh")
       │
       ▼
 ┌──────────────────┐      ┌────────────────────────────────────────────────┐
 │  ft_create_parser │─────▶│  TextIndexSchema                               │
 │                    │      │    - creates processor via Factory             │
 │  kLanguageByStr:   │      │    - owns shared_ptr<ILanguageProcessor>       │
 │  12 Snowball langs │      │    - owns Lexer(processor, punct, stop_words)  │
 │  + 5 CJKPV langs  │      │    - owns stem_tree_ (Rax)                    │
 └──────────────────┘      └───────────────┬────────────────────────────────┘
                                            │
                                            │ creates via
                                            ▼
                           ┌─────────────────────────────────────┐
                           │   LanguageProcessorFactory::Create() │
                           │     switch(language) →                │
                           │       Snowball langs → SnowballProc  │
                           │       CJK/PV langs  → ICUSegmenter   │
                           └────────────────┬────────────────────┘
                                            │
                     ┌──────────────────────┼──────────────────────┐
                     ▼                                             ▼
 ┌──────────────────────────────────┐        ┌──────────────────────────────────┐
 │     SnowballProcessor             │        │        ICUSegmenter               │
 │  (EN, FR, DE, ES, IT, PT,         │        │  (ZH, JA, KO, PL, VI)            │
 │   RU, SV, TR, NL, ID, AR)         │        │                                  │
 │                                    │        │  Tokenize():                     │
 │  Tokenize():                       │        │    icu::BreakIterator            │
 │    1. Punct/space split            │        │    ::createWordInstance(locale)   │
 │    2. NFC normalize (NFKC for AR)  │        │    (thread-local cached)         │
 │    3. Case fold                    │        │                                  │
 │                                    │        │  BuildStemMap(): NO-OP           │
 │  BuildStemMap():                   │        │  SupportsStemming(): false        │
 │    sb_stemmer per language          │        │  DefaultPunctuation(): empty     │
 │    (thread-local cache)             │        │    (BreakIterator handles it)    │
 │                                    │        │                                  │
 │  SupportsStemming(): true          │        └──────────────────────────────────┘
 │  DefaultPunctuation():             │
 │    ASCII default + Arabic extras   │
 └──────────────────────────────────┘
```

### ILanguageProcessor Interface

```cpp
class ILanguageProcessor {
 public:
  // Segment text into tokens. For Snowball: punct/space split + normalize.
  // For ICU: BreakIterator word boundaries. Returns normalized, lowercased tokens.
  virtual std::vector<std::string> Tokenize(absl::string_view text) const = 0;

  // Build the stem map for all tokens in a document. Called once per document.
  // No-op for ICU languages.
  virtual void BuildStemMap(const std::vector<std::string>& tokens,
                            uint32_t min_stem_size,
                            InProgressStemMap& stem_mappings) const {}

  // Default punctuation characters for this language.
  // Arabic extends ASCII defaults with Arabic-specific punctuation.
  // CJK returns empty — ICU BreakIterator handles punctuation natively.
  virtual const std::string& DefaultPunctuation() const = 0;

  // Whether this processor produces stems (controls stem tree construction).
  virtual bool SupportsStemming() const { return false; }

  virtual ~ILanguageProcessor() = default;
};
```

### Interface Hierarchy

```
ILanguageProcessor
  ├── SnowballProcessor   → English, French, German, Spanish, Italian,
  │                         Portuguese, Russian, Swedish, Turkish,
  │                         Dutch, Indonesian, Arabic
  │
  └── ICUSegmenter        → Chinese, Japanese, Korean, Polish, Vietnamese
```

---

## Tokenization Flow: Before and After

### Before

```
text → Lexer (punct split, byte-by-byte) → NormalizeLowerCase → sb_stemmer [English only]
```

### After

```
                    ┌─ SnowballProcessor → punct split → NFC normalize → sb_stemmer
text → Lexer ───────┤
                    └─ ICUSegmenter → BreakIterator word boundaries → tokens
                       [CJKPV only]
```

---

## Updated Lexer (Simplified Role)

```
┌────────────────────────────────────────────────────────┐
│  Lexer                                                   │
│    - holds shared_ptr<ILanguageProcessor>                │
│    - Utf8Iterator (code-point aware iteration)          │
│    - UTF-8 validation                                    │
│    - Stop word filtering                                 │
│    - Delegates Tokenize → processor_->Tokenize()        │
│    - Delegates stemming → processor_->BuildStemMap()     │
└────────────────────────────────────────────────────────┘
```

---

## Query Path Changes (CJK)

```
 ┌──────────────────────┐         ┌─────────────────────────────────────┐
 │  filter_parser.cc     │────────▶│  ILanguageProcessor                  │
 │  ParseUnquotedToken   │  calls  │  .Tokenize(query_term)               │
 │                        │         │                                       │
 │  IF ICUSegmenter AND   │         │  If N > 1 tokens from CJK term:      │
 │  produces N>1 tokens:  │         │    → Build ComposedPredicate(AND)     │
 │  build AND predicate   │         │  Otherwise:                           │
 └──────────────────────┘         │    → Single TermPredicate (as today)  │
                                   └─────────────────────────────────────┘
```

For a Chinese query like `北京大学`, the parser today produces one `TermPredicate("北京大学")` which matches nothing. After the change, `ICUSegmenter::Tokenize()` segments it into `["北京", "大学"]`, and the parser builds a `ComposedPredicate(AND)` — the same structure as an unquoted multi-word English query.

---

## Supporting Infrastructure Changes

```
┌────────────────────────────────────────────────────────────────────────┐
│  Utf8Iterator (new)        │  Prerequisite for all changes              │
│    - Next() → one code point per call                                   │
│    - Fixes min_stem_size (byte count → code point count)               │
│    - Fixes fuzzy.h DP matrix (byte-level → code-point-level)           │
├─────────────────────────────┼──────────────────────────────────────────┤
│  UnicodeNormalizer          │  Normalize() implemented (NFC/NFKC)      │
│    - NFC for all Snowball langs                                         │
│    - NFKC for Arabic (presentation forms)                               │
│    - Combined with case-fold in single ICU pass                         │
├─────────────────────────────┼──────────────────────────────────────────┤
│  Snowball third_party/      │  11 new .c/.h files generated             │
│    - add_language.sh run    │  CMakeLists updated with LTO preserved    │
├─────────────────────────────┼──────────────────────────────────────────┤
│  index_schema.proto         │  Language enum: 12 values (done)          │
│                              │  + 5 CJK/PV values (if scoped)           │
├─────────────────────────────┼──────────────────────────────────────────┤
│  RDB Versioning             │  kRelease14: non-English schemas          │
│                              │  rejected by older binaries              │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Summary of Key Architectural Changes

| Aspect | Current | After |
|--------|---------|-------|
| **Language routing** | Hardcoded English in `Lexer` | `ILanguageProcessor` polymorphism via factory |
| **Tokenization** | Byte-by-byte char loop in Lexer | `Utf8Iterator` + processor's `Tokenize()` |
| **Stemming** | `sb_stemmer("english")` directly in Lexer | `SnowballProcessor` wraps stemmer per language; ICUSegmenter has no-op |
| **Normalization** | Only case-fold (ASCII fast path + ICU) | NFC (all Snowball) / NFKC (Arabic) before stemming |
| **CJK tokenization** | Impossible (splits on whitespace only) | `ICUSegmenter` uses `BreakIterator` dictionary-based segmentation |
| **CJK query parsing** | Single term → single lookup | Single CJK term → N tokens → AND predicate |
| **Fuzzy search** | DP matrix sized by bytes | DP matrix sized by code points |
| **min_stem_size** | Checks `word.length()` (bytes) | Checks code point count via `Utf8Iterator` |
| **Processor lifecycle** | N/A | Created once at schema construction, shared via `shared_ptr` across Lexer copies |
| **Stop words** | English defaults only | Per-language default lists (Lucene-sourced) |

---

## Callsites Affected

The `TextIndexSchema` calls into `Lexer` in three places that now route through the processor interface:

1. **`StageAttributeData()`** — index-time tokenization
2. **`GetAllStemVariants()`** — stem expansion during search
3. **`DeleteKeyData()`** — cleanup on key deletion

For ICU languages, `SupportsStemming()` returns false and the stem expansion path is skipped entirely.

---

## Design Decisions

- **Token positions for ICU languages**: `BreakIterator` returns byte boundaries; these are mapped to sequential token indices (0, 1, 2...) the same way the existing punct-split loop assigns positions. Phrase queries work correctly.
- **`NOSTEM` flag on ICU-language fields**: Accepted at `FT.CREATE` time without error, but has no effect — stemming is always disabled for ICU-segmented languages.
- **Processor lifecycle**: Created once at `TextIndexSchema` construction, stored by value inside `Lexer`, shared via `shared_ptr` when `Lexer` is copied (e.g., `GetLexer()` in `FilterParser`).
- **`PUNCTUATION` parameter for CJK**: Accepted but ignored (warning logged). `BreakIterator` handles punctuation natively.
- **Extensibility**: Adding a new language means implementing a new `ILanguageProcessor` subclass and registering it in the factory. The core pipeline remains unchanged.
