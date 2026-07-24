# Multi-Language Feature Gate Design

## Overview

Gate non-English text index creation to valkey-search version 1.4+. Any attempt to create, replicate, or load a non-English text index on a version < 1.4 node should be rejected with a clear error message.

## Current State

The codebase already has foundational pieces:

1. **Proto schema** (`src/index_schema.proto`): Defines 12 languages (ENGLISH through ARABIC)
2. **Version constants** (`src/version.h`): `kRelease14` is defined for multi-language full text search
3. **`GetMinVersion()`** (`src/index_schema.cc`): Already computes that non-English text indexes require `kRelease14`
4. **Parser** (`src/commands/ft_create_parser.cc`): `kLanguageByStr` only maps `"ENGLISH"` — other languages are rejected at parse time with a generic "Unknown argument" error

### Current Error (unhelpful)

```
(error) Bad arguments for LANGUAGE: Unknown argument `FRENCH`
```

### Proposed Error (clear)

```
(error) Non-English text indexes require valkey-search version 1.4 or later
```

## What's Missing

There is no explicit version-aware feature gate that gives a clear error message. The current rejection is a side effect of the parser not knowing about other languages. Once languages are added to the parser map (required for 1.4), a proper gate is needed.

## Design

The gate operates at two levels:

### 1. Create-time gate (FT.CREATE)

**Where:** `src/commands/ft_create_parser.cc`

**Approach:**

- Expand `kLanguageByStr` to include all supported languages
- After parsing the language, check if it's non-English and if `kModuleVersion < kRelease14`
- Return a clear error

```cpp
// Expand the language map
const absl::NoDestructor<
    absl::flat_hash_map<absl::string_view, data_model::Language>>
    kLanguageByStr({
        {"ENGLISH", data_model::LANGUAGE_ENGLISH},
        {"FRENCH", data_model::LANGUAGE_FRENCH},
        {"GERMAN", data_model::LANGUAGE_GERMAN},
        {"SPANISH", data_model::LANGUAGE_SPANISH},
        {"ITALIAN", data_model::LANGUAGE_ITALIAN},
        {"PORTUGUESE", data_model::LANGUAGE_PORTUGUESE},
        {"RUSSIAN", data_model::LANGUAGE_RUSSIAN},
        {"SWEDISH", data_model::LANGUAGE_SWEDISH},
        {"TURKISH", data_model::LANGUAGE_TURKISH},
        {"DUTCH", data_model::LANGUAGE_DUTCH},
        {"INDONESIAN", data_model::LANGUAGE_INDONESIAN},
        {"ARABIC", data_model::LANGUAGE_ARABIC},
    });
```

```cpp
// Version check in ParseLanguage() after parsing succeeds
if (language != data_model::LANGUAGE_ENGLISH &&
    language != data_model::LANGUAGE_UNSPECIFIED) {
  if (kModuleVersion < kRelease14) {
    return absl::InvalidArgumentError(
        "Non-English text indexes require valkey-search version 1.4 or later");
  }
}
```

**Why here:** This is the earliest point where users get a clear, actionable error. It prevents the index from ever being created on an incompatible version.

### 2. Cluster-level gate (already exists)

The existing `MetadataManager` machinery handles the cluster case:

- `IndexSchema::GetMinVersion()` stamps non-English text indexes with `kRelease14`
- `MetadataManager::ComputeMinVersion()` aggregates across all indexes to get the cluster-wide minimum
- Nodes running < 1.4 reject metadata messages where `top_level_min_version > kModuleVersion`

**No additional work needed.**

### 3. RDB load gate (already exists)

The `minimum_semantic_version` callback in `MetadataManager::Init()` calls `ComputeMinVersion()`, which feeds into the RDB section's min version. When a < 1.4 node loads an RDB containing a non-English index, it rejects the section.

**No additional work needed.**

### 4. Query/Read gate (not needed)

