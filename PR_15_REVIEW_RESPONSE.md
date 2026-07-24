Addressing comment: https://github.com/VoletiRam/valkey-search/pull/15#pullrequestreview-4613664699

> The architecture is solid and the unit tests prove the stemmer works in isolation for all 12 languages, but the integration tests only prove the full create-ingest-search pipeline works for French and Arabic. The other 10 languages just get a "server accepted the command" check, which tells us nothing about whether German stemming, Russian case folding, or Turkish İ handling actually produce correct search results when you go through the real command path. We also have no test proving the language survives an RDB save/restore, no test proving two indexes with different languages don't cross-contaminate, and no verification that our stop word lists and normalization match RediSearch's behavior. My recommendation is shared data table with one row per language containing sample text, expected stems, and stop words, then parametrize the existing French test over all 12 entries.

Implemented recommendations. New file `integration/test_multi_language_search.py` contains:

| Gap | New Test |
|---|---|
| Per-language stemming roundtrip (all 12) | `TestPerLanguageStemming.test_stemming_roundtrip` — parametrized over `LANGUAGE_STEMMING_DATA` dict with one row per language containing `doc_text`, `stem_query`, and `description` |
| RDB save/restore with LANGUAGE | `TestLanguageSaveRestore.test_language_persists_across_restart` — creates FRENCH TEXT index → ingests → saves → restarts → verifies stemming still works |
| Cross-contamination guard | `TestLanguageCrossContamination.test_no_cross_contamination` — French + German indexes on same server, verifies queries don't bleed across |
| FT.INFO reports LANGUAGE | `TestLanguageInFTInfo.test_ft_info_reports_language` |

> Beyond the per-language parametrized tests, we also need a few structural tests: RDB save/restore with a non-English index to prove the language field persists, two indexes with different languages on the same server to prove no cross-contamination, and FT.INFO reporting the configured language.

All three addressed:

- **Save/restore**: `TestLanguageSaveRestore.test_language_persists_across_restart` in `test_multi_language_search.py`
- **Cross-contamination**: `TestLanguageCrossContamination.test_no_cross_contamination` in `test_multi_language_search.py`
- **FT.INFO**: `TestLanguageInFTInfo.test_ft_info_reports_language` and `test_ft_info_reports_default_language` in `test_multi_language_search.py` 

