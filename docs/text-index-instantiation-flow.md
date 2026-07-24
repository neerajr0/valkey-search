# FT.CREATE → TextIndex / Lexer Instantiation Flow

This documents the call chain from the `FT.CREATE` command handler down to where `TextIndex` and `Lexer` objects are actually instantiated.

## Call Chain

```
1. FTCreateCmd()                           [src/commands/ft_create.cc]
       │
       │ calls
       ▼
2. ParseFTCreateArgs(ctx, argv, argc)      [src/commands/ft_create_parser.cc]
       │
       │  Internally does:
       │    - Parses index name, ON (HASH/JSON), PREFIX, LANGUAGE
       │    - Parses schema-level text params:
       │        PUNCTUATION, WITHOFFSETS/NOOFFSETS, NOSTOPWORDS/STOPWORDS,
       │        NOSTEM, MINSTEMSIZE
       │    - Parses SCHEMA section, calling ParseText() for TEXT fields
       │        → populates data_model::TextIndex proto per field
       │
       │ returns data_model::IndexSchema proto to caller (step 1)
       │
       │ (back in FTCreateCmd)
       │ calls
       ▼
3. SchemaManager::CreateIndexSchemaInternal(ctx, proto)  [src/schema_manager.cc]
       │
       │ calls
       ▼
4. IndexSchema::Create(ctx, proto, ...)    [src/index_schema.cc]
       │
       │  Internally does:
       │    - Reads language_, punctuation_, stop_words_, min_stem_size_,
       │      with_offsets_ from proto
       │    - Iterates over attributes; on first TEXT field encountered:
       │
       │      calls
       ▼
5. IndexSchema::CreateTextIndexSchema()    [src/index_schema.h]
       │
       │ constructs (via make_shared)
       ▼
6. TextIndexSchema(language, punctuation, with_offsets, stop_words, min_stem_size)
                                           [src/indexes/text/text_index.cc]
       │
       │  Member-initializes (in initializer list):
       │
       │    (a) lexer_(language, punctuation, stop_words)
       │         → Lexer constructor builds punct_bitmap_ and stop_words_set_
       │         → Stemmers are thread-local, created lazily on first Tokenize()
       │
       │    (b) text_index_ = make_shared<TextIndex>(false)
       │         → TextIndex constructor creates prefix radix tree
       │         → suffix tree is null unless EnableSuffix() is called later
```

## Legend

| Label | Meaning |
|-------|---------|
| **calls** | This function directly invokes the next function |
| **returns ... to caller** | Control flows back up to the calling function |
| **Internally does** | Actions happening within the same function (not a separate call site) |
| **constructs (via make_shared)** | The function creates a new heap-allocated object |
| **Member-initializes** | Object members initialized in the C++ constructor initializer list |

## Key Observations

- `ft_create_parser.cc` only produces a protobuf (`data_model::IndexSchema`). No runtime index or lexer objects are created during parsing.
- The proto flows through `SchemaManager` → `IndexSchema::Create()` → `CreateTextIndexSchema()` before any runtime objects exist.
- There is one `TextIndexSchema` per `IndexSchema` (created on the first TEXT field). All TEXT fields within the same index share it.
- The `Lexer` lives as a value member of `TextIndexSchema`. Its language, punctuation, and stop words are fixed at construction time.
- `TextIndex` (the radix tree container) is default-constructed without suffix support. If any field specifies `WITHSUFFIXTRIE`, `EnableSuffix()` recreates it with `TextIndex(true)`.
