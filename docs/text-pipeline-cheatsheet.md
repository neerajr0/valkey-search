# Text Pipeline & Query Parsing Cheat Sheet

Quick reference for the core classes involved in index creation, text ingestion, and query parsing.

---

## Classes at a Glance

| Class / Function | File(s) | One-liner |
|------------------|---------|-----------|
| Scanner | `src/utils/scanner.h` | Low-level UTF-8 decoder and byte iterator |
| Lexer | `src/indexes/text/lexer.h`, `lexer.cc` | Stateless text tokenizer (split, normalize, stem) |
| FuzzySearch | `src/indexes/text/fuzzy.h` | Approximate string matching via Damerau-Levenshtein on RadixTree |
| FilterParser | `src/commands/filter_parser.h`, `filter_parser.cc` | Parses FT.SEARCH query strings into a predicate tree |
| FTCreateCmd | `src/commands/ft_create.cc` | Valkey module command handler — orchestrates the full FT.CREATE flow |
| FTCreateParser | `src/commands/ft_create_parser.h`, `ft_create_parser.cc` | Parses FT.CREATE command args into IndexSchema protobuf |
| IndexSchema | `src/index_schema.h`, `index_schema.cc` | Runtime representation of a search index (owns fields, manages mutations) |

---

## Scanner (`src/utils/scanner.h`)

Foundation utility for all Unicode-aware text processing. Provides:

- Position-tracked iteration over a `string_view`
- `NextUtf8()` — decode one code point, returns `kInvalidCp` for malformed sequences
- `IsValidUtf8(text)` — single validation routine used as the ingestion gate
- `AtLeastNCodepoints(text, n)` — short-circuit threshold check
- `PushBackUtf8(s, cp)` — encode a code point back to UTF-8
- `ExpectedLen(b0)` — bytes expected for a UTF-8 lead byte

No external dependencies. Used by Lexer, FuzzySearch, and FilterParser.

---

## Lexer (`src/indexes/text/lexer.h`)

Stateless tokenizer configured per-index. One instance lives in `TextIndexSchema`, constructed at `FT.CREATE` time with the index's punctuation, language, and stop words.

### Full tokenization pipeline (`Lexer::Tokenize`)

Called by the **ingestion path** (`TextIndexSchema::StageAttributeData`). Runs the complete pipeline on raw document text:

1. **Split** on punctuation characters (configurable `PunctuationSet`, uses Scanner)
2. **Normalize** to lowercase (ASCII fast path; ICU NFC + case-fold for non-ASCII)
3. **Filter** stop words
4. **Stem** using Snowball (thread-local cached stemmers per language)

### Individual primitives (used by the query path)

The **FilterParser does NOT call `Lexer::Tokenize()`**. It implements its own character-by-character scanning loop to parse query syntax (`|`, `*`, `%`, `"`, `@`, `-`, `(`, `)`) and build a predicate tree. However, it calls the **same Lexer instance's utility methods** to ensure tokens are normalized identically to ingestion:

| Method | Called by FilterParser when... |
|--------|-------------------------------|
| `IsPunctuation(cp)` | Scanning codepoints to detect token boundaries mid-parse |
| `NormalizeLowerCaseInPlace(word)` | A token has been fully extracted and needs to match indexed form |
| `IsStopWord(word)` | Deciding whether to emit a predicate or skip the token |
| `StemWordInPlace(word, stemmer)` | Query expansion needs stem variants (`GetAllStemVariants`) |

### Why two usage modes instead of one shared call

The ingestion path processes raw text with no syntax — `Tokenize()` runs end-to-end. The query parser processes structured expressions like `@title:(hello|world) -"stop this" run*` where operators, wildcards, and fuzzy markers have meaning. Calling `Tokenize()` on that would destroy the query semantics.

The consistency guarantee is preserved because both paths use the same `PunctuationSet`, the same normalization function, and the same stop-word set from the same Lexer instance.

### Key types

- `PunctuationSet` — bitset for ASCII, hash set for non-ASCII code points
- `InProgressStemMap` — stemmed_word → list of original variants (for reverse lookup)

---

## FuzzySearch (`src/indexes/text/fuzzy.h`)

Performs approximate matching on the RadixTree (Rax) that stores indexed terms.

- Uses Damerau-Levenshtein distance (supports transposition)
- DP matrix operates on **code points** (not bytes), so `é→è` = 1 edit
- Prunes subtrees where `min_dist > max_distance`
- Returns `Postings::KeyIterator`s for all matching words

