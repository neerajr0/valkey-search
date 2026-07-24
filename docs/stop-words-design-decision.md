# Design Decision: `use_default_stop_words` Flag

## Problem

The FT.CREATE parser processes parameters in flexible order. A user can write any of:

```
FT.CREATE idx ON HASH LANGUAGE french SCHEMA title TEXT
FT.CREATE idx ON HASH NOSTOPWORDS LANGUAGE french SCHEMA title TEXT
FT.CREATE idx ON HASH LANGUAGE french STOPWORDS 2 foo bar SCHEMA title TEXT
```

We need to apply per-language default stop words (e.g., French defaults when `LANGUAGE french` is specified), but only if the user didn't explicitly override them.

## Why This Wasn't a Problem With English Only

Previously, the default was hardcoded at initialization:

```cpp
schema_text_defaults.stop_words = kDefaultStopWords;  // Always English
```

This worked because:
1. Set English defaults before the loop
2. If user says `NOSTOPWORDS` or `STOPWORDS N ...`, it overwrites during the loop
3. If not, English defaults remain

There was only one possible default, so it was always correct regardless of parsing order.

## Why It's a Problem Now

The correct default now depends on the `LANGUAGE` parameter — which is parsed *during* the same flexible-order loop as `STOPWORDS`/`NOSTOPWORDS`. You can't set the right default before the loop because you don't know the language yet.

## Alternatives Considered

**Set default based on language at initialization, override in loop:**
Not possible — language isn't known at initialization. It's parsed in the same loop.

**Re-apply language-specific defaults after parsing language:**
Breaks the case where `NOSTOPWORDS` appears before `LANGUAGE` in the command. You'd overwrite the user's explicit choice.

**Enforce strict parameter ordering (language before stop words):**
Would break the existing flexible-order design and diverge from RediSearch behavior where parameters can appear in any order.

## Solution: Deferred Default With a Flag

Add `use_default_stop_words` (defaults to `true`) to `PerIndexTextParams`. Both `NOSTOPWORDS` and `STOPWORDS N ...` set it to `false`. After the parsing loop completes:

```cpp
if (schema_text_defaults.use_default_stop_words) {
  schema_text_defaults.stop_words = indexes::text::GetDefaultStopWords(
      schema_text_defaults.language);
}
```

This is a standard "deferred default" pattern: track whether the user touched a field, then apply the smart default at the end if they didn't.

## Naming

`use_default_stop_words` was chosen over `stop_words_explicitly_set` because:
- `stop_words_explicitly_set` sounds like it only refers to `STOPWORDS N ...`, not `NOSTOPWORDS`
- `use_default_stop_words` reads naturally at the point of use: "if we should use default stop words, apply per-language defaults"
- In the parsers, `use_default_stop_words = false` clearly means "the user overrode the defaults"
