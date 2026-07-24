# Multi-Language Feature Flag Implementation Plan

## Goal

Tie multi-language support to **both** module version >= 1.4 AND a `multi-language-support` feature flag. This ensures:
- No version < 1.4 can use multi-language even if the flag is set
- Version >= 1.4 still requires the flag to be explicitly enabled
- The feature is fully opt-in and version-gated

## Changes

### 1. Make `multi-language-support` a dev config (startup-only)

In `src/valkey_search_options.cc`, change the config builder from:
```cpp
static auto multi_language_support =
    config::BooleanBuilder(kMultiLanguageSupportConfig, false).Build();
```

To:
```cpp
static auto multi_language_support =
    config::BooleanBuilder(kMultiLanguageSupportConfig, false).Dev().Build();
```

The `.Dev()` modifier makes the config:
- Only settable at module load time in production (when `debug-mode` is `no`)
- Modifiable at runtime only when `debug-mode` is enabled (useful for testing)

This means users must configure it at startup via the `loadmodule` directive in a config file:
```
loadmodule /path/to/libsearch.so --debug-mode yes --multi-language-support yes
```

And **cannot** change it at runtime via `CONFIG SET search.multi-language-support yes` when `debug-mode` is disabled.

**Note:** `ParseAndLoadArgv` resets `debug-mode` to `false` at the start of arg parsing. Since `multi-language-support` is a Dev config, it requires `debug-mode=yes` to be accepted. Therefore `--debug-mode yes` must appear before `--multi-language-support yes` in the module args.

### 2. `src/multi_language.h` — helper functions

Contains three inline helpers:
- `IsNonEnglishLanguage(language)` — true if language is not English/unspecified
- `IsMultiLanguageSupported()` — true if `kModuleVersion >= kRelease14` AND feature flag is enabled
- `IsLanguageSupported(language)` — combines both checks into a single predicate

### 3. `src/commands/ft_create_parser.cc` — guard clause

Uses `IsLanguageSupported()` from `multi_language.h`:
```cpp
if (!IsLanguageSupported(language)) {
  return absl::InvalidArgumentError(
      "Non-English text indexes require module version >= 1.4 and "
      "multi-language-support to be enabled");
}
```

### 4. `src/index_schema.cc` — `GetMinVersion()` — NO CHANGE NEEDED

The current implementation is correct as-is:
```cpp
if (has_text_index) {
  if (IsNonEnglishLanguage(unpacked->language())) {
    return kRelease14;
  }
  return kRelease12;
}
```

**Rationale:** The parser prevents non-English index creation when `!IsLanguageSupported()`. So `GetMinVersion()` will only ever see a non-English index if multi-language was supported at creation time. The version stamp purely reflects the data's intrinsic requirement — no feature flag check needed here.

### 5. Unit tests

#### `testing/ft_create_test.cc` — `NonEnglishLanguageVersionGate`

| Scenario | kModuleVersion | Feature Flag | Language | Expected |
|----------|---------------|--------------|----------|----------|
| A1 | 1.2 | off | FRENCH | Error: "Non-English text indexes require module version >= 1.4 and multi-language-support to be enabled" |
| A2 | 1.2 | off | ENGLISH | OK |
| A3 | 1.2 | off | (unspecified) | OK |
| A4 | 1.2 | off | KLINGON | Error: "Bad arguments for LANGUAGE: Unknown argument `KLINGON`" |
| B1 | 1.4 | disabled | FRENCH | Error: "Non-English text indexes require module version >= 1.4 and multi-language-support to be enabled" |
| B2 | 1.4 | disabled | ENGLISH | OK |
| B3 | 1.4 | disabled | (unspecified) | OK |
| B4 | 1.4 | disabled | KLINGON | Error: "Bad arguments for LANGUAGE: Unknown argument `KLINGON`" |
| C1 | 1.4 | enabled | FRENCH | OK |
| C2 | 1.4 | enabled | ENGLISH | OK |
| C3 | 1.4 | enabled | (unspecified) | OK |
| C4 | 1.4 | enabled | KLINGON | Error: "Bad arguments for LANGUAGE: Unknown argument `KLINGON`" |