Invoked at query time when FilterParser encounters `%word%` (fuzzy syntax).

---

## FilterParser (`src/commands/filter_parser.h`)

Parses an FT.SEARCH query expression into a `query::Predicate` tree. Supports:

- **Numeric ranges**: `@field:[1 10]`
- **Tags**: `@field:{tag1|tag2}`
- **Text terms**: `word`, prefix `word*`, suffix `*word`, fuzzy `%word%`
- **Exact phrase**: `"word1 word2"`
- **Logical ops**: AND (implicit between terms), OR (`|`), NOT (`-`)
- **Grouping**: parentheses for precedence
- **Match-all**: `*`

### How it extracts text tokens (without calling `Lexer::Tokenize`)

The parser's token-building loop (`ParseUnquotedTextToken`, `ParseQuotedTextToken`) scans the query expression codepoint-by-codepoint:

```
@title:(running* world) → two separate predicate leaves:
  1. PrefixPredicate("running")  — detected trailing '*'
  2. TermPredicate("world")      — plain term
```

At each codepoint it checks:
- Is it a query syntax character (`|`, `(`, `)`, `@`, `-`, `"`)? → break/handle
- Is it a wildcard/fuzzy marker (`*`, `%`)? → flag prefix/suffix/fuzzy mode
- Is it a backslash? → escape handling
- Is it punctuation per `lexer.IsPunctuation(cp)`? → token boundary

After extracting a complete token string, it calls `lexer.NormalizeLowerCaseInPlace()` and `lexer.IsStopWord()` to bring the token into the same form that was indexed during ingestion.

### Dependencies

- `IndexSchema` — validate field names, look up index types
- `Lexer` (via `TextIndexSchema`) — `IsPunctuation`, `NormalizeLowerCaseInPlace`, `IsStopWord` for token consistency with ingestion
- `Scanner` — code-point-level parsing of the query expression

Output: `FilterParseResults` containing the root predicate, filter identifiers, and flags for which query operations are used.

---

## FTCreateCmd (`src/commands/ft_create.cc`)

The top-level Valkey module command handler for `FT.CREATE`. This is the entry point when a client issues the command — it ties together parsing, validation, schema creation, and cluster coordination.

### Execution flow

1. **Parse** — calls `ParseFTCreateArgs()` (from FTCreateParser) to convert raw `argv` into a `data_model::IndexSchema` protobuf
2. **Set DB** — stamps the protobuf with the currently selected database number
3. **ACL check** — calls `AclPrefixCheck` to verify the caller has write access to the declared key prefixes
4. **Create schema** — calls `SchemaManager::Instance().CreateIndexSchema(ctx, proto)` which builds the runtime `IndexSchema`, subscribes to keyspace events, and returns a fingerprint/version pair
5. **Reply / Cluster consistency** — in cluster mode (with coordinator), initiates a `CreateConsistencyCheckFanoutOperation` that fans out to other nodes to verify the index was created consistently; in standalone/loading/multi-exec mode, replies `OK` directly
6. **Replication** — in non-coordinator (CMD) clusters, calls `ValkeyModule_ReplicateVerbatim()` so replicas see the same command; CME clusters replicate via `FT.INTERNAL_UPDATE` with metadata versioning instead

### CreateConsistencyCheckFanoutOperation

An internal helper class (defined in the same file) that inherits from `ClusterInfoFanoutOperation`. After a successful local create, it sends an `InfoIndexPartitionRequest` carrying the new fingerprint/version to all cluster nodes, confirming they agree on the index definition.

### Dependencies

- `FTCreateParser` (`ParseFTCreateArgs`) — argument parsing
- `SchemaManager` — registry of all indexes; performs the actual creation
- `ACL` — prefix-level permission checks
- `ValkeySearch` — cluster/coordinator mode detection
- `ClusterInfoFanoutOperation` — cluster consistency protocol

---

## FTCreateParser (`src/commands/ft_create_parser.h`)

Parses the `FT.CREATE` command arguments into a `data_model::IndexSchema` protobuf.

Example command:
```
FT.CREATE my-index ON HASH PREFIX 1 doc: LANGUAGE english STOPWORDS 3 a the is
  SCHEMA title TEXT WEIGHT 2.0 NOSTEM price NUMERIC tags TAG SEPARATOR ","
```

Handles:
- Index-level: name, ON type (HASH/JSON), PREFIX, LANGUAGE, SCORE, STOPWORDS, PUNCTUATION, MINSTEMSIZE, NOOFFSETS, SKIPINITIALSCAN
- Per-field: VECTOR (HNSW/FLAT + params), TAG (separator, case-sensitive), NUMERIC, TEXT (WITHSUFFIXTRIE, NOSTEM, WEIGHT)

