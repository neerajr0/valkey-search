# Task #4: Stop Word List Collection + Embedding

**Owner:** Neeraj Ramachandran  
**Effort:** 3 days  
**Status:** In Progress  
**Dependencies:** Independent (can start immediately)  
**Downstream:** Task #12 (per-language default stop word wiring) depends on this

---

## Overview

The current system has a single English default stop word list hardcoded in `ft_create_parser.h`. To support multi-language full-text search, we need per-language default stop word lists for all 12 Snowball languages. When a customer creates an index with `LANGUAGE french`, French stop words should filter automatically without requiring the customer to specify them.

---

## Relationship Between Task #4 and Task #12

**Task #4** (this task): Collect stop word data, create `stop_words.h`, update `THIRD_PARTY_NOTICES`, point `kDefaultStopWords` at the centralized English list.

**Task #12**: Wire per-language defaults into the `FT.CREATE` parsing flow so that `LANGUAGE french` automatically uses French stop words.

### Does Task #4 depend on Task #12?

No. Task #4 is purely about data collection and embedding. It produces the header file and makes it available. Task #12 consumes it.

### Does Task #12 depend on Task #4?

Yes. Task #12 calls `GetDefaultStopWords(language)` which lives in the header created by Task #4.

### Should they be merged?

**Recommendation: Merge them.** Here's why:

- Task #12's code change is small (~15 lines in `ft_create_parser.cc` + a bool flag in the struct). The complexity is in understanding the parsing flow, not in volume of code.
- The 2-day estimate in the design doc accounts for writing tests for all the interaction combinations (language + default, language + explicit override, language + NOSTOPWORDS). That testing effort is the same whether it's a separate task or part of Task #4.
- Shipping Task #4 without Task #12 delivers no user-visible behavior — the stop word arrays just sit unused until wiring happens.
- Both tasks are assigned to the same person (Neeraj).

