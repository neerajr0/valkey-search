# Default Punctuation Per Language — Implementation Plan

## Overview

Add per-language default punctuation to valkey-search, following the same pattern used for stop words. When a user creates an index with `LANGUAGE <lang>` and does not provide an explicit `PUNCTUATION` override, the index uses the language-appropriate default punctuation set.

## Background

The current implementation uses a single ASCII punctuation set for all languages:
```
,.<>{}[]"':;!@#$%^&*()-+=~/\\|?
```

This is insufficient for non-English languages that use script-specific punctuation as word boundaries:
- **Spanish**: `¡` (U+00A1) inverted exclamation, `¿` (U+00BF) inverted question mark
- **French/German/Italian/Spanish/Russian**: `«` (U+00AB) `»` (U+00BB) guillemets
- **German/Russian**: `„` (U+201E) `‚` (U+201A) low quotation marks
- **Arabic**: `،` (U+060C) Arabic comma, `؛` (U+061B) Arabic semicolon, `؟` (U+061F) Arabic question mark
- **Common typographic**: `–` (U+2013) en-dash, `—` (U+2014) em-dash, `…` (U+2026) ellipsis, `'` `'` `"` `"` smart quotes

Source: [Unicode CLDR Punctuation Exemplars (v46)](https://www.unicode.org/cldr/charts/46/by_type/core_data.alphabetic_information.punctuation.html)

## Scope

### In scope
- Define per-language default punctuation constants
- Create `GetDefaultPunctuation(Language)` mapping function
- Wire defaults into `ft_create_parser.cc` at index creation time
- Add `use_default_punctuation` flag to `PerIndexTextParams`

### Out of scope (handled by UTF-8 Iterator PR)
- Multi-byte punctuation matching in the lexer (requires `PunctuationSet` keyed by code points instead of `bitset<256>`)
- Multi-byte punctuation matching in the filter parser's token boundary detection
- These are addressed by [VoletiRam/valkey-search#6](https://github.com/VoletiRam/valkey-search/pull/6) which changes `IsPunctuation` from byte-level to code-point-level

### Design note: Quotation marks vs query syntax
The ASCII `"` (U+0022) is the **only** character that acts as a query syntax operator (phrase delimiter) in the filter parser. Guillemets `«»` and smart quotes `""''` are treated purely as **punctuation** (word boundaries), not as phrase delimiters. No changes to the parser's phrase-handling logic are needed. Once the multi-byte PR lands, these characters will work as word boundaries in both the lexer and the parser's token boundary detection automatically.

## Language-Specific Punctuation Analysis

Based on Unicode CLDR data, languages fall into tiers:

| Tier | Languages | Extra punctuation beyond ASCII default |
|------|-----------|---------------------------------------|
| 1 — ASCII only | English, Portuguese, Swedish, Turkish, Dutch, Indonesian | None |
| 2 — Inverted marks | Spanish | `¡` `¿` |
| 3 — Guillemets | French, German, Italian, Russian | `«` `»` |
| 4 — Low quotes | German, Russian | `„` `‚` |
| 5 — Arabic script | Arabic | `،` `؛` `؟` |

Additionally, all Latin-script languages benefit from common typographic punctuation that appears frequently in real-world text: `–` `—` `…` `'` `'` `"` `"`.

## Implementation Design

### Approach: Layered constants

Define a common multi-byte punctuation extension shared across most European languages, then per-language additions for truly script-specific characters.

```cpp
// Base ASCII punctuation (existing)
kDefaultPunctuation = ",.<>{}[]\"':;!@#$%^&*()-+=~/\\|?"

// Common typographic punctuation (multi-byte, shared across Latin-script languages)
kCommonMultiBytePunctuation = "–—…''""«»"

// Language-specific extensions
kSpanishPunctuation    = kDefaultPunctuation + kCommonMultiBytePunctuation + "¡¿"
kFrenchPunctuation     = kDefaultPunctuation + kCommonMultiBytePunctuation
kGermanPunctuation     = kDefaultPunctuation + kCommonMultiBytePunctuation + "„‚"
kItalianPunctuation    = kDefaultPunctuation + kCommonMultiBytePunctuation
kPortuguesePunctuation = kDefaultPunctuation + kCommonMultiBytePunctuation
kRussianPunctuation    = kDefaultPunctuation + kCommonMultiBytePunctuation + "„‚"
kSwedishPunctuation    = kDefaultPunctuation + kCommonMultiBytePunctuation
kTurkishPunctuation    = kDefaultPunctuation + kCommonMultiBytePunctuation
kDutchPunctuation      = kDefaultPunctuation + kCommonMultiBytePunctuation
kIndonesianPunctuation = kDefaultPunctuation + kCommonMultiBytePunctuation
kArabicPunctuation     = kDefaultPunctuation + "،؛؟"  // Arabic-specific, no guillemets needed
```

Note: English retains only the ASCII default for backward compatibility.

### File structure

New file: `src/indexes/text/punctuation.h`
- Same pattern as `stop_words.h`
- Per-language `std::string` constants
- `GetDefaultPunctuation(data_model::Language)` function

## Implementation Steps

### Step 1: Create `src/indexes/text/punctuation.h`

Define inline string constants for each language's default punctuation and a `GetDefaultPunctuation()` switch function. Follow the same structure as `stop_words.h`:
- Copyright + license header
- Attribution comment (Unicode CLDR source)
- `namespace valkey_search::indexes::text`
- Inline constants per language
- `GetDefaultPunctuation(data_model::Language)` function

### Step 2: Add `use_default_punctuation` flag to `PerIndexTextParams`

In `src/commands/ft_create_parser.h`:
```cpp
struct PerIndexTextParams {
  std::string punctuation{kDefaultPunctuation};
  bool use_default_punctuation{true};  // NEW: tracks whether user explicitly set PUNCTUATION
  // ... rest unchanged
};
```

### Step 3: Update PUNCTUATION parser to set the flag

In `ft_create_parser.cc`, the existing `CreateSchemaTextParser()` already parses `PUNCTUATION` into `schema_text_defaults.punctuation`. Add logic so that when `PUNCTUATION` is explicitly provided, `use_default_punctuation` is set to `false`:

```cpp
parser.AddParamParser(kPunctuationParam,
    std::make_unique<vmsdk::ParamParser<PerIndexTextParams>>(
        [](PerIndexTextParams &params, vmsdk::ArgsIterator &itr) -> absl::Status {
          std::string value;
          VMSDK_RETURN_IF_ERROR(vmsdk::ParseParamValue(itr, value));
          params.punctuation = std::move(value);
          params.use_default_punctuation = false;  // User explicitly set it
          return absl::OkStatus();
        }));
```

### Step 4: Wire per-language defaults in `ParseFTCreateArgs`

After language is resolved and before the schema is finalized (same location where stop words defaults are applied):

```cpp
// If no explicit punctuation override was provided, apply per-language defaults
if (schema_text_defaults.use_default_punctuation) {
  schema_text_defaults.punctuation =
      indexes::text::GetDefaultPunctuation(schema_text_defaults.language);
}
```

### Step 5: Include header

Add `#include "src/indexes/text/punctuation.h"` to `ft_create_parser.cc`.

### Step 6: Update `THIRD_PARTY_NOTICES`

Add the following entry to the end of `THIRD_PARTY_NOTICES`:

```
Package: Unicode CLDR (Punctuation Exemplars)
License: Unicode License V3
License URL: https://www.unicode.org/license.txt
Source URL: https://www.unicode.org/cldr/charts/46/by_type/core_data.alphabetic_information.punctuation.html
Details: Per-language default punctuation character sets for full-text search tokenization.
  Languages: French, German, Spanish, Italian, Portuguese, Russian, Swedish,
  Turkish, Dutch, Indonesian, Arabic.
```

### Step 7: Update CMakeLists if needed

`punctuation.h` is header-only (inline constants), so no new source file needs to be added to the build. Verify it compiles as part of the existing targets.

## Testing

### Unit tests (in `testing/ft_create_test.cc`)

1. **Default punctuation — guillemets tier (French)**: Create an index with `LANGUAGE FRENCH` (no explicit `PUNCTUATION`) and verify the stored punctuation includes `«»` and the common multi-byte set (`–—…''""«»`).
2. **Explicit PUNCTUATION overrides default**: Create an index with `LANGUAGE ARABIC PUNCTUATION ",.!"` and verify only the explicit set `",.!"` is stored (per-language default is not applied).
3. **Default punctuation — inverted marks tier (Spanish)**: Create an index with `LANGUAGE SPANISH` and verify the stored punctuation includes `¡` and `¿` in addition to the common multi-byte set.
4. **Default punctuation — low quotes tier (German)**: Create an index with `LANGUAGE GERMAN` and verify the stored punctuation includes `„` and `‚` in addition to guillemets and common multi-byte set.
5. **Default punctuation — Arabic script tier**: Create an index with `LANGUAGE ARABIC` and verify the stored punctuation includes `،` `؛` `؟` (and does NOT include guillemets — Arabic has its own set).
6. **Default punctuation — ASCII-only tier (Indonesian)**: Create an index with `LANGUAGE INDONESIAN` and verify the stored punctuation includes the common multi-byte set but no language-specific additions beyond that.
7. **Custom PUNCTUATION specified twice**: Create an index with `LANGUAGE FRENCH PUNCTUATION ",.!" PUNCTUATION "xyz"` and verify the last value wins (`"xyz"` is stored) and the language default is not applied.

### Integration tests (dependent on multi-byte PR)
8. **Arabic tokenization**: Index `hello،world` with `LANGUAGE ARABIC`, verify it tokenizes to `["hello", "world"]`.
9. **Spanish tokenization**: Index `¿Hola?` with `LANGUAGE SPANISH`, verify it tokenizes to `["hola"]`.
10. **Guillemet tokenization**: Index `«bonjour»` with `LANGUAGE FRENCH`, verify it tokenizes to `["bonjour"]`.

Note: Integration tests 8–10 require the multi-byte `PunctuationSet` from the UTF-8 Iterator PR to actually pass. They can be written now and skipped/marked TODO until that PR merges.

## Dependencies

| Dependency | Status | Impact |
|-----------|--------|--------|
| UTF-8 Iterator PR (task 0) | In Review | Multi-byte punctuation won't actually split tokens until this lands. Our constants will be persisted correctly but won't function as word boundaries for non-ASCII chars. |
| Language enum + parser map (task 2) | In Review | Required — we need the language values to exist. Already present on this branch. |
| Proto `punctuation` field | Done | Already exists in `IndexSchema` proto as `string punctuation`. No schema changes needed. |

## Data Source & Attribution

### Source: Unicode CLDR Punctuation Exemplars (v46)

The Unicode Common Locale Data Repository (CLDR) is the authoritative source for locale-specific character data, maintained by the Unicode Consortium. The Punctuation Exemplars chart explicitly lists "punctuation characters used in running text" for each locale. This is the same data ICU uses internally for text segmentation.

URL: https://www.unicode.org/cldr/charts/46/by_type/core_data.alphabetic_information.punctuation.html

### Why this is sufficient

- **Authoritative**: CLDR is the standard used by ICU, Java, Android, iOS, and every major platform for locale data.
- **Conservative**: We include only the most common punctuation per locale. Uncommon or domain-specific characters are handled by the user-configurable `PUNCTUATION` parameter.
- **Strictly better than status quo**: Redis Search uses only ASCII punctuation for all languages. Our per-language defaults are an improvement over that baseline.
- **Not exhaustive by design**: No list can cover every typographic convention in every domain. The `PUNCTUATION` parameter at `FT.CREATE` time covers edge cases.

### Attribution requirements

Unicode CLDR data is licensed under the [Unicode License V3](https://www.unicode.org/license.txt). This is permissive and compatible with BSD-3-Clause but requires attribution. Attribution is provided in:

1. **File header** in `punctuation.h` (comment block citing source URL and license)
2. **`THIRD_PARTY_NOTICES`** file at the repository root (new entry for Unicode CLDR)

This follows the same pattern used for Apache Lucene stop word attribution in `stop_words.h`.

## Effort Estimate

~2 days (as stated in the quip delivery plan, task 11).

## Open Questions

1. **Should English get the common multi-byte punctuation too?** Current plan says no (backward compat), but real English text contains smart quotes and em-dashes. Decision: keep English as ASCII-only for now; users can use `PUNCTUATION` to override.
2. **Should we log a warning for CJK languages with PUNCTUATION set?** The quip mentions this. Not in scope for this task since CJK languages aren't in the current enum, but the `GetDefaultPunctuation` function should return empty string for CJK when added later (ICU handles punctuation natively).