Output protobuf is consumed by `IndexSchema::Create()`. The parser itself does **not** create indexes.

---

## IndexSchema (`src/index_schema.h`)

The central runtime object for a search index. Responsibilities:

- Owns all `Attribute`s (each wrapping a Tag, Numeric, Text, or Vector index)
- Creates and holds the `TextIndexSchema` (shared Lexer config for text fields)
- Subscribes to keyspace events (document add/modify/delete in Valkey)
- Orchestrates mutation processing (tokenize → update RadixTree/posting lists)
- Manages backfill scanning of pre-existing keys
- Tracks per-document scores, mutation sequence numbers, field masks
- Handles RDB save/load for persistence
- Provides field lookups used by FilterParser during query parsing

---

## Data Flow

### Lexer API Surface

```
Lexer (one instance per TextIndexSchema, created at FT.CREATE)
│
├── Tokenize(text, stem, min_stem_size, stem_map)
│     ↑ Called by ingestion path (StageAttributeData)
│     └── Full pipeline: split → normalize → stop-word → stem
│
├── IsPunctuation(cp)
│     ↑ Called by FilterParser mid-scan (per codepoint)
│
├── NormalizeLowerCaseInPlace(word)
│     ↑ Called by FilterParser after extracting a token
│
├── IsStopWord(word)
│     ↑ Called by FilterParser to decide emit vs. skip
│
├── StemWordInPlace(word, stemmer, min_stem_size)
│     ↑ Called by query expansion (GetAllStemVariants)
│
└── GetStemmer()
      ↑ Called by ingestion + query expansion
```

### End-to-End Flows

```
FT.CREATE command
    │
    ▼
┌──────────────────┐
│   FTCreateCmd     │──→ Entry point (ACL check, DB stamp, replication)
│  (ft_create.cc)   │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│  FTCreateParser   │──→ data_model::IndexSchema (protobuf)
└────────┬─────────┘     (punctuation, stopwords, language, field configs)
         │
         ▼
┌──────────────────┐
│  SchemaManager    │──→ Creates runtime IndexSchema + fingerprint/version
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│   IndexSchema     │──→ Creates TextIndexSchema (owns Lexer instance)
│   (runtime)       │    Owns all field indexes (Tag, Numeric, Text, Vector)
└────────┬─────────┘
         │
         │  On document ingestion (keyspace notification):
         ▼
┌──────────────────┐
│  Lexer::Tokenize  │──→ Full pipeline on raw document text
│                   │    (split → normalize → stop-word → stem)
└──────────────────┘
         │
         │  Tokens stored in RadixTree + PostingLists
         ▼
─────────────────────────────────────────────────────────

FT.SEARCH command
    │
    ▼
┌──────────────────┐
│  FilterParser     │──→ Parses query syntax character-by-character
│                   │    Uses Lexer primitives (IsPunctuation,
│                   │    NormalizeLowerCaseInPlace, IsStopWord)
│                   │    Does NOT call Lexer::Tokenize
│                   │    Produces predicate tree
└────────┬─────────┘
         │
         │  For fuzzy queries (%word%):
         ▼
┌──────────────────┐
│   FuzzySearch     │──→ Traverses RadixTree using Scanner (code-point DP)
│                   │    Returns posting list iterators
└──────────────────┘

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        Scanner — UTF-8 foundation used by all above
```

---

## Key Design Principles

1. **One Lexer, two usage modes** — Ingestion calls `Lexer::Tokenize()` for the full pipeline. The query parser calls individual primitives (`IsPunctuation`, `NormalizeLowerCaseInPlace`, `IsStopWord`) because it must parse query syntax that `Tokenize()` knows nothing about.
2. **Consistency is structural** — Both paths use the same Lexer instance (same punctuation set, same normalization, same stop words), so query tokens always match indexed tokens.
3. **Code-point semantics** — Scanner decodes UTF-8 everywhere so multi-byte characters are never confused with ASCII punctuation or treated as multiple edits.
4. **Stateless Lexer** — configuration lives in `TextIndexSchema`; the Lexer itself holds no per-document state.
5. **Thread-local stemmers** — Snowball stemmers aren't thread-safe, so each worker thread caches its own per-language instance.
6. **Separation of parsing and execution** — FTCreateParser produces a protobuf (data), FilterParser produces a predicate tree (logic). Neither directly touches index storage.
