# Research: OpenSearch & Lucene Multi-Language FTS — Comparison with Valkey-Search Proposal

## Purpose

This document compares how OpenSearch (built on Apache Lucene) and Redis Search handle multi-language full-text search, with specific focus on:
1. The 12 Snowball languages (English, French, German, Spanish, Italian, Portuguese, Russian, Swedish, Turkish, Dutch, Indonesian, Arabic)
2. CJKPV languages (Chinese, Japanese, Korean, Polish, Vietnamese)
3. Architectural differences and implications for the valkey-search multi-language proposal

---

## 1. Architecture Overview

### 1.1 Lucene/OpenSearch/Elasticsearch: Composable Analyzer Chain

Lucene uses a **composable pipeline** model ([OpenSearch Analyzers documentation](https://www.opensearch.org/docs/latest/analyzers/)):

```
Character Filters → Tokenizer → Token Filters
```

Each component is independently configurable per field. Multiple analyzers can co-exist on the same index (via multi-field mappings). This architecture means:

- Different fields in the same document can have different language analyzers
- Users can build custom analyzer chains (e.g., `icu_tokenizer` + `snowball` filter + custom stop words)
- Plugins extend the system without modifying core code

**Key difference from valkey-search**: Valkey-search uses a **single language per index** model (set at `FT.CREATE` time), with a strategy-pattern `ILanguageProcessor` interface. The pipeline is less composable — the processor handles tokenization, normalization, and stemming as a unit. This is simpler but less flexible.

### 1.2 Redis Search: Per-Index Language with Friso (Chinese)

Redis Search follows a model closer to valkey-search's proposed architecture ([Redis Search Technical Overview](https://redis.io/docs/latest/develop/ai/search-and-query/administration/overview/)):
- Language is declared per-index or per-document
- Uses **Snowball** (libstemmer C) for 15+ stemming languages — the same algorithm implementations as valkey-search proposes
- Uses **Friso** (a dictionary-based MMSEG segmenter) for Chinese ([Redis Chinese Support](https://redis.io/docs/latest/develop/interact/search-and-query/advanced-concepts/chinese/), [Redis Forum](https://forum.redis.io/t/cant-query-with-chinese/274)) — not ICU
- No specialized support for Japanese, Korean, Polish, or Vietnamese in the open-source version

### 1.3 Valkey-Search Proposal: Strategy Pattern

Valkey-search proposes a two-branch approach ([Multi-language support plan - low level details](https://quip-amazon.com/03HdAmpInFuD)):
- `SnowballProcessor` for 12 Snowball languages (same algorithms as Redis Search and Lucene/Snowball)
- `ICUSegmenter` for CJKPV (Option A) or specialized libraries (Option B)

---

## 2. Snowball Languages — Approach Comparison

### 2.1 What Lucene/OpenSearch Offers Beyond Snowball

For the 12 languages targeted by valkey-search, Lucene provides **multiple stemmer options per language** ([Trifork: Analysing European Languages with Lucene](https://trifork.nl/blog/analysing-european-languages-with-lucene/), [Elasticsearch Stemmer Token Filter](https://www.elastic.co/docs/reference/text-analysis/analysis-stemmer-tokenfilter)):

| Stemmer Type | Description | Languages Available |
|---|---|---|
| **Snowball** (aggressive) | Algorithmic suffix stripping; same algorithms as libstemmer C | Arabic, Dutch, English, Finnish, French, German, Hungarian, Italian, Norwegian, Portuguese, Romanian, Russian, Spanish, Swedish, Turkish + more |
| **Light** stemmers | Minimal suffix removal + plural handling; fewer rules, fewer false conflations | French, German, Hungarian, Italian, Portuguese, Russian, Spanish, Swedish, Finnish |
| **Minimal** stemmers | Only removes plurals | French, German, Portuguese, Italian, Dutch |
| **Hunspell** (dictionary-based) | Uses grammar rules + dictionary; finds all valid stems; handles complex morphology | Any language with an OOo/Firefox dictionary (hundreds available) |

Content was rephrased for compliance with licensing restrictions.

**Key insight**: Lucene's default language analyzers do NOT always use Snowball ([Elasticsearch Language Analyzers Reference](https://www.elastic.co/guide/en/elasticsearch/reference/current/analysis-lang-analyzer.html)). For example:
- The default `french` analyzer uses `FrenchLightStemFilter` (light stemmer), not Snowball
- The default `german` analyzer uses `GermanLightStemFilter` + `GermanNormalizationFilter`
- The default `italian` analyzer uses `ItalianLightStemFilter`

This is because the Snowball stemmer is known to be **overly aggressive** for some languages — for example, stemming English "international" to "intern" by stripping the "ational" suffix ([Trifork: Analysing European Languages with Lucene](https://trifork.nl/blog/analysing-european-languages-with-lucene/)). Light stemmers produce more unique index terms but significantly fewer false conflations.

**Implications for valkey-search**: 
- Using only Snowball (as proposed) will produce **algorithmically identical** stems to Redis Search, ensuring migration compatibility.
- However, search **quality** may be lower than Elasticsearch/OpenSearch defaults for some languages (French, German, Italian, Portuguese) where those systems use lighter stemmers.
- If customers are migrating from Elasticsearch/OpenSearch rather than Redis Search, their expectations around stemming behavior may differ.
- A follow-on improvement could add light stemmer alternatives (the algorithms are straightforward, often <200 lines of code per language), but this is not required for 1.4.

### 2.2 Arabic-Specific Handling

Both Lucene and valkey-search handle Arabic with special care:

| System | Arabic Approach |
|---|---|
| **Lucene/OpenSearch** | `ArabicStemmer` (root extraction) + `ArabicNormalizationFilter` (removes diacritics, normalizes Hamza/Alef forms, normalizes Teh Marbuta) ([Elasticsearch Language Analyzers: Arabic](https://www.elastic.co/guide/en/elasticsearch/reference/current/analysis-lang-analyzer.html)) |
| **Valkey-search** | NFKC normalization (collapses presentation forms) + Snowball Arabic stemmer + extended punctuation set ([Multi-language support plan - low level details](https://quip-amazon.com/03HdAmpInFuD)) |

Valkey-search's use of NFKC normalization for Arabic presentation forms is a correct design decision that Lucene handles differently (via dedicated normalization filters). The effect should be equivalent.

### 2.3 Stop Words

| System | Approach |
|---|---|
| **Lucene/OpenSearch** | Built-in per-language stop word lists (sourced from Snowball project and academic sources). Applied as a token filter stage — independently configurable. |
| **Redis Search** | Default English stop words; `STOPWORDS` parameter at index creation for custom lists. |
| **Valkey-search** | Per-language default stop word lists (sourced from Apache Lucene, Apache 2.0 license). Same mechanism as Redis Search (`NOSTOPWORDS`, `STOPWORDS N word1...`). |

Valkey-search's approach of copying stop word lists from Lucene means the default behavior will be similar to OpenSearch for the same languages.

### 2.4 Unicode Normalization

| System | Approach |
|---|---|
| **Lucene/OpenSearch** | `icu_normalizer` character filter (configurable: NFC, NFD, NFKC, NFKD). `icu_folding` for accent removal. These are separate pipeline stages. ([OpenSearch ICU Analyzer](https://docs.opensearch.org/latest/analyzers/language-analyzers/icu/)) |
| **Valkey-search** | NFC for all Snowball languages, NFKC for Arabic. Combined with case-fold in a single ICU pass. |

Valkey-search's approach is correct. The single-pass optimization (NFC + case-fold in one ICU call) is a performance advantage over Lucene's multi-stage approach.

---

## 3. CJK Languages — Approach Comparison

### 3.1 Available Approaches in Lucene/OpenSearch

For CJK, Lucene/OpenSearch offers a **tiered system** of increasing quality:

| Approach | How It Works | Quality | Deployment Complexity |
|---|---|---|---|
| **CJK Analyzer** (built-in) | Bigram tokenization — overlapping 2-character windows. `東京大学` → `東京`, `京大`, `大学` ([OpenSearch CJK Analyzer](https://opensearch.org/docs/latest/analyzers/language-analyzers/cjk/)) | Low — produces many spurious tokens; high recall but low precision | None — built-in |
| **ICU Analyzer** (plugin) | `icu_tokenizer` with dictionary-based segmentation for CJK. Uses ICU4J's `RuleBasedBreakIterator` with CJK dictionary. ([OpenSearch ICU Analyzer](https://docs.opensearch.org/latest/analyzers/language-analyzers/icu/)) | Medium — language-aware word boundaries; good for Chinese/Japanese general text | Install `analysis-icu` plugin |
| **SmartChinese** (plugin) | HMM-based probabilistic segmenter for Simplified Chinese. Segments sentences first, then words. ([Elasticsearch SmartChinese Plugin](https://elastic.co/docs/reference/elasticsearch/plugins/analysis-smartcn)) | Medium-High — trained on Chinese corpora; handles ambiguity better than pure dictionary | Install `analysis-smartcn` plugin |
| **Kuromoji** (Japanese, plugin) | Full morphological analyzer based on MeCab algorithm with IPAdic dictionary (~300k entries). Provides POS tagging, lemmatization, compound decomposition. ([Elasticsearch Kuromoji Plugin](https://www.elastic.co/guide/en/elasticsearch/plugins/current/analysis-kuromoji-tokenizer.html)) | High — industry standard for Japanese search | Install `analysis-kuromoji` plugin |
| **Nori** (Korean, plugin) | Morphological analyzer using `mecab-ko-dic` dictionary. Supports decompound modes (none/discard/mixed). ([Elasticsearch Nori Plugin](https://www.elastic.co/guide/en/elasticsearch/plugins/current/analysis-nori-tokenizer.html)) | High — handles Korean agglutination correctly | Install `analysis-nori` plugin |
| **Sudachi** (Japanese, Amazon OpenSearch) | Alternative Japanese tokenizer with multiple segmentation granularities (A/B/C units). ([AWS Announcement](https://aws.amazon.com/about-aws/whats-new/2023/10/amazon-opensearch-four-language-analyzers/)) | High — modern alternative to Kuromoji | Amazon OpenSearch plugin |

### 3.2 Detailed Comparison: ICU vs Specialized (OpenSearch/Lucene Context)

#### Chinese

| | ICU (`icu_tokenizer`) | SmartChinese | Jieba/cppjieba |
|---|---|---|---|
| **Algorithm** | Dictionary + LSTM model (`CjkBreakEngine`) | HMM probability + Viterbi algorithm | MMSEG + HMM for unknown words |
| **Dictionary size** | ICU built-in (general purpose, medium) | Embedded Chinese word dictionary | ~350k entries (configurable user dict) |
| **Example**: `苹果手机` | May produce `苹果` + `手机` or `苹果手机` depending on version | Reliably segments `苹果` + `手机` | Configurable; user dictionary can keep `苹果手机` as one token |
| **Custom dictionary** | Not easily customizable | No | Yes — trivial to add product names, brand names |
| **License** | Unicode/ICU License (permissive) | Apache 2.0 | MIT |

**Redis Search comparison**: Redis Search uses **Friso** for Chinese — a C library implementing the MMSEG algorithm (Maximum Matching with ambiguity detection) ([Redis Chinese Support](https://redis.io/docs/latest/develop/interact/search-and-query/advanced-concepts/chinese/), [Redis Forum on Friso](https://forum.redis.io/t/cant-query-with-chinese/274)). Friso is similar to cppjieba in approach (dictionary + algorithm) but is a different implementation. Valkey-search's Option A (ICU) would produce different segmentation from Redis Search's Friso.

#### Japanese

| | ICU | Kuromoji (Lucene) | MeCab |
|---|---|---|---|
| **Algorithm** | Dictionary-based `CjkBreakEngine` | Viterbi lattice search on IPAdic/UniDic dictionary | CRF-based lattice search on IPAdic |
| **Morphology** | Word boundaries only; no POS, no lemma | Full POS tagging + base form (lemmatization) + reading ([Atilika Kuromoji project](https://www.atilika.org/)) | Full POS + base form + reading + pronunciation |
| **Compound handling** | Limited — may keep compounds as single tokens | 3 modes: `normal` (no decomposition), `search` (decompose + keep original at same position), `extended` (decompose unknowns into unigrams) ([Elasticsearch Kuromoji Tokenizer](https://www.elastic.co/guide/en/elasticsearch/plugins/current/analysis-kuromoji-tokenizer.html)) | Via user dictionary or NEologd |
| **Example**: `東京大学` | May produce `東京大学` (1 token) or `東京` + `大学` — inconsistent | `search` mode: emits `東京大学` (pos 0) + `東京` (pos 0) + `大学` (pos 1) — both compound and parts are searchable | `東京` + `大学` or `東京大学` depending on dictionary |
| **License** | Unicode/ICU | Apache 2.0 (Java reimplementation) | LGPL (original C++) / Apache 2.0 (Lucene Java port) |

**Critical finding**: Kuromoji's `search` mode is a key differentiator — it emits both the compound word AND its constituent parts at the same position. This means a search for either `東京大学` or `大学` will match. ICU cannot replicate this behavior. This is the primary quality gap identified in the valkey-search proposal's appendix.

Kuromoji is a **pure Java reimplementation** of MeCab's Viterbi algorithm (Apache 2.0 licensed) ([Atilika Kuromoji](https://www.atilika.org/)). It is NOT a wrapper around MeCab's C library. The IPAdic dictionary data is compiled into a finite-state transducer (FST) format for efficient lookup. This means the LGPL concern with MeCab's C library does not apply to Lucene's Kuromoji.

#### Korean

| | ICU | Nori (Lucene) | Kiwi |
|---|---|---|---|
| **Algorithm** | Basic word boundary detection | Morphological analysis via `mecab-ko-dic` with Korean-specific grammar rules ([Elasticsearch Nori Plugin](https://www.elastic.co/guide/en/elasticsearch/plugins/current/analysis-nori.html)) | Probabilistic morphological analyzer ([kiwipiepy on PyPI](https://pypi.org/project/kiwipiepy/)) |
| **Agglutination** | Poor — `서울에서왔어요` often stays as one token or splits incorrectly | Correctly decomposes: `서울` + `에서` + `오` + `았` + `어요` | Similar decomposition quality to Nori |
| **Decompound modes** | N/A | `none` (no decompounding), `discard` (only parts), `mixed` (compound + parts) | N/A — always decompounds |
| **License** | Unicode/ICU | Apache 2.0 (Lucene reimplementation) | **LGPL-2.1** (not MIT as previously stated in low-level doc) ([jsDelivr kiwi-nlp CDN metadata](https://www.jsdelivr.com/package/npm/kiwi-nlp)) |

**License correction**: Research indicates Kiwi is licensed under **LGPL-2.1-or-later**, not MIT. This is the same license constraint as MeCab, meaning static linking is problematic. The valkey-search proposal's Option B table lists Kiwi as MIT — this should be verified and corrected.

**Nori's architecture**: Like Kuromoji, Lucene's Nori is a **pure Java reimplementation** that uses the `mecab-ko-dic` dictionary data but does NOT link against MeCab's C/C++ library. The dictionary is compiled into Lucene's FST format. The Apache 2.0 license applies to the Nori code itself. ([Elasticsearch Nori Plugin](https://www.elastic.co/guide/en/elasticsearch/plugins/current/analysis-nori.html), [Elastic Blog: Nori](https://www.elastic.co/jp/blog/nori-the-official-elasticsearch-plugin-for-korean-language-analysis))

#### Polish

| | ICU | Stempel (Lucene) | Morfeusz |
|---|---|---|---|
| **Algorithm** | Standard Latin word boundaries (space-delimited) | Algorithmic stemmer using pre-trained tables from Polish corpus. Based on Egothor stemmer engine. ([Lucene Stempel Documentation](https://lucene.apache.org/core/8_6_0/analyzers-stempel/index.html)) | Full morphological dictionary + grammar rules |
| **Quality** | Tokenization is correct (Polish uses spaces); no stemming | Algorithmic stemming trained on extensive Polish corpus — handles fusional morphology well for an algorithmic approach | Gold-standard morphological analysis |
| **License** | Unicode/ICU | Apache 2.0 (tables) + Egothor License (stemmer engine) — both allow commercial use | BSD (2-clause) |

**Key finding**: Lucene already has a **high-quality Polish stemmer** (Stempel) that is more capable than raw ICU. OpenSearch's Polish analyzer (requires plugin) uses Stempel ([OpenSearch Language Analyzers](https://docs.opensearch.org/latest/analyzers/language-analyzers/index/)). Stempel operates by training stemming tables from a large Polish corpus rather than using hand-written suffix rules — this makes it much more effective for fusional languages than Snowball-style approaches.

**Implication for valkey-search**: For Polish, the choice isn't just ICU vs Morfeusz. Stempel represents a middle ground — algorithmic stemming without the deployment complexity of Morfeusz, but significantly better quality than ICU-only (which provides zero stemming for Polish). The Stempel algorithm is open-source (Apache 2.0 + Egothor), but is implemented in Java. A C++ port would be required.

#### Vietnamese

| | ICU | CocCoc Tokenizer | RDRsegmenter |
|---|---|---|---|
| **Algorithm** | Standard Latin word boundaries — problematic because Vietnamese multi-syllable words use spaces between syllables | Dictionary + DAG-based tokenizer optimized for Vietnamese ([CocCoc on GitHub](https://github.com/coccoc/coccoc-tokenizer)) | Rule-based segmenter (Ripple Down Rules) ([RDRsegmenter on GitHub](https://github.com/datquocnguyen/RDRsegmenter)) |
| **Example**: `xe đạp` (bicycle) | Splits into `xe` + `đạp` — incorrect, loses compound word semantics | Correctly identifies `xe đạp` as one token | Correctly identifies `xe đạp` as one token |
| **License** | Unicode/ICU | **GPL-3.0** | MIT (Java) |

**Key finding**: ICU is **insufficient for Vietnamese** — it treats each syllable as a separate word because Vietnamese uses spaces between syllables of multi-word terms ([Vietnamese Word Segmentation research](https://www.researchgate.net/publication/220706845_Vietnamese_Word_Segmentation)). This is the same problem identified in the valkey-search proposal. Without a specialized Vietnamese segmenter, search quality will be poor.

Both OpenSearch and Elasticsearch have no built-in Vietnamese analyzer. The community typically uses either:
1. ICU tokenizer (produces syllable-level tokens — low quality)
2. Custom plugins wrapping CocCoc or similar tools

CocCoc's GPL-3.0 license makes it impractical for valkey-search (as noted in the proposal). The proposal correctly identifies this gap.

---

## 4. Key Architectural Differences Summary

| Aspect | Lucene/OpenSearch | Redis Search | Valkey-Search (Proposed) |
|---|---|---|---|
| **Language granularity** | Per-field (via multi-field mappings) | Per-index or per-document | Per-index |
| **Stemmer options** | Multiple per language (Snowball, Light, Minimal, Hunspell) | Snowball only (libstemmer C) | Snowball only (same as Redis Search) |
| **CJK approach** | Tiered: bigram → ICU → specialized (Kuromoji/Nori) | Friso (Chinese only); no Japanese/Korean | ICU (Option A) or specialized (Option B) |
| **Polish** | Stempel (trained algorithmic stemmer) | No specific support | ICU only (no stemming) |
| **Vietnamese** | No built-in support | No specific support | ICU only (incorrect segmentation) |
| **Pipeline composability** | Fully composable (char filters → tokenizer → token filters) | Monolithic per language | Strategy pattern; less composable than Lucene |
| **Custom dictionaries** | Supported for Kuromoji, Nori, Hunspell | Supported for Friso (Chinese) | Not mentioned in proposal |
| **Mixed-language documents** | Multi-field mappings allow per-language analysis of same content | Per-document language override | Single language per index; ICU is script-aware for Latin in CJK indexes |

---

## 5. Findings Relevant to Valkey-Search

### 5.1 For Snowball Languages — Valkey-Search Is Well-Aligned

The proposed approach (Snowball + NFC normalization + per-language stop words) matches Redis Search exactly and produces equivalent algorithmic output to Lucene's Snowball token filter. This is the correct design for Redis Search migration compatibility.

**Potential enhancement for future releases**: Light stemmers could be offered as an alternative for customers migrating from Elasticsearch/OpenSearch (where light stemmers are the default for French, German, Italian, Portuguese). These are simple algorithms (100-200 lines of C per language) that could be added as an additional `ILanguageProcessor` subclass.

### 5.2 For CJK — ICU Is a Reasonable Baseline but Has Known Gaps

| Language | ICU Quality vs Lucene Specialized | Gap Severity |
|---|---|---|
| **Chinese** | Moderate gap — ICU dictionary segmentation is reasonable but less tuneable than Kuromoji/SmartCN/Jieba. No custom dictionary support. | Medium — most common words segment correctly; domain-specific terms (product names, proper nouns) may fail |
| **Japanese** | Significant gap — ICU lacks compound decomposition modes, POS-based filtering, and base form (lemma) extraction. Kuromoji's `search` mode (emit compound + parts) cannot be replicated. | High — Japanese search quality will be noticeably lower than Elasticsearch/OpenSearch |
| **Korean** | Significant gap — ICU cannot decompose agglutinative forms. Searching `서울` won't find `서울에서왔어요`. Nori/mecab-ko-dic handle this correctly. | High — many natural Korean queries will return fewer results |
| **Polish** | Moderate gap — tokenization is correct (Polish uses spaces), but no stemming. Lucene's Stempel provides good algorithmic stemming. | Medium — recall will be lower without stemming for a highly inflected language |
| **Vietnamese** | Critical gap — ICU produces incorrect syllable-level tokens. | Critical — search will be effectively broken for multi-syllable Vietnamese words |

### 5.3 Redis Search Compatibility

For customers migrating from Redis Search:
- **Snowball languages**: Valkey-search will be **identical** (same libstemmer C algorithms)
- **Chinese**: Redis Search uses Friso (MMSEG); valkey-search ICU will produce **different segmentation** — some queries may match differently
- **Japanese/Korean/Polish/Vietnamese**: Redis Search has no support; valkey-search with ICU provides something rather than nothing

### 5.4 OpenSearch/Elasticsearch Compatibility

For customers migrating from OpenSearch/Elasticsearch:
- **Snowball languages**: Stemming output will differ where ES/OS uses light stemmers by default (French, German, Italian, Portuguese, etc.)
- **CJK**: Significant quality gap if ICU-only vs ES's Kuromoji/Nori
- **Arabic**: Equivalent (both use algorithmic stemming + normalization)

### 5.5 Observations on Specialized Library Licensing

| Library | Claimed License (in proposal) | Actual License (verified) | Static Linking OK? |
|---|---|---|---|
| cppjieba (Chinese) | MIT | MIT | ✅ Yes |
| MeCab (Japanese) | LGPL | LGPL-2.1 | ❌ No — requires dynamic linking |
| Kiwi (Korean) | MIT | **LGPL-2.1-or-later** ([jsDelivr metadata](https://www.jsdelivr.com/package/npm/kiwi-nlp)) | ❌ No — same constraint as MeCab |
| Morfeusz (Polish) | BSD | BSD-2-Clause | ✅ Yes |
| CocCoc (Vietnamese) | GNU (noted as impractical) | GPL-3.0 | ❌ No |

**License correction needed**: The low-level design doc states Kiwi is MIT-licensed. Multiple sources (PyPI, jsDelivr CDN metadata) indicate it is LGPL-2.1-or-later. This changes the feasibility analysis for Option B — Korean would have the **same deployment constraints as Japanese** (dynamic linking required, `dlopen()` with fallback).

### 5.6 Alternative Approach: Lucene-Style Reimplementation

Lucene solved the MeCab/mecab-ko-dic licensing problem by **reimplementing the algorithm in Java** (Apache 2.0) while using the dictionary data (which has a separate, permissive license) ([Atilika Kuromoji](https://www.atilika.org/), [Elasticsearch Nori Plugin](https://www.elastic.co/guide/en/elasticsearch/plugins/current/analysis-nori.html)). This approach:

- Kuromoji: Java reimplementation of Viterbi lattice search → Apache 2.0
- Nori: Java reimplementation using mecab-ko-dic data → Apache 2.0

A similar approach for valkey-search would be:
- **Reimplement the Viterbi algorithm in C++** (straightforward — it's a well-documented algorithm)
- **Use IPAdic dictionary data** (Apache 2.0 licensed) compiled into an FST
- **Use mecab-ko-dic data** (Apache 2.0 licensed) for Korean

This would give Kuromoji/Nori-equivalent quality without any LGPL dependency. The effort is higher than wrapping existing libraries but avoids all licensing concerns. This is what Lucene did, and it's proven to work at massive scale.

---

## 6. Per-Language Recommendations

For each of the 12 Snowball-targeted languages, the following table provides a top-choice recommendation considering quality, licensing, and static-linkability.

| # | Language | Top Choice | Rationale | License Concern? | Fallback if Top Choice Not Viable |
|---|---|---|---|---|---|
| 1 | **English** | **Snowball** | Industry standard; identical output to Redis Search and Lucene's Snowball filter. Porter/Porter2 algorithm is well-proven. No quality gap vs ES/OS (which also defaults to Snowball for English). | None — BSD/MIT (libstemmer C) | N/A |
| 2 | **French** | **Snowball** | Identical output to Redis Search (migration compatibility). ES/OS defaults to a light stemmer, but Snowball provides higher recall at cost of slightly lower precision. For a Redis Search replacement, Snowball is the correct choice. | None | Light stemmer (port from Lucene `FrenchLightStemmer`, Apache 2.0, ~200 lines of C) if precision over recall is preferred |
| 3 | **German** | **Snowball** | Same rationale as French — Redis Search compatibility. German Snowball handles umlauts and compound-word suffixes. ES/OS uses a light stemmer + `GermanNormalizationFilter` (ä→a, ö→o, ü→u, ß→ss) by default. | None | Light stemmer + German normalization filter (Lucene `GermanLightStemmer` + `GermanNormalizationFilter`, Apache 2.0) if ES/OS parity is needed |
| 4 | **Spanish** | **Snowball** | Good quality for Spanish morphology. Redis Search compatibility. ES/OS defaults to light stemmer but the difference is minimal for Spanish. | None | Light stemmer (Lucene `SpanishLightStemFilter`, Apache 2.0) |
| 5 | **Italian** | **Snowball** | Redis Search compatibility. Italian Snowball handles verb conjugations and plurals well. ES/OS defaults to light stemmer. | None | Light stemmer (Lucene `ItalianLightStemFilter`, Apache 2.0) |
| 6 | **Portuguese** | **Snowball** | Redis Search compatibility. ES/OS defaults to a light stemmer derived from the UniNE algorithm ([Lucene PortugueseLightStemmer](https://lucenenet.apache.org/docs/4.8.0-beta00017/api/analysis-common/Lucene.Net.Analysis.Pt.html)). Snowball is more aggressive but provides higher recall. | None | Light stemmer (Lucene `PortugueseLightStemFilter`, Apache 2.0) |
| 7 | **Russian** | **Snowball** | Both Redis Search and ES/OS use Snowball for Russian. No quality gap. Russian Snowball handles Cyrillic suffixes correctly. | None | N/A — Snowball is the ecosystem standard |
| 8 | **Swedish** | **Snowball** | Both Redis Search and ES/OS use Snowball for Swedish. No quality gap. | None | Light stemmer available in Lucene (`SwedishLightStemFilter`) if over-stemming is observed |
| 9 | **Turkish** | **Snowball** | Both Redis Search and ES/OS use Snowball for Turkish. Handles Turkish-specific morphology (agglutinative suffixes, vowel harmony). Important: Turkish requires locale-aware case folding (`İ`→`i`, `I`→`ı`) which ICU case-fold handles correctly. | None | N/A — Snowball is the ecosystem standard |
| 10 | **Dutch** | **Snowball** | Both Redis Search and ES/OS use Snowball for Dutch ([Elasticsearch Snowball Token Filter](https://www.elastic.co/guide/en/elasticsearch/reference/current/analysis-snowball-tokenfilter.html)). Lucene also offers a Kp (Kraaij-Pohlmann) stemmer variant ([Snowball algorithms page](https://snowballstem.org/algorithms/)) but the standard Dutch Snowball is sufficient. | None | Minimal stemmer (Lucene `DutchMinimalStemmer`, plural removal only) if over-stemming is a concern |
| 11 | **Indonesian** | **Snowball** | Both Redis Search and ES/OS use Snowball for Indonesian. Indonesian morphology is primarily prefixing/suffixing — Snowball handles the common affixes (me-, ber-, di-, ke-...-an, -kan, -i). | None | N/A — Snowball is the ecosystem standard |
| 12 | **Arabic** | **Snowball + NFKC** | Snowball Arabic stemmer provides root extraction compatible with Redis Search. NFKC normalization (as proposed) handles presentation form collapsing that NFC cannot. ES/OS uses a slightly different `ArabicStemmer` + `ArabicNormalizationFilter` pipeline, but the net effect is similar. Extended punctuation set (Arabic comma ،, question mark ؟, semicolon ؛) must be added to defaults. | None | ICU-based Arabic normalization (ICU handles Arabic presentation forms natively via NFKC) — already the proposed approach |

### CJKPV Languages

| # | Language | Top Choice | Rationale | License Concern? | Fallback if Top Choice Not Viable |
|---|---|---|---|---|---|
| 13 | **Chinese** | **cppjieba** (third-party) | Header-only C++ library; dictionary-based MMSEG + HMM segmentation; supports custom user dictionaries for domain-specific terms (product names, proper nouns); closest in approach to Redis Search's Friso. ICU is a viable baseline but lacks custom dictionary support and produces less predictable segmentation for compound terms. | None — MIT license, header-only, static linking trivial | **ICU** (`BreakIterator` with `"zh"` locale) — already integrated, no new dependencies; quality is medium but acceptable for general text |
| 14 | **Japanese** | **C++ Viterbi reimplementation** using IPAdic dictionary data (Lucene-style approach) | Provides Kuromoji-equivalent morphological analysis (compound decomposition, base form extraction) without LGPL dependency. Lucene proved this approach works at scale. IPAdic dictionary data is Apache 2.0 licensed. | None — algorithm reimplementation is original code; dictionary data is Apache 2.0 | **ICU** (`BreakIterator` with `"ja"` locale) — significant quality gap (no compound decomposition, no POS filtering) but zero additional engineering effort. **MeCab** (`dlopen()` with ICU fallback) if reimplementation effort is too high — ⚠️ LGPL-2.1, cannot statically link, requires `libmecab.so` on customer system |
| 15 | **Korean** | **C++ Viterbi reimplementation** using mecab-ko-dic dictionary data (Lucene Nori-style approach) | Same rationale as Japanese — provides proper morphological decomposition of agglutinative forms. mecab-ko-dic data is Apache 2.0 licensed. Without this, searching `서울` cannot find documents containing `서울에서왔어요`. | None — algorithm reimplementation is original code; dictionary data is Apache 2.0 | **ICU** (`BreakIterator` with `"ko"` locale) — significant quality gap (cannot decompose agglutinative particles) but zero additional effort. **Kiwi** — ⚠️ LGPL-2.1 (not MIT as stated in original proposal), same static-linking constraint as MeCab |
| 16 | **Polish** | **Stempel** (C++ port from Lucene) | Algorithmic stemmer trained on extensive Polish corpus; handles fusional morphology far better than ICU-only (which provides zero stemming). Used by OpenSearch's Polish analyzer plugin. Significantly less complex to deploy than Morfeusz while providing good quality. | None — Apache 2.0 (tables) + Egothor License (engine); both permit commercial use and static linking | **Morfeusz** (BSD-2-Clause, statically linkable) — gold-standard morphological analysis but higher integration complexity. If Stempel port effort is too high: **ICU** (`BreakIterator` with `"pl"` locale) — correct tokenization but no stemming whatsoever, resulting in lower recall for a highly inflected language |
| 17 | **Vietnamese** | **RDRsegmenter** (C++ port) | Rule-based Vietnamese word segmenter that correctly handles multi-syllable compound words (e.g., `xe đạp` → one token). MIT-licensed (Java implementation). ICU is actively broken for Vietnamese (splits syllables incorrectly). CocCoc is higher quality but GPL-3.0 (cannot use). | Requires C++ port from Java — MIT license permits this freely | **ICU** — ⚠️ produces **incorrect** syllable-level tokens for Vietnamese; effectively broken for multi-syllable words. Only acceptable as a placeholder with explicit documentation that Vietnamese search quality is limited. **CocCoc** — ⚠️ GPL-3.0, cannot statically link or distribute |

### Summary of Recommendations

**Snowball languages (1–12): Snowball is the recommended top choice for all.**

This is driven by:
1. **Redis Search migration compatibility** — identical stemming output (same libstemmer C algorithms)
2. **Proven quality** — Snowball stemmers have decades of use in production search systems
3. **Zero licensing concerns** — BSD-licensed libstemmer C
4. **Static linking** — trivially statically linked (already integrated in valkey-search)
5. **No additional dependencies** — all 12 languages compile from the same Snowball codebase via `add_language.sh`

**Where Snowball may be suboptimal** (but is still recommended for 1.4):

For French, German, Spanish, Italian, and Portuguese, Elasticsearch/OpenSearch defaults to **light stemmers** which are less aggressive and produce fewer false conflations. If customer feedback after 1.4 launch indicates over-stemming issues for these languages, light stemmer alternatives can be added as a follow-on enhancement. The light stemmer algorithms are simple (100-200 lines per language), Apache 2.0 licensed, and fit cleanly into the `ILanguageProcessor` interface as an additional `LightStemmerProcessor` subclass.

**No third-party library is recommended for any of the 12 Snowball languages.** Hunspell (dictionary-based stemming) exists as an alternative in the Lucene ecosystem but adds significant complexity (dictionary file management, larger memory footprint, slower performance) with marginal quality improvement for the languages in scope. It is not worth the trade-off.

**CJKPV languages (13–17): Specialized approaches recommended, with ICU as fallback.**

The key themes:
1. **Chinese**: cppjieba (MIT, header-only) is the simplest high-quality integration. ICU is an acceptable baseline.
2. **Japanese & Korean**: The Lucene-style reimplementation path (Viterbi algorithm in C++ + Apache 2.0 dictionary data) is the recommended long-term approach. It avoids LGPL entirely while matching Elasticsearch/OpenSearch quality. If engineering bandwidth is constrained, ICU is the fallback — but with documented quality limitations.
3. **Polish**: Stempel (Apache 2.0) fills the gap between "no stemming" (ICU) and "full morphological analysis" (Morfeusz). A C++ port is recommended.
4. **Vietnamese**: The hardest language. ICU is broken, CocCoc is GPL. RDRsegmenter (MIT, Java) is the only viable permissively-licensed option but requires a C++ port.

---

## 7. Recommendations for the CJKPV Comparison Document

### For the "Comparing ICU vs Specialized Libraries" section:

1. **Acknowledge the three-tier model** from OpenSearch: bigram (lowest quality), ICU (medium), specialized (highest). Valkey-search is choosing between middle and top tier.

2. **Correct the Kiwi license** from MIT to LGPL-2.1. This means Option B has TWO LGPL dependencies (MeCab and Kiwi), not one.

3. **Consider the Lucene reimplementation path** as a third option (Option C): Reimplement Viterbi in C++ using the permissively-licensed dictionary data directly. Higher engineering effort but zero licensing issues and deployment friction identical to Option A.

4. **Note the Vietnamese gap explicitly**: ICU is effectively broken for Vietnamese. Both options (A and B) need a solution here. Since CocCoc is GPL and the proposal already acknowledges this, the real choice for Vietnamese may be between "broken (ICU syllable splitting)" and "build a custom segmenter."

5. **Document the Stempel option for Polish**: A C++ port of Lucene's Stempel algorithm would provide meaningful Polish stemming without the complexity of Morfeusz, and is fully Apache 2.0 licensed.

6. **For Snowball languages**: Note that valkey-search will be algorithmically identical to Redis Search but will differ from Elasticsearch/OpenSearch defaults for French, German, Italian, and Portuguese (which use light stemmers). This is a conscious compatibility choice favoring Redis Search migration.

---

## 8. Summary Table: Valkey-Search Proposal vs Ecosystem

| Language | Redis Search | Elasticsearch/OpenSearch (Default) | Valkey-Search (Proposed) | Gap vs ES/OS |
|---|---|---|---|---|
| English | Snowball | Snowball (Porter) | Snowball | None |
| French | Snowball | Light stemmer | Snowball | Minor (more aggressive stemming) |
| German | Snowball | Light stemmer + normalization | Snowball | Minor |
| Spanish | Snowball | Light stemmer | Snowball | Minor |
| Italian | Snowball | Light stemmer | Snowball | Minor |
| Portuguese | Snowball | Light stemmer | Snowball | Minor |
| Russian | Snowball | Snowball | Snowball | None |
| Swedish | Snowball | Snowball | Snowball | None |
| Turkish | Snowball | Snowball | Snowball | None |
| Dutch | Snowball | Snowball | Snowball | None |
| Indonesian | Snowball | Snowball | Snowball | None |
| Arabic | Snowball | Arabic stemmer + normalization | Snowball + NFKC | Minimal |
| Chinese | Friso (MMSEG) | ICU / SmartChinese | ICU | Medium vs SmartCN |
| Japanese | None | Kuromoji (MeCab-based, Apache 2.0) | ICU | **High** |
| Korean | None | Nori (mecab-ko-dic, Apache 2.0) | ICU | **High** |
| Polish | None | Stempel (algorithmic, Apache 2.0) | ICU (no stemming) | **Medium-High** |
| Vietnamese | None | None (no built-in) | ICU (broken segmentation) | **Critical** (both lack support, but ICU actively misleads) |