**Note:** Since `kModuleVersion` is a compile-time constant (currently 1.2), scenarios B and C require temporarily changing `kModuleVersion` to 1.4 in `version.h` for manual testing. For automated unit tests:
- With `kModuleVersion = 1.2`: non-English always rejected regardless of flag
- With `kModuleVersion = 1.4`: non-English rejected if flag is off, accepted if flag is on

#### `testing/index_schema_test.cc` — `GetMinVersionTests`

No changes needed. `GetMinVersion()` is unchanged and its tests don't depend on the feature flag.

### 6. Manual testing matrix

Since the config is now a **dev config** (`.Dev()`), runtime `CONFIG SET` is blocked when `debug-mode` is disabled. The config can be set at module load time via command-line args (with `debug-mode yes` specified first).

Run valkey-server with module loaded using config files. Test all combinations:

#### Scenario A: Module version 1.2 (current), flag enabled at startup

Config file:
```
port 6399
loadmodule /path/to/libsearch.so --debug-mode yes --multi-language-support yes
```

```
$ valkey-cli FT.CREATE test_french LANGUAGE FRENCH SCHEMA title TEXT
(error) Non-English text indexes require module version >= 1.4 and multi-language-support to be enabled

$ valkey-cli FT.CREATE test_english LANGUAGE ENGLISH SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_default SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_klingon LANGUAGE KLINGON SCHEMA title TEXT
(error) Bad arguments for LANGUAGE: Unknown argument `KLINGON`
```

#### Verify runtime CONFIG SET is blocked (debug-mode=no)

Config file:
```
port 6399
loadmodule /path/to/libsearch.so --debug-mode no
```

```
$ valkey-cli CONFIG SET search.multi-language-support yes
(error) ERR CONFIG SET failed (possibly related to argument 'search.multi-language-support') - Modification of 'multi-language-support' requires 'debug-mode' to be enabled.
```

#### Scenario B: Module version 1.4, feature flag DISABLED

Temporarily set `kModuleVersion` to 1.4 in `version.h`, rebuild.

Config file:
```
port 6399
loadmodule /path/to/libsearch.so --debug-mode yes
```

```
$ valkey-cli FT.CREATE test_french LANGUAGE FRENCH SCHEMA title TEXT
(error) Non-English text indexes require module version >= 1.4 and multi-language-support to be enabled

$ valkey-cli FT.CREATE test_english LANGUAGE ENGLISH SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_default SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_klingon LANGUAGE KLINGON SCHEMA title TEXT
(error) Bad arguments for LANGUAGE: Unknown argument `KLINGON`
```

#### Scenario C: Module version 1.4, feature flag ENABLED at startup

Config file:
```
port 6399
loadmodule /path/to/libsearch.so --debug-mode yes --multi-language-support yes
```

```
$ valkey-cli FT.CREATE test_french LANGUAGE FRENCH SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_english LANGUAGE ENGLISH SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_default SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_klingon LANGUAGE KLINGON SCHEMA title TEXT
(error) Bad arguments for LANGUAGE: Unknown argument `KLINGON`
```

## Implementation Order

1. [x] Add `kRelease14` in `version.h`
2. [x] Add `multi-language-support` config flag in `valkey_search_options.cc` with `.Dev()`
3. [x] Add `src/multi_language.h` with helper functions
4. [x] Update guard clause in `ParseLanguage()` to use `IsLanguageSupported()`
5. [x] Add `GetMinVersion()` logic for non-English text indexes
6. [x] Add unit tests in `ft_create_test.cc` and `index_schema_test.cc`
7. [x] Build and run unit tests
8. [x] Manual test Scenario A (version 1.2, flag enabled at startup — version blocks)
9. [x] Manual test: verify runtime CONFIG SET is blocked without debug-mode
10. [x] Temporarily bump version to 1.4, rebuild
11. [x] Manual test Scenario B (version 1.4, flag disabled at startup)
12. [x] Manual test Scenario C (version 1.4, flag enabled at startup)
13. [x] Revert version back to 1.2