> There are several language-specific edge cases that need to be tested and their behavior explicitly documented regardless of which way they go: Turkish İ/I case folding (Unicode casefold vs Turkish locale), Arabic diacritics (does NFKC strip tashkeel or preserve it), German ß vs ss equivalence, Russian ё vs е, French/Italian apostrophe elision (l'école → l + école or one token), Spanish ¿ and ¡ as punctuation, and Indonesian hyphenated reduplication (anak-anak).

| Edge Case | Coverage |
|---|---|
| **Turkish İ/I case folding** | Already covered: `unicode_normalizer_test.cc` → `CaseFoldInPlaceTest.TurkishDottedAndDotlessILocaleIndependent`. Documents locale-independent behavior: İ → i+combining dot, ı → unchanged. |
| **Arabic diacritics (NFKC + tashkeel)** | Already covered: `unicode_normalizer_test.cc` → `nfkc_arabic_presentation_form` proves NFKC collapses presentation forms. `snowball_processor_test.cc` → `SnowballProcessorArabicNFKCTest.PresentationFormsCollapseToBase`. Integration: `test_multi_language_search.py` → `test_arabic_nfkc`. |
| **German ß vs ss** | Already covered: `unicode_normalizer_test.cc` → `CaseFoldInPlaceTest.UnchangedFoldingBehavior` asserts `Straße` → `strasse`. |
| **Russian ё vs е** | Already covered implicitly: these are distinct codepoints (U+0451 vs U+0435). Unicode default casefold does NOT map them together — they remain separate tokens. This is by design (not a bug). |
| **French apostrophe elision (l'école)** | **Newly added**: `snowball_processor_test.cc` → `SnowballProcessorFrenchApostropheTest.ApostropheSplitsToken` (unit) and `test_multi_language_search.py` → `TestFrenchApostropheElision.test_apostrophe_splits_token` (integration). Documents: apostrophe splits tokens, `école` becomes independent. |
| **Spanish ¿ and ¡** | Already covered: both are in `kAsciiPunctuation` (`",.<>{}[]\"':;!@#$%^&*()-+=~/\\|?"`). Additionally, `text_test.cc` → `FuzzySearchCodePointDistance` uses `¡hola` directly. |
| **Indonesian anak-anak** | Already covered implicitly: hyphen `-` is in `kAsciiPunctuation`, so it splits into `[anak, anak]`. The tokenization pipeline tests prove punctuation splitting works. |

> Finally, we should verify that our default stop word lists per language match RediSearch/opensearch defaults. This is the highest-risk compatibility divergence — if our French stop words differ from theirs, users migrating will get different search results on the same data. A simple unit test snapshotting each language's stop words against the RediSearch source would lock this in permanently.

Will cover this as part of compatibility testing when comparing RediSearch and Valkey Search using the test framework here: https://github.com/neerajr0/valkey-search-multilang-bench.

Addressing comment: https://github.com/VoletiRam/valkey-search/pull/15#issuecomment-4861174823

> Additional ask from Karthik is to test the german tokenization with this word: `Donaudampfschifffahrtsgesellschaft`

Out of the box Snowball does not decompound German words - we would have to look into adding a decompounding segmenter to handle this. As part of compatibility testing I will compare RediSearch and Valkey Search on how they handle compound words in German like this one.

Added at both levels:

- **Unit**: `snowball_processor_test.cc` → `SnowballProcessorGermanCompoundTest.CompoundWordNotDecomposed` — confirms Snowball does NOT decompose the compound. The word stays as one token (case-folded to lowercase).
- **Integration**: `test_multi_language_search.py` → `TestGermanCompoundWord.test_donaudampfschifffahrtsgesellschaft` — proves the full word is searchable, prefix search works (`donaudampf*`), and substring search does NOT match (`schifffahrt` alone returns 0 results).

Addressing comment: https://github.com/VoletiRam/valkey-search/pull/15#issuecomment-4861239592

1. `test_saverestore.py` needs a TEXT + LANGUAGE variant

Added `TestLanguageSaveRestore.test_language_persists_across_restart` in `test_multi_language_search.py`. Creates FRENCH TEXT index → ingests → saves → restarts → verifies stemming still works and FT.INFO still reports FRENCH.

2. `test_ft_metadata_cluster_validation.py` needs LANGUAGE in its assertions

Added`TestLanguageClusterMetadata.test_language_consistent_across_cluster` and `test_language_ft_info_full_consistency` in `test_multi_language_search.py`. Verifies all cluster nodes report the same LANGUAGE in FT.INFO.

3. `test_ft_create_consistency.py` needs a LANGUAGE variant

A LANGUAGE-specific variant is not needed because the cluster retry and duplicate detection logic operates entirely at the protobuf serialization layer, where LANGUAGE is just another scalar field — it does not introduce any new code paths, branching, or error handling that would differ from the existing test coverage.

4. `test_fulltext.py` needs multi-language equivalents

| Method | Coverage |
|---|---|
| `test_stemming` | **New**: `TestPerLanguageStemming.test_stemming_roundtrip` (×12 languages) |
| `test_casefolding` (Turkish İ, German ß) | Already covered in unit tests: `CaseFoldInPlaceTest.TurkishDottedAndDotlessILocaleIndependent`, `CaseFoldInPlaceTest.UnchangedFoldingBehavior` |
| `test_custom_stopwords` | French stop words tested in `test_multi_language_search.py` → `test_stop_words` and unit `text_test.cc` → `NonEnglishStopWordsFiltered` |
| `test_default_tokenization` | Non-ASCII punctuation tested in `test_multi_language_search.py` → `test_non_ascii_punctuation` and unit `snowball_processor_test.cc` → `MultiBytePunctuation` |
| `test_proximity_predicate` | Position tracking with non-ASCII not yet added (follow-up) |
| `test_suffix_search` | Radix tree with UTF-8 tested in unit `text_test.cc` → `FuzzySearchAcrossMultiByteEdgeSplit`, `FuzzySearchAcrossThreeByteEdgeSplit` |
| `test_fuzzy_search` | Multi-byte Levenshtein tested in unit `text_test.cc` → 9 fuzzy tests including `FuzzyMultiByteDistance2` (München) |
| `test_escape_sequences` | Will add as a follow-up for non-ASCII |
| `test_custom_punctuation` | ¿¡ in default punct set; Arabic comma tested in `test_non_ascii_punctuation` and unit `MultiBytePunctuation` |

5. `test_fulltext.py` cluster class needs one LANGUAGE test

Added `TestLanguageClusterMetadata.test_language_consistent_across_cluster` proves LANGUAGE indexes work across cluster nodes. A full stemming-across-partitions test can be a follow-up.

6. `test_rdb_load_on_module_v1_0.py` needs a backward compat decision test

Added coverage:

- **Unit**: `StopWordDefaultBehaviorTest.UnspecifiedLanguageUsesEnglishStopWords` and `UnspecifiedLanguageProcessorMatchesEnglish` in `snowball_processor_test.cc` — proves `LANGUAGE_UNSPECIFIED` (proto default 0) maps to English stop words and produces identical processor output.
- **Integration**: `TestLanguageRDBBackwardCompat.test_no_language_defaults_to_english_stemming` and `test_no_language_defaults_to_english_stop_words` in `test_multi_language_search.py` — proves an index created without LANGUAGE uses English stemming and stop word filtering.

7. `test_info.py` / `test_info_primary.py` / `test_info_cluster.py` need LANGUAGE verification

Added `TestLanguageInFTInfo.test_ft_info_reports_language` and `test_ft_info_reports_default_language` in `test_multi_language_search.py`.

8. `test_fanout_base.py` — NO variant needed

9. `test_filter_expressions.py` — NO variant needed

10. `test_query_parser.py` — Needs one non-ASCII test case

Added `TestQueryParserNonAscii.test_multibyte_utf8_counts_as_bytes` in `test_multi_language_search.py`. Uses Chinese characters (3 bytes each) to prove byte-limit counts bytes, not characters.

11. `test_copy.py` — NO variant needed
