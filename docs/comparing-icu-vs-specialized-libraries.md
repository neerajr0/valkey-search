# Multi Language Support - CJKPV Languages

## Background
As part of multi-language support for planned for Valkey Search 1.4, we plan to support languages that will not use standard tokenization or stemming supported by Snowball. These include Chinese, Japanese, Korean, Polish and Vietnamese. 

These languages exhibit characteristics that making processing with Snowball difficult, since its stemming algorithms are designed for whitespace-delimited words with suffix-based morphology. To provide some examples:

1. Chinese and Vietnamese are considered **isolating** languages – each word usually maps to one morpheme without the use of inflectional suffixes. Therefore stemming is not applicable and tokenization on whitespace can lead to misinterpretation of the sentence.
In Chinese,

我喜欢跑步 = 我 / 喜欢 / 跑步 = I / like / running 

"喜欢" (like) is two characters that form one word, but "欢跑" (spanning the boundary) is meaningless. You cannot split on whitespace because there is none.

In Vietnamese, 

xe đạp = one word (bicycle), two syllables  
xe máy = one word (motorcycle), two syllables

If you split on whitespace, "xe" (vehicle) appears alone and matches both bicycles and motorcycles indiscriminately.

1. Japanese and Korean are considered **agglutinative**languages – they combine multiple morphemes into one word. Tokenization therefore requires morphological analysis to determine grammatical, not character boundaries.
In Japanese, 食べる *taberu* (to eat) becomes 食べさせられなかった *tabesaserarenakatta* (was not made to eat), where each suffix (-*sase* for causative, *-rare* for passive, -*na* for negative, -*katta* for past tense) is cleanly stuck onto the root.

1. Polish is considered a **synthetic** language – a single suffix often blends multiple grammatical categories at once. For example:
kot-ów = of the cats

kot / -ów
cat / genitive + plural + masculine (all in one suffix)

It is also **fusional** – the interior of words change when a suffix is appended. For example:

| A | Base (nominative) | "of the ___" (genitive) | Interior change |
| --- | --- | --- | --- |
| dog | pies | psa | "ie" vanishes → `ps` + `-a` |
| dream | sen | snu | "e" vanishes → `sn` + `-u` |