---

## Updated PR Description

### Motivation

Multi-language FTS needs to be gated behind both module version >= 1.4 AND a startup feature flag (`multi-language-support`). This ensures the feature cannot be used on older versions even if the flag is set, and requires explicit opt-in on 1.4+ nodes. The flag is a dev config, meaning it can only be modified at runtime when `debug-mode` is enabled — in production it is effectively startup-only.

### Summary of Changes

- Added `kRelease14` in `version.h`. Multi-language FTS will be introduced in version 1.4. Version 1.3 will be added later during release time.
- Added `multi-language-support` boolean dev config flag in `valkey_search_options.cc` (default: `false`). Users enable at module load time via config file: `loadmodule /path/to/libsearch.so --debug-mode yes --multi-language-support yes`. Runtime `CONFIG SET` is blocked when `debug-mode` is disabled.
- Added `src/multi_language.h` with helper functions:
  - `IsNonEnglishLanguage(language)` — true if language is not English/unspecified
  - `IsMultiLanguageSupported()` — true if `kModuleVersion >= kRelease14` AND feature flag is enabled
  - `IsLanguageSupported(language)` — combines both checks into a single "can this language be used?" predicate
- Updated guard clause in `ParseLanguage()` to reject non-English text indexes unless both version >= 1.4 and feature flag are satisfied
- Added logic to return version 1.4 in `IndexSchema::GetMinVersion()` if the schema uses a text index and specifies a non-English language

### Automated tests

- Added unit tests in `index_schema_test.cc` to assert that correct versions are returned for different index/language/has_db_num combinations.
- Added unit tests in `ft_create_test.cc` to assert that non-English languages are rejected when `IsLanguageSupported()` returns false (version < 1.4 or feature flag disabled), and allowed when both conditions are met.

### Manual testing

Tested with valkey-server running locally using config files for module args:

**Scenario A: Module version 1.2, flag enabled at startup (version blocks regardless)**

Config: `loadmodule /path/to/libsearch.so --debug-mode yes --multi-language-support yes`

```
$ valkey-cli FT.CREATE test_french LANGUAGE FRENCH SCHEMA title TEXT
(error) Non-English text indexes require module version >= 1.4 and multi-language-support to be enabled

$ valkey-cli FT.CREATE test_english LANGUAGE ENGLISH SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_default SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_klingon LANGUAGE KLINGON SCHEMA title TEXT
(error) Bad arguments for LANGUAGE: Unknown argument `KLINGON`
```

**Verify runtime CONFIG SET is blocked (debug-mode=no):**

Config: `loadmodule /path/to/libsearch.so --debug-mode no`

```
$ valkey-cli CONFIG SET search.multi-language-support yes
(error) ERR CONFIG SET failed (possibly related to argument 'search.multi-language-support') - Modification of 'multi-language-support' requires 'debug-mode' to be enabled.
```

**Scenario B: Module version 1.4, feature flag DISABLED (default)**

Config: `loadmodule /path/to/libsearch.so --debug-mode yes`

```
$ valkey-cli FT.CREATE test_french LANGUAGE FRENCH SCHEMA title TEXT
(error) Non-English text indexes require module version >= 1.4 and multi-language-support to be enabled

$ valkey-cli FT.CREATE test_english LANGUAGE ENGLISH SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_default SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_klingon LANGUAGE KLINGON SCHEMA title TEXT
(error) Bad arguments for LANGUAGE: Unknown argument `KLINGON`
```

**Scenario C: Module version 1.4, feature flag ENABLED at startup**

Config: `loadmodule /path/to/libsearch.so --debug-mode yes --multi-language-support yes`

```
$ valkey-cli FT.CREATE test_french LANGUAGE FRENCH SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_english LANGUAGE ENGLISH SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_default SCHEMA title TEXT
OK

$ valkey-cli FT.CREATE test_klingon LANGUAGE KLINGON SCHEMA title TEXT
(error) Bad arguments for LANGUAGE: Unknown argument `KLINGON`
```
