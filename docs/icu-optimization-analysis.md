# ICU Dependency Optimization Analysis

## Current State

valkey-search statically links the full ICU library (v76), resulting in significant binary bloat:

| Library | Size | Purpose |
|---------|------|---------|
| `libicudata.a` | 31 MB | Locale data, normalization tables, collation rules |
| `libicui18n.a` | 11 MB | Internationalization: collation, formatting, transliteration |
| `libicuuc.a` | 5.1 MB | Core Unicode: normalization, case mapping, properties |
| `source/data/` (repo) | 45 MB | Full ICU data source (4,136 data items) |
| **Total ICU footprint** | **~47 MB in binary, 45 MB in repo** | |

The final `libsearch.so` is 170 MB.

## Actual ICU Usage

All ICU usage is confined to a single file: `src/indexes/text/unicode_normalizer.cc`

### Headers (all from `common/unicode/`)

```cpp
#include "unicode/bytestream.h"
#include "unicode/casemap.h"
#include "unicode/normalizer2.h"
#include "unicode/stringpiece.h"
#include "unicode/utypes.h"
```

### APIs Used

| API | Library | Purpose |
|-----|---------|---------|
| `icu::Normalizer2::getNFCInstance()` | `libicuuc.a` | NFC normalization (most languages) |
| `icu::Normalizer2::getNFDInstance()` | `libicuuc.a` | NFD normalization |
| `icu::Normalizer2::getNFKCInstance()` | `libicuuc.a` | NFKC normalization (Arabic) |
| `icu::Normalizer2::getNFKDInstance()` | `libicuuc.a` | NFKD normalization |
| `icu::CaseMap::utf8Fold()` | `libicuuc.a` | Unicode case folding |

### Data Files Required

Only normalization and case folding data:

| File | Size | Purpose |
|------|------|---------|
| `nfc.nrm` | 36 KB | NFC normalization |
| `nfkc.nrm` | 55 KB | NFKC normalization |
| `nfkc_cf.nrm` | 53 KB | NFKC casefold |
| `nfkc_scf.nrm` | 52 KB | NFKC simple casefold |
| `ucase.icu` | 31 KB | Case mapping data |
| `uprops.icu` | 144 KB | Unicode properties |
| **Total needed** | **~371 KB** | |

### What Is NOT Used

- `libicui18n.a` — no collation, number/date formatting, transliteration, regex
- 3,903 locale `.res` files (number formats, date formats, etc.)
- 39 break iterator `.brk` rule files
- Collation data (`coll/ucadata-*.icu`)
- `unames.icu` (330 KB, Unicode character names)
- All of `source/i18n/` (headers and source)

## Optimization Recommendations

### 1. Drop `libicui18n.a` — saves 11 MB

No symbols from `libicui18n.a` are referenced. Remove from `third_party/icu/CMakeLists.txt`:

```cmake
target_link_libraries(icu INTERFACE 
  ${ICU_BUILD_DIR}/lib/libicudata.a
  # REMOVED: ${ICU_BUILD_DIR}/lib/libicui18n.a
  ${ICU_BUILD_DIR}/lib/libicuuc.a
)

target_include_directories(icu INTERFACE 
  ${ICU_SOURCE_DIR}/common 
  # REMOVED: ${ICU_SOURCE_DIR}/i18n
)
```

### 2. Apply ICU Data Filtering — saves ~30 MB from data blob