If the index can't be created on < 1.4 (gate #1) and can't be replicated to < 1.4 (gates #2 and #3), then queries against non-English indexes on < 1.4 nodes are impossible — the index simply won't exist there.

**No separate query gate needed.**

### 5. Delete gate (not needed)

`FT.DROPINDEX` operates on existing indexes by name. If the index doesn't exist on a < 1.4 node, there's nothing to delete. If a non-English index somehow exists (e.g., from a future downgrade scenario), allowing deletion is the safe behavior.

**No delete gate needed.**

## Summary of Changes

| Component | Change | Status |
|-----------|--------|--------|
| `src/index_schema.proto` | Add all language enum values | ✅ Done |
| `src/version.h` | Add `kRelease14` | ✅ Done |
| `src/index_schema.cc` | Return `kRelease14` for non-English in `GetMinVersion()` | ✅ Done |
| `src/commands/ft_create_parser.cc` | Expand `kLanguageByStr` with all languages | ✅ Done |
| `src/commands/ft_create_parser.cc` | Add version check in `ParseLanguage()` | ✅ Done |
| `testing/index_schema_test.cc` | `GetMinVersion` parameterized unit tests | ✅ Done |
| `testing/ft_create_test.cc` | Language version gate unit tests | ✅ Done |
| `src/coordinator/metadata_manager.cc` | None (already gates cluster replication) | N/A |

## Test Cases

### Unit Tests (ft_create_parser / ft_create_test)

#### Test: Non-English language rejected on version < 1.4

Since `kModuleVersion` is currently 1.2.0, all non-English languages should be rejected at create time.

```cpp
// For each non-English language, FT.CREATE should return an error
// FT.CREATE idx LANGUAGE FRENCH ON HASH PREFIX 1 doc: SCHEMA title TEXT
// Expected: "(error) Non-English text indexes require valkey-search version 1.4 or later"
```

Test cases to cover:

| Language | Expected Result |
|----------|----------------|
| ENGLISH | Success (allowed on all versions) |
| FRENCH | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| GERMAN | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| SPANISH | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| ITALIAN | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| PORTUGUESE | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| RUSSIAN | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| SWEDISH | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| TURKISH | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| DUTCH | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| INDONESIAN | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| ARABIC | Error: "Non-English text indexes require valkey-search version 1.4 or later" |
| UNKNOWN_LANG | Error: "Bad arguments for LANGUAGE: Unknown argument `UNKNOWN_LANG`" |

#### Test: English language always allowed

```cpp
// FT.CREATE idx LANGUAGE ENGLISH ON HASH PREFIX 1 doc: SCHEMA title TEXT
// Expected: Success regardless of version
```

#### Test: Unspecified language (no LANGUAGE param) always allowed

```cpp
// FT.CREATE idx ON HASH PREFIX 1 doc: SCHEMA title TEXT
// Expected: Success (defaults to ENGLISH)
```

### Existing Tests (index_schema_test.cc — GetMinVersion)

These already exist and validate the metadata versioning layer:

| Test Name | Language | Attribute | Expected Version |
|-----------|----------|-----------|-----------------|
| EnglishTextIndex | ENGLISH | Text | kRelease12 |
| UnspecifiedLanguageTextIndex | UNSPECIFIED | Text | kRelease12 |
| NonEnglishTextIndex | FRENCH | Text | kRelease14 |
| VectorOnlyIndex | ENGLISH | Vector | kRelease10 |
| NonZeroDbNum | UNSPECIFIED | Vector | kRelease11 |
| EnglishText_DbNumNonZero | ENGLISH | Text | kRelease12 |
| NonEnglishText_DbNumNonZero | FRENCH | Text | kRelease14 |

### Integration Tests

#### Test: End-to-end rejection of non-English index creation

```
127.0.0.1:6379> FT.CREATE articles LANGUAGE FRENCH ON HASH PREFIX 1 article: SCHEMA title TEXT body TEXT
(error) Non-English text indexes require valkey-search version 1.4 or later
```

#### Test: English index creation still works

```
127.0.0.1:6379> FT.CREATE articles LANGUAGE ENGLISH ON HASH PREFIX 1 article: SCHEMA title TEXT body TEXT
OK
```

#### Test: Case-insensitive language parsing

```
127.0.0.1:6379> FT.CREATE articles LANGUAGE french ON HASH PREFIX 1 article: SCHEMA title TEXT body TEXT
(error) Non-English text indexes require valkey-search version 1.4 or later
```

## Future Work (Version 1.4)

When the module version is bumped to 1.4.0:

1. The version check in `ParseLanguage()` will pass for non-English languages
2. The `GetMinVersion()` logic will stamp those indexes as requiring 1.4+
3. Cluster nodes running < 1.4 will automatically reject metadata containing non-English indexes
4. RDB files written by 1.4 nodes with non-English indexes will be rejected by < 1.4 nodes on load

No code changes beyond the version bump are needed to "unlock" multi-language support.
