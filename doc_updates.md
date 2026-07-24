# Document Updates for Multi-Language Support - CJKPV Languages

## Section 1: Japanese Recommendation (replace the existing Recommendation subsection under Japanese)

##### Recommendation

**Top choice: MeCab C library (BSD-3-Clause)** — MeCab is tri-licensed (GPL-2.0 OR LGPL-2.1 OR BSD-3-Clause). Under the BSD option, static linking is permitted with no copyleft obligations. Provides full morphological analysis, POS tagging, and base form extraction out of the box. Battle-tested in production across many search systems with proven quality. Engineering effort is ~1-2 weeks integration. The practical trade-offs are dictionary management (IPAdic is 30-100MB) and runtime configuration, but these are solvable deployment concerns rather than legal blockers. Lower development time and proven production quality make this the recommended path.

**Alternative: C++ Viterbi reimplementation with IPAdic dictionary data** — provides Kuromoji-equivalent quality (compound decomposition, base form extraction) with zero external dependency at runtime. IPAdic data is Apache 2.0. Higher engineering effort (~3-4 weeks) but the algorithm is well-documented and Lucene's implementation serves as a reference. Better suited for a future phase if eliminating all external library dependencies becomes a priority.

**Fallback**: ICU — significant quality gap (no compound decomposition, no POS, no base form) but zero additional effort. Document the limitation explicitly.

---

## Section 2: Trade-off Summary Table (replace Japanese rows 21-22)

||Language|Library|Quality|Engineering Effort|Limitations|
|---|---|---|---|---|---|
|21|**Japanese**|**MeCab (BSD-3-Clause static link) — Recommended**|High|~1-2 weeks|BSD option permits static linking with no copyleft; dictionary data (30-100MB) must be compiled/bundled at build time; proven production quality|
|22| |Viterbi reimplementation + IPAdic (Apache 2.0)|High|~3-4 weeks|Higher engineering effort; requires FST compilation of dictionary; ongoing dictionary maintenance; consider for future phase if removing external library dependencies becomes a goal|

---

## Section 3: Recommended Path for 1.4 — Phase 2 bullet (replace the JapaneseProcessor line)

* `JapaneseProcessor` class using MeCab (BSD-3-Clause static link) with IPAdic dictionary

---

## Section 4: New Section — Add after "Recommended path for 1.4" in the Conclusion

### LGPL Licensing: Path Forward Assessment

Two high-quality libraries were excluded from the recommended path due to LGPL-3.0 licensing constraints: **Kiwi** (Korean morphological analyzer) and **CocCoc** (Vietnamese tokenizer). Both are production-grade and would reduce engineering effort compared to the recommended alternatives. This section outlines what would need to be determined to use them, and what changes if a path forward exists.

#### What Needs to Be Determined

The core question is whether valkey-search (distributed as a self-contained `libsearch.so` under BSD-3-Clause) can comply with LGPL-3.0 obligations when statically linking these libraries. Dynamic linking is not an option — the team has confirmed that valkey-search will remain a fully self-contained statically-linked `.so` module. Specifically, we need to determine:

1. **Determine if static linking is feasible**: LGPL-3.0 Section 4(d)(0) requires that, if statically linking, the distributor must provide the object files (or source) for the non-LGPL portions of the combined work, so that a user can re-link against a modified version of the LGPL library. Determine whether the valkey-search build and release process can accommodate distributing object files alongside the `.so`.

2. **Anti-tivoization (from GPL-3.0 Section 6)**: LGPL-3.0 inherits the requirement to provide "Installation Information" — the keys, authorization codes, or other information needed to install modified versions of the library on the target hardware. Determine whether this applies to valkey-search's deployment targets (cloud instances, containers, user-managed servers) and whether compliance is feasible.

3. **License compatibility with the Valkey project**: Valkey and valkey-search are BSD-3-Clause. Confirm with the Valkey project maintainers and any relevant open-source legal counsel whether LGPL-3.0 dependencies are acceptable within the project's licensing policy and community norms.

#### Recommendations That Would Change

If LGPL-3.0 static linking obligations can be satisfied:

| Language | Current Recommendation | Would Change To | Effort Savings |
|---|---|---|---|
| **Korean** | C++ Viterbi reimplementation + mecab-ko-dic (~3-4 weeks) | Kiwi (~1 week integration) | ~2-3 weeks |
| **Vietnamese** | RDRsegmenter C++ port (~2-3 weeks, 97% accuracy) | CocCoc (~1 week integration, production-grade accuracy) | ~1-2 weeks |

**Korean — Kiwi (LGPL-3.0):** Kiwi is a probabilistic morphological analyzer that provides decomposition quality comparable to Lucene's Nori. It would replace the need for a from-scratch Viterbi reimplementation, saving approximately 2-3 weeks of engineering effort.

**Vietnamese — CocCoc (LGPL-3.0):** CocCoc is a high-performance dictionary + DAG-based tokenizer used in production by the CocCoc search engine. It would replace the need to port RDRsegmenter from Java to C++, saving approximately 1-2 weeks and likely providing higher segmentation accuracy than RDRsegmenter's rule-based approach.

**Total potential savings:** ~3-5 weeks of engineering effort across Korean and Vietnamese support, with potentially higher quality results for both languages.

#### Next Steps

1. Consult with open-source legal counsel on LGPL-3.0 static linking feasibility for the valkey-search distribution model
2. Engage Valkey project maintainers on LGPL dependency policy
3. Make a go/no-go decision before Phase 2 engineering begins to avoid wasted reimplementation work