## Goals
As described in [3. CJKPV Languages: [if scoped for 1.3]: Multi language support plan - low level details](https://quip-amazon.com/03HdAmpInFuD#temp:C:EMEa102aeaa4b4844adbf215fbf4), there are currently two options to handle processing these languages: 

[Option A: ICU (No New Libraries): Multi language support plan - low level details](https://quip-amazon.com/03HdAmpInFuD#temp:C:EMEcb68fc6424e149dba5c0f81a5) - We use `CjkBreakEngine` from the already integrated ICU (International Components for Unicode) library to tokenize Chinese, Japanese and Korean, and the `BreakIterator` API to tokenize Polish and Vietnamese. 

[Option B: Specialized Libraries: Multi language support plan - low level details](https://quip-amazon.com/03HdAmpInFuD#temp:C:EME0f9adf07a3874115a5fb8f915) - We use specialized libraries to handle CJKPV languages (and any other languages if applicable), which provide more morphology-aware tokenization. 

This document aims to:

1. Compare the expected performance of full-text search for CJKPV languages between ICU and Specialized Libraries, including:
  1. Researching any other implementations that could yield higher performance for CJKPV languages.
  2. Determining if the current languages that we will be using Snowball for would have higher search performance if specialized libraries are used.
  3. If specialized libraries are used, checking if these are open-source friendly and have licenses permissible for use by AWS. If so, confirm that they can be linked statically so that they don’t have to be loaded at runtime.

## Comparing ICU vs Specialized Libraries

### How Other Search Engines Handle Multi-Language FTS

#### Lucene/OpenSearch/Elasticsearch: Tiered Approach
Lucene-based engines (Elasticsearch, OpenSearch) use a composable analyzer pipeline (`Character Filters → Tokenizer → Token Filters`) and offer a **tiered system** of CJK support with increasing quality:

| Tier | Component | How It Works | Quality |
| --- | --- | --- | --- |
| 1 (Lowest) | **CJK Analyzer** (built-in) | Bigram tokenization — overlapping 2-character windows. `東京大学` → `東京`, `京大`, `大学`. Produces many spurious tokens. | Low precision, high recall |
| 2 (Medium) | **ICU Analyzer** (plugin) | `icu_tokenizer` with dictionary-based segmentation via ICU's `RuleBasedBreakIterator` + `CjkBreakEngine`. Language-aware word boundaries. | Medium — good for general text |
| 3 (High) | **Kuromoji** (Japanese), **Nori** (Korean), **SmartChinese** (Chinese) | Full morphological analysis — POS tagging, lemmatization, compound decomposition, custom dictionaries. | High — industry standard |
Key architectural properties:

- Different fields in the same document can have different language analyzers (multi-field mappings)
- Specialized analyzers are installed as **plugins** — they extend the system without modifying core code
- Kuromoji and Nori are **pure Java reimplementations** of MeCab's Viterbi algorithm (Apache 2.0 licensed) — they do NOT link against MeCab's LGPL C library
OpenSearch supports 35+ built-in language analyzers for Snowball-style languages and offers ICU, Kuromoji, Nori, and Sudachi as plugin-based analyzers for CJK. ([OpenSearch Language Analyzers](https://docs.opensearch.org/latest/analyzers/language-analyzers/index/), [OpenSearch ICU Analyzer](https://docs.opensearch.org/latest/analyzers/language-analyzers/icu/))

#### Redis Search: Friso for Chinese, No CJK Otherwise
Redis Search uses a model closer to valkey-search's proposed architecture:

- Language is declared per-index or per-document
- Uses **Snowball** (libstemmer C) for 15+ stemming languages
- Uses **Friso** for Chinese — a C library implementing the MMSEG algorithm (Maximum Matching with ambiguity detection). Dictionary-based segmentation similar in approach to cppjieba. ([Redis Chinese Support](https://redis.io/docs/latest/develop/interact/search-and-query/advanced-concepts/chinese/), [Redis Forum](https://forum.redis.io/t/cant-query-with-chinese/274))
- **No support** for Japanese, Korean, Polish, or Vietnamese in the open-source version
This means:

- For Chinese, valkey-search with ICU would produce **different segmentation** than Redis Search's Friso
- For Japanese/Korean/Polish/Vietnamese, valkey-search with ICU provides something where Redis Search has nothing

### Per-Language Comparison

#### Chinese

##### ICU Segmentation (BreakIterator + CjkBreakEngine)
ICU's `BreakIterator::createWordInstance("zh")` activates the `CjkBreakEngine`, which uses a dictionary + LSTM model for Chinese word segmentation. This engine is already present in valkey-search's integrated ICU library (`third_party/icu/source/common/brkeng.cpp`).

**Strengths:**

- Already integrated — zero new dependencies
- General-purpose dictionary handles common vocabulary correctly
- Script-aware — Latin characters (brand names like "iPhone") in Chinese text are tokenized correctly on word boundaries
**Weaknesses:**

- Dictionary is not easily customizable — cannot add domain-specific terms (product names, slang)
- Segmentation of ambiguous compounds can be unpredictable across ICU versions
- No user dictionary mechanism
**Example:** `苹果手机` (Apple phone / iPhone)

- ICU may produce `苹果` + `手机` or `苹果手机` as a single unit depending on dictionary coverage and LSTM model confidence

##### Specialized: cppjieba (MIT, Header-Only)
[cppjieba](https://github.com/yanyiwu/cppjieba) is a C++ implementation of the Jieba Chinese word segmentation algorithm. It uses MMSEG + HMM for unknown word detection.

**Strengths:**

- **Header-only** — trivial integration, no separate build step
- **MIT license** — no static linking restrictions
- ~350k dictionary entries with support for **custom user dictionaries**
- Well-tested in production (jieba ecosystem has millions of users)
- Predictable, deterministic segmentation
- Performance benchmarks show jieba-rs (Rust port) is 33% faster than cppjieba; cppjieba itself is fast for C++
**Weaknesses:**

- Adds ~5MB dictionary data to the binary/distribution
- One more dependency to maintain
- Algorithm tuned for Simplified Chinese primarily (Traditional Chinese via alternative dictionaries)
**Example:** `苹果手机`

- Default: `苹果` + `手机` (correct segmentation)
- With custom dict: can keep `苹果手机` as one token if configured

##### Comparison with Redis Search (Friso) and OpenSearch (SmartChinese)

| A | Redis Search (Friso) | OpenSearch (SmartChinese) | ICU | cppjieba |
| --- | --- | --- | --- | --- |
| Algorithm | MMSEG | HMM + Viterbi | Dictionary + LSTM | MMSEG + HMM |
| Custom dictionary | Yes | No | No | Yes |
| Unknown word handling | Single-character fallback | HMM-based | LSTM model | HMM-based (handles neologisms, product names) |
| License | MIT | Apache 2.0 | Unicode/ICU | MIT |
| Language | C | Java | C/C++ | **C++ (header-only)** |
| Static linking | Yes | N/A (Java) | Yes | Yes (header-only, trivial) |
| C++ integration effort | Moderate — C library requires wrapper, separate build step, manual memory management | N/A (Java, cannot use) | None (already integrated) | **Minimal — header-only, native C++ API, no build step** |
| Community / ecosystem | Small — single maintainer, limited activity | Apache Lucene project | ICU project (large) | **Large — jieba ecosystem has millions of users across Python/Rust/C++/Go ports; actively maintained** |
| Migration compatibility with Redis Search | — | Low | Low | **Medium-High** (same algorithmic family as Friso) |
| **Why not for valkey-search** | **Not viable**: C library with no C++ API; requires foreign build integration into CMake; smaller community with limited maintenance; no HMM for unknown words; cppjieba provides the same MMSEG algorithm with superior C++ ergonomics and additional HMM capability | Not viable: Java-only implementation | Viable as fallback (already integrated) but lower quality — no custom dictionaries, less predictable compound segmentation | **Recommended** |

##### Recommendation
**Top choice: cppjieba** — MIT licensed, header-only, supports custom dictionaries, same algorithmic family (MMSEG) as Redis Search's Friso. Produces predictable, high-quality segmentation.

**Fallback: ICU** — already integrated, acceptable quality for general text, zero additional dependencies. Use if minimizing new code is the priority for 1.4.

#### Japanese

##### ICU Segmentation (BreakIterator "ja")
ICU's Japanese segmentation uses the `CjkBreakEngine` with a Japanese dictionary variant.

**Strengths:**

- Already integrated
- Handles basic word boundary detection for common vocabulary
**Weaknesses:**

- **No compound decomposition modes** — cannot emit both a compound and its parts (Kuromoji's killer feature)
- **No POS tagging** — cannot filter particles, auxiliary verbs
- **No base form extraction** — `食べた` (ate) is not reduced to `食べる` (to eat)
- Inconsistent segmentation of compound nouns: `東京大学` may stay as one token or split into `東京` + `大学` unpredictably
- Cannot be tuned with user dictionaries
**Example:** `東京大学` (Tokyo University)

- ICU: may produce `東京大学` (1 token) OR `東京` + `大学` — **inconsistent**
- Impact: searching `大学` (university) may or may not find documents containing `東京大学`

##### Specialized: MeCab / Kuromoji-Style Reimplementation
Two paths exist for high-quality Japanese:

**Path 1: MeCab C library (dlopen)**

- Full morphological analysis, POS tagging, base form extraction
- IPAdic dictionary (~300k entries including proper nouns, place names)
- MeCab is tri-licensed: **GPL-2.0 OR LGPL-2.1 OR BSD-3-Clause** (user's choice). Under the BSD option, static linking is technically permitted. However, practical deployment concerns remain: large dictionary files (30–100MB), runtime configuration, and the complexity of bundling/managing dictionary data make a self-contained `.so` module difficult.
- Requires dictionary download mechanism (IPAdic is 30–100MB)
**Path 2: Kuromoji-style C++ reimplementation (recommended)**

- Reimplement the Viterbi lattice-search algorithm in C++ (well-documented, finite-state approach)
- Use **IPAdic dictionary data** (Apache 2.0 licensed) compiled into an FST format
- This is exactly what Lucene did — Kuromoji is a pure Java reimplementation, NOT a MeCab wrapper

##### The Lucene Approach: Viterbi Reimplementation with IPAdic (Apache 2.0)
Lucene solved the MeCab deployment complexity problem definitively:

1. Reimplemented the Viterbi lattice-search algorithm from scratch in Java
2. Used IPAdic dictionary data (Apache 2.0) compiled into Lucene's FST format
3. Added search-specific features like compound decomposition modes:

  - `normal`: no decomposition
  - `search`: emit BOTH the compound AND its constituent parts at the same position
  - `extended`: decompose unknown words into unigrams
The `search` mode is the key differentiator — for `東京大学`, Kuromoji emits:

  - `東京大学` at position 0 (compound)
  - `東京` at position 0 (part)
  - `大学` at position 1 (part)
This means searching for either `東京大学` OR `大学` finds the document. ICU cannot replicate this behavior.
([Atilika Kuromoji](https://www.atilika.org/), [Elasticsearch Kuromoji Plugin](https://www.elastic.co/guide/en/elasticsearch/plugins/current/analysis-kuromoji-tokenizer.html))

##### Recommendation
**Top choice: C++ Viterbi reimplementation with IPAdic dictionary data** — provides Kuromoji-equivalent quality (compound decomposition, base form extraction) with zero LGPL dependency. IPAdic data is Apache 2.0. Higher engineering effort (~3-4 weeks) but the algorithm is well-documented and Lucene's implementation serves as a reference.

**Fallback (if reimplementation effort is too high for 1.4):** ICU — significant quality gap (no compound decomposition, no POS, no base form) but zero additional effort. Document the limitation explicitly.

**Not recommended despite BSD option:** MeCab C library — while MeCab is tri-licensed (GPL-2.0 OR LGPL-2.1 OR BSD-3-Clause) and the BSD option technically permits static linking, the practical deployment burden remains: 30–100MB dictionary data, runtime configuration files, and ongoing dictionary maintenance. The Viterbi reimplementation approach uses only the dictionary *data* (IPAdic, Apache 2.0) compiled into an FST, avoiding the MeCab runtime entirely.

#### Korean

##### ICU Segmentation (BreakIterator "ko")
ICU provides basic Korean word boundary detection but cannot perform morphological decomposition.

**Strengths:**

- Already integrated
- Handles basic space-delimited Korean text
**Weaknesses:**

- **Cannot decompose agglutinative forms** — Korean combines root + particles/suffixes into single orthographic words
- `서울에서왔어요` (I came from Seoul) stays as one token or splits incorrectly
- Searching `서울` will NOT find documents containing `서울에서왔어요`
- No morpheme extraction
**Example:** `서울에서왔어요`

- ICU: `서울에서왔어요` (1 token) — searching for `서울` finds nothing
- Proper decomposition: `서울` + `에서` + `오` + `았` + `어요`

##### Specialized: Kiwi / Nori-Style Reimplementation
**Kiwi:**

- Probabilistic Korean morphological analyzer
- Decomposition quality similar to Nori
- ⚠️ **LGPL-3.0** (NOT MIT as stated in the original low-level design doc; the project README states "LGPL v3 라이센스로 배포됩니다")
- LGPL-3.0 requires providing object files for relinking if statically linked, plus anti-tivoization obligations inherited from GPL-3.0 — impractical for a self-contained `.so` module
**Nori-style C++ reimplementation (recommended):**

- Same approach as Japanese — reimplement Viterbi in C++ using `mecab-ko-dic` dictionary data
- `mecab-ko-dic` is Apache 2.0 licensed
- Lucene's Nori proves this approach works at scale
- Supports decompound modes: `none`, `discard` (only parts), `mixed` (compound + parts)

##### Note License Correction: Kiwi Is LGPL-3.0, Not MIT
The [low-level design document](https://quip-amazon.com/03HdAmpInFuD/Multi-language-support-plan-low-level-details#temp:s:temp:C:EME17d33eed43a940e7a4187f14d;temp:C:EME91ab8a4b51bf4d4e89ab0e700) states Kiwi's license as MIT. The canonical source ([bab2min/Kiwi on GitHub](https://github.com/bab2min/Kiwi)) states it is **LGPL-3.0** ("LGPL v3 라이센스로 배포됩니다"). This means:

- Static linking requires providing object files so users can re-link against a modified Kiwi — impractical for a self-contained module
- LGPL-3.0 additionally inherits GPL-3.0's anti-tivoization clause: "Installation Information" must be provided so users can install modified library versions on the target hardware
- Kiwi has **more restrictive constraints than MeCab** (which offers a BSD option); dynamic linking with `dlopen()` + runtime fallback is the only viable path
- Option B in the original proposal has TWO problematic dependencies (MeCab operational complexity + Kiwi LGPL-3.0), not one

##### Recommendation
**Top choice: C++ Viterbi reimplementation with mecab-ko-dic data** — provides proper morphological decomposition of agglutinative forms. mecab-ko-dic data is Apache 2.0. Without this, Korean search quality will be significantly impaired.

**Fallback:** ICU — correct for space-delimited Korean but cannot handle agglutination. Many natural Korean queries will return fewer results.

**Not recommended for static linking:** Kiwi — LGPL-3.0 (not MIT as previously believed). Stricter than MeCab due to anti-tivoization obligations. Dynamic linking with `dlopen()` is the only viable integration path.

#### Polish

##### ICU Segmentation (BreakIterator "pl") — No Stemming
Polish uses spaces between words, so ICU tokenization is **correct** — it produces proper word boundaries. However, ICU provides **zero stemming** for Polish.

**The problem without stemming:**
Polish is highly inflected (fusional morphology). A single noun can have 7 case forms × 2 numbers = 14 variants. Without stemming:

- Searching `kot` (cat, nominative) will NOT find `kota` (cat, genitive) or `kotów` (cats, genitive)
- Recall is severely impacted for a language where nearly every noun/verb/adjective inflects

##### Specialized: Morfeusz (BSD) vs Stempel (Apache 2.0)
**Morfeusz:**

- Full morphological dictionary + grammar rules — gold-standard Polish analysis
- BSD-2-Clause license — static linking permitted
- Higher integration complexity (dictionary management, memory footprint)
- Best suited if Polish search quality is a top priority
**Stempel** (from Lucene):

- Algorithmic stemmer using **pre-trained stemming tables** generated from an extensive Polish corpus
- Based on the Egothor stemmer engine
- NOT hand-written suffix rules (like Snowball) — the tables are trained on real data, handling fusional morphology patterns that confuse algorithmic approaches
- Apache 2.0 (tables) + Egothor License (engine) — both permit commercial use and static linking
- Used by OpenSearch's Polish analyzer plugin ([OpenSearch Polish Analyzer](https://docs.opensearch.org/latest/analyzers/language-analyzers/index/))
- Currently implemented in Java — requires a C++ port
- Significantly simpler than Morfeusz while providing good quality for IR purposes
([Lucene Stempel Documentation](https://lucene.apache.org/core/8_6_0/analyzers-stempel/index.html))

**Key distinction** from the Stempel docs: "For an IR system stems are usually sufficient, for a morphological analysis system obviously lemmas are a must." Stempel produces stems (sufficient for search), not lemmas (required for NLP). This matches valkey-search's needs.

##### Recommendation
**Top choice: Morfeusz** — BSD-2-Clause, statically linkable, gold-standard morphological analysis quality. Provides full morphological dictionary with grammar rules, yielding the highest accuracy for Polish stemming and lemmatization.

**Alternative: Stempel (C++ port)** — Apache 2.0 licensed, statically linkable, trained on Polish corpus, handles fusional morphology well for IR purposes. Good quality middle ground between "no stemming" (ICU) and "full morphological analysis" (Morfeusz). Lower integration complexity than Morfeusz.

**Fallback: ICU** — correct tokenization but no stemming. Acceptable if Polish is a lower-priority language and the limitation is documented.

#### Vietnamese

##### ICU Segmentation (BreakIterator "vi") — Broken for Multi-Syllable Words
Vietnamese presents a unique challenge: it uses **spaces between syllables** of multi-syllable words. The word "xe đạp" (bicycle) is written with a space between its two syllables. ICU's `BreakIterator` treats each space-separated token as a word — producing **incorrect** syllable-level tokens.

**Example:** `xe đạp` (bicycle) vs `xe máy` (motorcycle)

- ICU produces: `xe` + `đạp` and `xe` + `máy`
- Correct: `xe đạp` (one token) and `xe máy` (one token)
- Impact: searching `xe` returns BOTH bicycles AND motorcycles indiscriminately — semantically wrong
This is not a minor quality gap — **ICU is actively producing incorrect results** for Vietnamese. Any multi-syllable Vietnamese word (which is a large percentage of the vocabulary) will be broken apart. This is confirmed by ICU's source code: dictionary-based break engines only exist for Thai, Lao, Myanmar, Khmer, and CJK scripts (`brkeng.cpp`), and LSTM models are gated to Khmer, Lao, Myanmar, and Thai only (`lstmbe.cpp`). 

Since Vietnamese uses the Latin script, ICU applies standard UAX #29 word break rules which treat spaces as word boundaries — there is no hook to invoke a dictionary engine for Latin-script text based on locale. The Universal Dependencies project identifies Vietnamese as "the prototypical example" of a language where spaces mark syllable boundaries, not word boundaries ([UD v2 Segmentation](https://universaldependencies.org/v2/segmentation.html)).

##### Specialized: CocCoc (LGPL-3.0 — Problematic) vs RDRsegmenter (MIT)
**CocCoc Tokenizer:**

- High-performance Vietnamese tokenizer (dictionary + DAG-based approach)
- Used in production by the CocCoc search engine
- ⚠️ **LGPL-3.0** — static linking requires providing object files for relinking plus anti-tivoization obligations. While technically less restrictive than GPL, the relinking requirements and anti-tivoization clause make it impractical for a self-contained `.so` module distributed under BSD-3-Clause.
- **Not viable for valkey-search** without accepting LGPL-3.0 deployment obligations
**RDRsegmenter:**

- Rule-based Vietnamese word segmenter using Ripple Down Rules methodology
- 97% accuracy on Vietnamese Electronic Textbooks corpus ([RDRsegmenter paper](https://github.com/datquocnguyen/RDRsegmenter))
- **MIT license** (Java implementation) — permits C++ port and static linking
- Significantly simpler than CocCoc — rule-based rather than dictionary + DAG
- Reference implementation is Java; C++ port required
**Other options:**

- Both OpenSearch and Elasticsearch have **no built-in Vietnamese analyzer**
- Community solutions typically use ICU (broken) or CocCoc-based custom plugins

##### Recommendation
**Top choice: RDRsegmenter (C++ port)** — MIT licensed, permissive for redistribution and static linking, 97% accuracy, handles multi-syllable compounds correctly. Requires porting from Java to C++.

**Fallback: ICU** — ⚠️ produces **incorrect** syllable-level tokens. Only acceptable as a documented placeholder with explicit warning that Vietnamese search quality is severely limited. Users would need to be advised that multi-syllable Vietnamese words will not be correctly segmented.

**Not viable: CocCoc** — LGPL-3.0, incompatible with static linking and proprietary distribution.

### Are Snowball Languages Better Served by Specialized Libraries?

#### Light Stemmers (French, German, Italian, Portuguese, Spanish)
Elasticsearch/OpenSearch defaults to **light stemmers** (not Snowball) for French, German, Italian, Portuguese, and Spanish. Light stemmers apply fewer grammatical rules using simple algorithms — primarily plural removal and common suffix stripping — resulting in:

- More unique terms in the index
- Fewer false conflations (unrelated words being stemmed to the same root)
- Lower recall but higher precision
([Trifork: Analysing European Languages with Lucene](https://trifork.nl/blog/analysing-european-languages-with-lucene/), [Elasticsearch Language Analyzers](https://www.elastic.co/guide/en/elasticsearch/reference/current/analysis-lang-analyzer.html))

**Example (English):** Snowball stems "international" → "intern" (stripping the "ational" suffix). A light stemmer would NOT make this reduction.

**Example (French):** The `FrenchLightStemFilter` is 203 lines of code vs Snowball's `FrenchStemmer` at 608 lines — it removes plurals and some suffixes but is far less aggressive.

Light stemmers are available as Apache 2.0 code (100-200 lines per language) and could be added as an alternative `ILanguageProcessor` subclass in a future release.

#### Hunspell (Dictionary-Based)
Hunspell is a dictionary-based stemming system used by OpenOffice and Firefox. It applies grammatical rules from a rules file and checks stems against a dictionary. Available for hundreds of languages.

**Advantages:** Handles complex morphology; continues stemming until all valid stems are found; supports multi-suffix/prefix removal.

**Disadvantages:** Requires dictionary file management (~5-50MB per language); quality varies enormously by language; slower than algorithmic stemmers; larger memory footprint; only stems words found in its dictionary.

#### Conclusion: Snowball Is Sufficient for 1.4
For the 12 Snowball languages (English, French, German, Spanish, Italian, Portuguese, Russian, Swedish, Turkish, Dutch, Indonesian, Arabic):

1. **Snowball produces algorithmically identical stems** to Redis Search — migration compatibility is preserved
2. **Quality is proven** — decades of production use
3. **No licensing concerns** — BSD-licensed libstemmer C
4. **Where ES/OS differs** (light stemmers for FR, DE, ES, IT, PT), the difference is a precision/recall trade-off, not a correctness issue
Light stemmers are a viable follow-on enhancement if customer feedback indicates over-stemming issues. Hunspell is not recommended (complexity/benefit ratio is poor for the languages in scope).

### Licensing and Static Linking Summary

#### Libraries That Can Be Statically Linked

| Library | Language | License | Notes |
| --- | --- | --- | --- |
| **libstemmer C** (Snowball) | All 12 Snowball langs | BSD | Already integrated |
| **ICU** | All languages | Unicode/ICU License | Already integrated |
| **cppjieba** | Chinese | MIT | Header-only; ~5MB dictionary data |
| **Morfeusz** | Polish | BSD-2-Clause | Full morphological analyzer |
| **Stempel** (tables) | Polish | Apache 2.0 + Egothor License | Requires C++ port from Java |
| **IPAdic** (dictionary data) | Japanese | Apache 2.0 | Used with Viterbi reimplementation |
| **mecab-ko-dic** (dictionary data) | Korean | Apache 2.0 | Used with Viterbi reimplementation |
| **RDRsegmenter** | Vietnamese | MIT | Requires C++ port from Java |

#### Libraries Requiring Dynamic Linking (LGPL)

| Library | Language | License | Deployment Impact |
| --- | --- | --- | --- |
| **MeCab** | Japanese | GPL-2.0 OR LGPL-2.1 OR BSD-3-Clause (tri-licensed) | BSD option permits static linking, but practical issues remain: 30–100MB dictionary data, runtime configuration files, dictionary download mechanism. The Viterbi reimplementation approach (using only IPAdic data, Apache 2.0) avoids these operational concerns entirely. |
| **Kiwi** | Korean | LGPL-3.0 | Requires providing object files for relinking if statically linked; dynamic linking via `dlopen()` with fallback is only practical path |
| **CocCoc** | Vietnamese | LGPL-3.0 | Same LGPL-3.0 constraints as Kiwi; relinking obligations incompatible with self-contained BSD-3-Clause module |

### Conclusion and Recommendation
We have addressed the goals of this document to compare ICU & specialized libraries; research alternate approaches for supporting CJKPV and Snowball languages with high performance; and assessing the licenses of specialized libraries for compatibility with static linking and distribution through valkey-search. Here are the summarized tradeoffs for the all languages to be supported:

#### Trade-off Summary Table

| Language | Library | Quality | Engineering Effort | Limitations |
| --- | --- | --- | --- | --- |
| **English** | Snowball (libstemmer C) | High | None (already integrated) | None — ecosystem standard |
| **French** | Snowball | High | None (already integrated) | More aggressive than ES/OS default (light stemmer); may over-stem |
| Light stemmer (port from Lucene `FrenchLightStemFilter`) | High (higher precision) | ~1 day (200 lines of C) | Lower recall than Snowball; only removes plurals + common suffixes |  |
| **German** | Snowball | High | None (already integrated) | More aggressive than ES/OS default; no ä→a / ö→o / ü→u normalization |
| Light stemmer + `GermanNormalizationFilter` | High (higher precision) | ~2 days | Lower recall; requires separate normalization pass |  |
| **Spanish** | Snowball | High | None (already integrated) | Slightly more aggressive than ES/OS default |
| Light stemmer (port from Lucene `SpanishLightStemFilter`) | High (higher precision) | ~1 day | Lower recall |  |
| **Italian** | Snowball | High | None (already integrated) | More aggressive than ES/OS default |
| Light stemmer (port from Lucene `ItalianLightStemFilter`) | High (higher precision) | ~1 day | Lower recall |  |
| **Portuguese** | Snowball | High | None (already integrated) | More aggressive than ES/OS default |
| Light stemmer (port from Lucene `PortugueseLightStemFilter`) | High (higher precision) | ~1 day | Lower recall |  |
| **Russian** | Snowball | High | None (already integrated) | None — same as ES/OS default |
| **Swedish** | Snowball | High | None (already integrated) | None — same as ES/OS default |
| **Turkish** | Snowball | High | None (already integrated) | None — requires locale-aware case folding (İ/i, I/ı) which ICU handles |
| **Dutch** | Snowball | High | None (already integrated) | None — same as ES/OS default |
| **Indonesian** | Snowball | High | None (already integrated) | None — same as ES/OS default |
| **Arabic** | Snowball + NFKC | High | None (already integrated) | Extended punctuation set (،؟؛) must be configured; NFKC handles presentation forms |
| **Chinese** | ICU (`BreakIterator` "zh") | Medium | None (already integrated) | No custom dictionary; inconsistent compound segmentation; differs from Redis Search (Friso) |
| cppjieba (MIT, header-only) | High | ~3 days | Adds ~5MB dictionary data; one more dependency to maintain |  |
| **Japanese** | ICU (`BreakIterator` "ja") | Low-Medium | None (already integrated) | No compound decomposition; no POS filtering; no base form; inconsistent splits |
| MeCab (tri-licensed: GPL-2.0/LGPL-2.1/BSD-3-Clause) | High | ~2.5 weeks | BSD option permits static linking, but 30–100MB dictionary deployment complexity remains; `dlopen()` + fallback still needed for practical distribution |  |
| Viterbi reimplementation + IPAdic (Apache 2.0) | High | ~3-4 weeks | Higher engineering effort; requires FST compilation of dictionary; ongoing dictionary maintenance |  |
| **Korean** | ICU (`BreakIterator` "ko") | Low | None (already integrated) | Cannot decompose agglutinative forms; searching root words fails to match inflected forms |
| Kiwi (LGPL-3.0, dynamic link) | High | ~1 week | ⚠️ LGPL-3.0 — static linking requires providing object files for relinking + anti-tivoization; NOT MIT as originally stated |  |
| Viterbi reimplementation + mecab-ko-dic (Apache 2.0) | High | ~3-4 weeks | Same engineering trade-offs as Japanese reimplementation |  |
| **Polish** | ICU (`BreakIterator` "pl") | Low (tokenization only) | None (already integrated) | Correct tokenization but zero stemming; severely reduced recall for a highly inflected language |
| Stempel (Apache 2.0 + Egothor, C++ port) | Good | ~2-3 weeks | Requires porting from Java; trained tables handle fusional morphology but not gold-standard |  |
| Morfeusz (BSD-2-Clause) | High | ~2 weeks | Full morphological dictionary; higher memory footprint; integration complexity |  |
| **Vietnamese** | ICU (`BreakIterator` "vi") | **Broken** | None (already integrated) | ⚠️ Splits multi-syllable words incorrectly (syllable-level tokens); search is effectively non-functional for compound words |
| CocCoc (LGPL-3.0) | High | ~1 week | ⚠️ LGPL-3.0 — relinking + anti-tivoization obligations incompatible with self-contained BSD-3-Clause module |  |
| RDRsegmenter (MIT, C++ port) | Good (97% accuracy) | ~2-3 weeks | Requires porting from Java; rule-based (simpler than CocCoc but effective) |  |

#### **Recommended path for 1.4:**
Given engineering constraints, a **phased approach** is practical:

1. **Phase 1 (P0 for release):**
  1. Support for Snowball languages (English, French, German, Spanish, Italian, Portuguese, Russian, Swedish, Turkish, Dutch, Indonesian, Arabic).
2. **Phase 2 (P1, can introduce in 2.0):** CJKPV languages
  1. `ChineseProcessor` class using cppjieba
  2. `JapaneseProcessor` class using Viterbi/IPAdic implementation based on Kuromoji OR using MeCab
  3. `KoreanProcessor` class using Viterbi implementation + mecab-ko-dic
  4. `PolishProcessor` class using Morfeusz
  5. `VietnameseProcessor` class using RDRsegmenter
3. **Phase 3 (P2, optimizations for 2.0 or later):**
  1. Add light stemmer support for French, German, Spanish, Italian, Portuguese