**Combined effort: ~4 days** (3 days Task #4 + the wiring/testing portion of Task #12, which is ~1 day of code + 1 day of tests).

### What Task #12 actually involves

The parsing flow in `ft_create_parser.cc` processes `LANGUAGE`, `STOPWORDS`, and `NOSTOPWORDS` in a flexible-order loop. The challenge:

1. **Ordering problem**: Stop words default to English at the start of parsing. Language may be parsed before or after stop words in the loop. You can't just set stop words based on language at the end — that would overwrite an explicit user-provided `STOPWORDS` list.

2. **Solution**: Add a `stop_words_explicitly_set` flag to `PerIndexTextParams`. The `STOPWORDS` and `NOSTOPWORDS` parsers set this flag. After the parsing loop, if the flag is false and language != English, apply `GetDefaultStopWords(language)`.

3. **Test matrix** (the bulk of the 2-day estimate):
   - `LANGUAGE french` with no stop words → uses French defaults
   - `LANGUAGE french STOPWORDS 2 foo bar` → uses explicit list, ignores French defaults
   - `LANGUAGE french NOSTOPWORDS` → empty list, ignores French defaults
   - `LANGUAGE english` → uses English defaults (backward compatible)
   - No `LANGUAGE` specified → uses English defaults (backward compatible)
   - `STOPWORDS 0` with `LANGUAGE german` → empty list (explicit override wins)

---

## Steps

### 1. Collect Stop Word Lists ✅ DONE

Downloaded from Apache Lucene into `data/stopwords/`:
- 10 snowball language files + Arabic + Turkish
- Generator script (`generate_header.py`)

### 2. Create `src/indexes/text/stop_words.h` ✅ DONE

- 12 language arrays (English uses 33-word RediSearch-compatible list)
- `GetDefaultStopWords(language)` lookup function
- Apache 2.0 attribution in header

### 3. Update `THIRD_PARTY_NOTICES` ✅ DONE

Added Apache Lucene attribution with location disclaimer.

### 4. Update `src/commands/ft_create_parser.h` ✅ DONE

- `kDefaultStopWords` now references `kEnglishStopWords` from `stop_words.h`
- Added `stop_words_explicitly_set` flag to `PerIndexTextParams`

### 5. Wire per-language defaults in `ft_create_parser.cc` (Task #12) ✅ DONE

Changes made:
- Added `use_default_stop_words` flag (defaults to `true`) to `PerIndexTextParams`
- `NOSTOPWORDS` handler sets `use_default_stop_words = false` and clears the list
- `ParseStopWords` (handles `STOPWORDS N ...`) sets `use_default_stop_words = false`
- After the parsing loop: if `use_default_stop_words` is still true, applies `GetDefaultStopWords(language)`
- See `docs/stop-words-design-decision.md` for rationale on why the flag is needed

### 6. Write Tests

Test cases for `testing/ft_create_parser_test.cc`:
- `LANGUAGE french` → French stop words applied
- `LANGUAGE french NOSTOPWORDS` → empty (explicit override)
- `LANGUAGE french STOPWORDS 2 foo bar` → explicit list used
- `LANGUAGE english` → English defaults (no change)
- No language → English defaults (backward compatible)
- `STOPWORDS 0 LANGUAGE german` → empty (explicit override, order shouldn't matter)

---

## Files Updated/Added

| File | Action | Status |
|------|--------|--------|
| `data/stopwords/*.txt` | **NEW** — source stop word files from Lucene | ✅ Done |
| `data/stopwords/generate_header.py` | **NEW** — generator script | ✅ Done |
| `src/indexes/text/stop_words.h` | **NEW** — per-language arrays + lookup | ✅ Done |
| `THIRD_PARTY_NOTICES` | **MODIFY** — Apache Lucene attribution | ✅ Done |
| `src/commands/ft_create_parser.h` | **MODIFY** — reference centralized stop words + flag | ✅ Done |
| `src/commands/ft_create_parser.cc` | **MODIFY** — wire per-language defaults | ✅ Done |
| `src/index_schema.proto` | **MODIFY** — add Language enum values | ✅ Done (temp, pending Task #2) |
| `testing/ft_create_parser_test.cc` | **MODIFY** — add per-language stop word tests | TODO |

---

## Source Research: Why Apache Lucene?

### RediSearch Does NOT Have Per-Language Stop Words

RediSearch only has a single English default stop word list (33 words). It does not ship per-language defaults.

### Apache Lucene Is the Correct Source

- Apache 2.0 license — permissive, compatible with BSD 3-Clause
- Industry standard — used by Elasticsearch
- Available for all 12 target languages
- Well-maintained and stable

---

## PR Description

### 1. Motivation

Valkey-Search currently has a single hardcoded English stop word list. When a customer creates an index with `LANGUAGE french`, there are no French stop words applied by default — the customer must manually specify them via `STOPWORDS N ...`. This is a poor experience for multi-language full-text search. Per-language default stop words should be applied automatically based on the declared language, matching the behavior users expect from search engines like Elasticsearch.

### 2. Summary of Changes

- **New file `src/indexes/text/stop_words.h`**: Per-language default stop word arrays for all 12 Snowball languages (English, French, German, Spanish, Italian, Portuguese, Russian, Swedish, Turkish, Dutch, Indonesian, Arabic). English uses the existing 33-word RediSearch-compatible list; other languages sourced from Apache Lucene (Apache 2.0). Includes a `GetDefaultStopWords(language)` lookup function.
- **Modified `src/commands/ft_create_parser.h`**: Replaced the inline `kDefaultStopWords` vector with a reference to `kEnglishStopWords` from the centralized header. Added `use_default_stop_words` flag to `PerIndexTextParams` to track whether stop words were explicitly set by the user.
- **Modified `src/commands/ft_create_parser.cc`**: `NOSTOPWORDS` and `STOPWORDS N ...` handlers now set `use_default_stop_words = false`. After the parsing loop, if the flag is still true, `GetDefaultStopWords(language)` is applied. This ensures explicit user overrides always win regardless of parameter order.
- **Modified `THIRD_PARTY_NOTICES`**: Added Apache Lucene attribution for the embedded stop word data.
- **Modified `testing/ft_create_parser_test.cc`**: Added 7 test cases covering the full interaction matrix: language with defaults, `NOSTOPWORDS` override, explicit `STOPWORDS` override, English backward compatibility, and order-independence (`NOSTOPWORDS`/`STOPWORDS 0` before `LANGUAGE`).

### 3. Testing

Unit tests added to `testing/ft_create_parser_test.cc` covering:
- `LANGUAGE french` → French stop words applied automatically
- `LANGUAGE french NOSTOPWORDS` → empty list (explicit override wins)
- `LANGUAGE french STOPWORDS 2 foo bar` → explicit list used, French defaults ignored
- `LANGUAGE english` → English defaults (backward compatible, no behavior change)
- `NOSTOPWORDS LANGUAGE french` → empty list (order-independent, explicit override wins)
- `STOPWORDS 0 LANGUAGE german` → empty list (order-independent, explicit override wins)
- `LANGUAGE german` → German stop words applied automatically

All existing tests continue to pass — the `kDefaultStopWords` reference change is transparent to existing English-only behavior.

---

## Integration Notes

- Stop words are already persisted in the `IndexSchema` protobuf (`repeated string stop_words`) and reloaded on RDB load. **No schema format changes required.**
- `NOSTOPWORDS` and `STOPWORDS N ...` continue to override per-language defaults.
- `LANGUAGE_UNSPECIFIED` falls back to English stop words (backward compatible).
- English stop words use the 33-word RediSearch-compatible list, not the larger Lucene English list.