ICU provides a built-in data filtering mechanism documented at:
**[ICU Data Build Tool](https://unicode-org.github.io/icu/userguide/icu_data/buildtool)**

#### How it works

Content rephrased for compliance with licensing restrictions:

The ICU Data Build Tool (available since ICU 64) lets you write a configuration file specifying which features and locales to bundle into the data package. The tool supports JSON or Hjson format. To use it with ICU4C, you set the `ICU_DATA_FILTER_FILE` environment variable when running configure:

```
ICU_DATA_FILTER_FILE=filters.json path/to/icu4c/source/runConfigureICU Linux
```

The tool supports two strategies: **subtractive mode** (default — start with everything, remove what you don't need) and **additive mode** (start empty, explicitly add only what you need).

#### Feature categories relevant to valkey-search

From the [ICU documentation's feature table](https://unicode-org.github.io/icu/userguide/icu_data/buildtool):

| Feature | Category | Files | Approx Size |
|---------|----------|-------|-------------|
| Normalization | `"normalization"` | `in/*.nrm` except `in/nfc.nrm` | 160 KiB |
| Break Iteration | `"brkitr_rules"` `"brkitr_dictionaries"` `"brkitr_tree"` | `brkitr/rules/*.txt` `brkitr/dictionaries/*.txt` `brkitr/*.txt` | 522 KiB + 2.8 MiB + 14 KiB |
| Collation | `"coll_ucadata"` `"coll_tree"` | `in/coll/ucadata-*.icu` `coll/*.txt` | 511 KiB + 2.8 MiB |
| Charset Conversion | `"conversion_mappings"` | `mappings/*.ucm` | 4.9 MiB |
| Transliteration | `"translit"` | `translit/*.txt` | 685 KiB |
| Currencies | `"curr_supplemental"` `"curr_tree"` | `curr/*.txt` | 2.5 MiB |
| Language Display Names | `"lang_tree"` | `lang/*.txt` | 2.1 MiB |
| Region Display Names | `"region_tree"` | `region/*.txt` | 1.1 MiB |
| Time Zones | `"zone_tree"` | `zone/*.txt` | 2.7 MiB |
| Units | `"unit_tree"` | `unit/*.txt` | 1.7 MiB |

We only need the **Normalization** category (plus `ucase.icu` and `uprops.icu` from `"misc"`).

#### Exclusion filter syntax

From the documentation, to exclude an entire category:

```
featureFilters: {
  confusables: exclude
}
```

For fine-grained file selection within a category, use an includelist:

```
featureFilters: {
  brkitr_dictionaries: {
    includelist: [
      burmesedict
    ]
  }
}
```

#### Recommended filter for valkey-search

Create `third_party/icu/icu-data-filter.json`:

```json
{
  "localeFilter": {
    "filterType": "language",
    "includelist": ["root"]
  },
  "featureFilters": {
    "normalization": {
      "filterType": "include",
      "includelist": ["nfc", "nfkc", "nfkc_cf", "nfkc_scf"]
    },
    "misc": {
      "filterType": "include",
      "includelist": ["ucase", "uprops"]
    },
    "brkitr_rules": "exclude",
    "brkitr_dictionaries": "exclude",
    "brkitr_tree": "exclude",
    "coll_ucadata": "exclude",
    "conversion_mappings": "exclude",
    "stringprep": "exclude",
    "translit": "exclude",
    "unames": "exclude",
    "curr_supplemental": "exclude",
    "curr_tree": "exclude",
    "lang_tree": "exclude",
    "region_tree": "exclude",
    "zone_tree": "exclude",
    "unit_tree": "exclude"
  }
}
```

Then pass it during configure in `build.sh`:

```bash
ICU_DATA_FILTER_FILE="${ROOT_DIR}/third_party/icu/icu-data-filter.json" \
"${ICU_SOURCE_DIR}/configure" \
    --enable-static \
    --disable-shared \
    ...
```

This reduces `libicudata.a` from 31 MB to approximately 300-400 KB.

#### Debugging the filter

The ICU documentation recommends inspecting `data/out/tmp/icudata.lst` after building to verify which files were included. You can also diff `rules.mk` before and after applying the filter to see exactly what was removed. The `PYTHONPATH` trick allows re-running the filter without a full rebuild:

```bash
PYTHONPATH=python python3 -m icutools.databuilder \
  --mode=gnumake --src_dir=data > data/rules.mk
```

### 3. Summary of Savings

| Component | Before | After | Savings |
|-----------|--------|-------|---------|
| `libicudata.a` | 31 MB | ~400 KB | ~30.6 MB |
| `libicui18n.a` | 11 MB | 0 (removed) | 11 MB |
| `libicuuc.a` | 5.1 MB | 5.1 MB | 0 |
| **Total ICU in binary** | **~47 MB** | **~5.5 MB** | **~41.5 MB** |

## Future Considerations

The header `src/indexes/text/unicode_normalizer.h` declares (but does not yet implement):

- `FindWordBoundaries()` — planned to use ICU `BreakIterator` for CJK segmentation
- `LocaleAwareCaseFold()` — planned for locale-specific case rules (e.g., Turkish İ/I)

A `TODO` comment in `language_processor.cc` references a future `ICUSegmenter` class.

When these are implemented, the data filter will need to add:

```json
"brkitr_rules": {
  "filterType": "include",
  "includelist": ["line", "word"]
},
"brkitr_dictionaries": {
  "filterType": "include", 
  "includelist": ["cjdict"]
}
```

This would add the CJK dictionary (~2 MB) but remain far smaller than the current 31 MB full data package.

## Validation After PR #15 (Processor Refactor)

PR #15 (`linbran5123:processor-refactor`) replaces the `Lexer` class with `LanguageProcessor`/`SnowballProcessor`. The ICU usage remains identical:

- `SnowballProcessor::NormalizeLowerCaseInPlace()` calls `UnicodeNormalizer::Normalize()` + `CaseFoldInPlace()`
- Arabic uses NFKC; all other Snowball languages use NFC
- No new ICU headers or APIs introduced
- `libicui18n.a` remains unused

**The optimization recommendations remain fully applicable after PR #15.**

## Build System Files

Relevant files for implementing these changes:

- `third_party/icu/CMakeLists.txt` — linkage and include paths
- `third_party/CMakeLists.txt` — `add_subdirectory(icu)`
- `src/indexes/CMakeLists.txt:138` — `target_link_libraries(text PUBLIC icu)`
- `build.sh:173-256` — `build_icu_if_needed()` function
- `src/indexes/text/unicode_normalizer.cc` — sole ICU consumer
