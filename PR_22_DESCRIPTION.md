# Prep multi-language-support for upstream merge into valkey-io/main

This PR brings `multi-language-support` up to date with the latest `valkey-io/valkey-search:main` and strips the fork-only CI plumbing that shouldn't ship upstream. The three commits are independently reviewable and land in the following order:

1. `1d1f601` — **chore: revert dev-specific workflow changes for multi-language support**
2. `fead744` — **Merge remote-tracking branch `upstream/main` into `multi-language-support-merge-main`**
3. `aff013f` — **fix post merging main: regenerate pickle files and update new test for updated return type of AddRecord**

After this lands, the branch's diff against `valkey-io/main` is exclusively multi-language content plus the third-party licensing notes the feature requires. The final upstream PR ([valkey-io/valkey-search#1263](https://github.com/valkey-io/valkey-search/pull/1263)) can then be raised without carrying stale dev-only files or a divergent view of the codebase.

---

## Commit 1 — `1d1f601` chore: revert dev-specific workflow changes for multi-language support

Two categories of fork-only CI hooks were introduced early in the branch's life and would be dead noise upstream. Both are removed here.

### `.coderabbit.yaml` (deleted, 6 lines)

Fork-only CodeRabbit auto-review config. Whether upstream enables CodeRabbit is a maintainer decision; the `multi-language-support` branch listed in `base_branches` won't exist after the upstream merge anyway. Full previous content, for reference:

```yaml
reviews:
  auto_review:
    enabled: true
    base_branches:
      - "main"
      - "multi-language-support"
```

### `- multi-language-support` trigger lines in seven CI workflows (-13 lines total)

Each of the seven workflows added the fork branch to its `pull_request` and/or `push` trigger `branches:` list. That branch won't exist after the upstream merge, so the lines become dead configuration on `valkey-io`. Everything else in these files (the `[0-9].[0-9]` branch regex, `paths-ignore`, the removal of the "Verify Branch" step, and the thread-safety `clang-tidy` invocation) is already on upstream `main` from PRs [#1130](https://github.com/valkey-io/valkey-search/pull/1130) and [#1082](https://github.com/valkey-io/valkey-search/pull/1082); those don't move.

Files touched (all deletions):

| File | Removed under |
|---|---|
| `.github/workflows/clang_tidy_format.yml` | `push.branches` |
| `.github/workflows/integration_tests-asan.yml` | `pull_request.branches`, `push.branches` |
| `.github/workflows/integration_tests.yml` | `pull_request.branches`, `push.branches` |
| `.github/workflows/macos.yml` | `pull_request.branches`, `push.branches` |
| `.github/workflows/unittests-asan.yml` | `pull_request.branches`, `push.branches` |
| `.github/workflows/unittests-tsan.yml` | `pull_request.branches`, `push.branches` |
| `.github/workflows/unittests.yml` | `pull_request.branches`, `push.branches` |

Verified post-commit: `grep -R "multi-language-support" .github/` returns nothing, and each workflow's blob hash is byte-identical to the corresponding file on `upstream/main`.

---

## Commit 2 — `fead744` Merge upstream/main into multi-language-support-merge-main

Merge base was 43 commits behind upstream. The merge produced 17 conflicted files, plus a large number of auto-merged files that carry upstream refactors we adopt wholesale (`RecordResult`, `NumericBTree`, HNSW load validation, `search.emulate-release`).

Resolution key:
- **T** — took upstream's blob (`git checkout --theirs`)
- **O** — took our blob (`git checkout --ours`)
- **M** — manually combined both sides

| # | File | Resolution | Rationale |
|---|---|---|---|
| 1 | `.config/typos.toml` | **M** | Kept HEAD's `'caf\\xC3\\xA9'` regex (needed for the raw-bytes `test_non_utf8_english_chars` fixture). All other multi-lang additions (`continuent`, `caf`, `mak`, `café`, new excludes) already on HEAD. |
| 2 | `integration/compatibility/aggregate-answers.pickle.gz` | **T** (then regenerated in commit 3) | Binary artifact; conflict is unresolvable at pickle level. See commit 3 rationale. |
| 3 | `integration/compatibility/text-search-answers.pickle.gz` | **T** (then regenerated in commit 3) | Same as above. |
| 4 | `integration/compatibility_test.py` | **M** | Adopted upstream's base-class switch (`ValkeySearchTestCaseBase` → `ValkeySearchTestCaseDebugMode`) and kept HEAD's `BASE_ANSWER_FILES` split for multi-lang. Also imported both `*DebugMode` classes and added `search.emulate-release=1.3.0` inside `TestAnswersCMD` to activate the whole-key-drop behavior from [#1155](https://github.com/valkey-io/valkey-search/pull/1155) that the regenerated pickles capture. |
| 5 | `integration/utils.py` | **O** | Kept HEAD's `start_server_with_old_module` / `cleanup_module_binary` / `skip_if_san_build` helpers — required by `test_rdb_cross_version_compat.py`. Upstream has no equivalent. |
| 6 | `src/index_schema.h` | **T** | Upstream tightened `ProcessAttributeMutation` to return `bool` (whether the mutation drops the whole key, matching Redisearch's invalid-data semantics) and added a friend test. Multi-lang doesn't override this method. |
| 7 | `src/indexes/numeric.cc` | **T** | Upstream [#1031](https://github.com/valkey-io/valkey-search/pull/1031) replaced `BTreeNumeric<T>` + `SegmentTree` with a single order-statistic B+-tree (`utils::NumericBTree`). Self-contained refactor, no multi-lang interaction. |
| 8 | `src/indexes/numeric.h` | **T** | Same PR #1031 refactor. |
| 9 | `src/indexes/tag.cc` | **T** | Upstream [#1031](https://github.com/valkey-io/valkey-search/pull/1031) reimplemented Tag on top of rax + `BagOfInternedStringPtrs`. HEAD's ASCII-only `Normalize`/`IndexTagForKey`/`DeindexTagForKey` are superseded. No multi-lang coupling (tags are exact-match, not tokenized). |
| 10 | `src/indexes/tag.h` | **T** | `AddRecord`/`ModifyRecord` return type switched from `absl::StatusOr<bool>` to `absl::StatusOr<RecordResult>` — part of a codebase-wide refactor. |
| 11 | `src/indexes/vector_base.cc` | **T** | Single-line `return true;` → `return RecordResult::kAdded;`, same refactor. |
| 12 | `src/indexes/vector_hnsw.cc` | **T** | Upstream added authoritative `m` and validation-flag parameters to `algo_->LoadIndex()` for the HNSW corruption-hardening work. |
| 13 | `src/utils/string_interning.h` | **T** | One-liner: `#include <utility>` for `std::move` (upstream `BorrowedInternedStringPtr` refactor). |
| 14 | `src/version.h` | **M** | Kept HEAD's `kModuleVersion = 1.4.0` and `kRelease14` constant (multi-lang is 1.4). Accepted upstream's `MODULE_RELEASE_STAGE = "dev"`, which arrived via [`b8663b3`](https://github.com/valkey-io/valkey-search/commit/b8663b3) "Open next development cycle on main" — the post-1.2.1 dev-cycle bump. Our pre-merge value `"rc2"` was a stale carryover from the Feb 2026 upstream sync that never got refreshed. |
| 15 | `testing/CMakeLists.txt` | **M** | Combined the two refactor directions: the *utils* test suite now lists both upstream's `numeric_btree_test.cc` (from #1031) and our `scanner_test.cc` in `UTILS_TEST_SOURCES`, and links both `numeric_btree` and `scanner` targets; the *indexes* test suite adds our `language_processor_test.cc`, `snowball_stem_test.cc`, `unicode_normalizer_test.cc` and drops the deleted `lexer_test.cc`. |
| 16 | `testing/vector_test.cc` | **T** | Adopted upstream's ~380-line HNSW corruption-rejection test suite plus signature updates (`VerifyResult` on `RecordResult`, `LoadIndex(..., expected_m, validate)`), the `header.set_mult(1.0/log(kM))` correction, and the added `<cmath>`, `<cstring>`, `<deque>`, `<numeric>` includes. |
| 17 | `third_party/hnswlib/hnswalg.h` | **T** | Adopted upstream's `LoadIndex` rewrite that derives geometry from the authoritative `expected_m` parameter instead of the untrusted header. Verified: the ARM64 alignment fix from [#1079](https://github.com/valkey-io/valkey-search/pull/1079) is preserved at lines 152-153 and 922-923. |

### Notable upstream refactors we now inherit (auto-merged, no conflict)

These weren't in the 17-file conflict list but arrived cleanly and are worth calling out because reviewers will see them in the diff:

- **`RecordResult` enum** (`src/indexes/index_base.h`) — `AddRecord` / `ModifyRecord` return `absl::StatusOr<RecordResult>` (`kAdded` / `kMissing` / `kInvalidData`) across every index type. `RemoveRecord` stays `absl::StatusOr<bool>`.
- **`utils::NumericBTree`** (`src/utils/numeric_btree.h`) — new order-statistic B+-tree used by Numeric. Replaces the old `BTreeNumeric<T>` template + parallel `SegmentTree`. `segment_tree.h`/`.cc` and their test file are deleted.
- **HNSW load-validation hardening** — `HierarchicalNSW::LoadIndex(..., expected_m, validate)` now range-checks header fields and detects corrupt/tampered chunks. `libsearch.so` links the same alignment fix as before.
- **`search.emulate-release` config** (`src/valkey_search_options.{h,cc}`) — the compatibility-emulation knob from upstream [#1063](https://github.com/valkey-io/valkey-search/pull/1063). Multi-lang doesn't gate on this directly, but `compatibility_test.py` uses it (see commit 3).

---

## Commit 3 — `aff013f` fix post merging main: regenerate pickle files and update new test for updated return type of AddRecord

Two independent issues to clean up after the merge landed. Both were only reachable *after* the merge because they're semantic-level fallout from combining changes on both sides — git's textual merge saw no conflict.

### 3a. Regenerate the three compatibility pickle files

**Why regeneration was the only viable resolution.** Both sides regenerated these pickle blobs for different reasons:

- **Upstream side** — the pickles were rewritten across five commits after our last upstream sync on Jul 1:
  - [`3e3825c` (#1155)](https://github.com/valkey-io/valkey-search/pull/1155): whole-key-drop on invalid data (Redisearch parity)
  - [`6f29727` (#1086)](https://github.com/valkey-io/valkey-search/pull/1086): expression-evaluation compat fixes
  - [`79e1b3e` (#893)](https://github.com/valkey-io/valkey-search/pull/893): array value type support
  - [`cf1c9bb` (#1174)](https://github.com/valkey-io/valkey-search/pull/1174): JSON backslash-escape indexing fix
  - [`4b2c085` (#979)](https://github.com/valkey-io/valkey-search/pull/979): tag escaped closing brace
- **Our side** — the pickles were rewritten by the multi-language compatibility-testing work in `9f3e662`, `4c22752`, `1b5d80f`, and `e140c86` to add new coverage and to defer the `snowballstemmer` dependency to pickle-generation time.

Taking `--ours` would lose upstream's five compat fixes; taking `--theirs` would lose our multi-lang additions to `data_sets.py` (which is a Python source file the pickle contents derive from). Neither side alone is correct, and a gzipped Python pickle can't be textually merged.

The compat framework was designed for exactly this case: `integration/compatibility/__init__.py` embeds a `compute_sources_hash()` of every `.py` in the compat directory inside each pickle, so `compatibility_test.py` can detect a stale pickle relative to its generators. `regenerate.sh` reruns the generators against a real Redisearch reference (Docker `redis/redis-stack-server`) and writes fresh pickles whose expected answers reflect the merged code state — capturing both upstream's compat fixes and our multi-language extensions.

Result of running `./integration/compatibility/regenerate.sh`:
- `generate.py`: 46 passed, 6 skipped in 52.5s → `aggregate-answers.pickle.gz` (707 KB)
- `generate_text.py`: 252 passed, 24 skipped in 4:43 → `text-search-answers.pickle.gz` (2.8 MB) and `text-search-multilang-answers.pickle.gz` (4.8 MB)

Skips are the intentionally-excluded scenarios documented in `unsupported_tests.md` and `multilang_failures_report.txt`.

### 3b. `testing/text_test.cc` — one-line `IndexDocument` assertion fix

Symptom: `libsearch.so` builds, and every source file compiles individually, but `testing/text_test.cc` fails to compile with:
```
error: cannot convert 'const valkey_search::indexes::RecordResult' to 'bool' in initialization
```
at line 629, inside the `IndexDocument` helper.

Root cause is a **silent (semantic) merge conflict**:

- Our July 6 commit `2331845` ("Replace Lexer with LanguageProcessor") added the `TextMultiLanguageTest::IndexDocument` helper. At the time, `Text::AddRecord` still returned `absl::StatusOr<bool>` on both sides, so `ASSERT_TRUE(result.value())` was correct.
- Upstream's July 10 [#1155](https://github.com/valkey-io/valkey-search/pull/1155) flipped `Text::AddRecord` from `StatusOr<bool>` to `StatusOr<RecordResult>` in `src/indexes/text.h` and `.cc` — but never touched `text_test.cc`.
- Both edits land on non-overlapping text lines: ours in `text_test.cc`, theirs in `text.h`. Git's line-based merge algorithm sees no conflict and merges both cleanly. The combined code doesn't compile because `.value()` now returns a `RecordResult` enum, and `ASSERT_TRUE` requires a `bool`.

Fix (line 629, inside `IndexDocument`):

```diff
- ASSERT_TRUE(result.value());
+ ASSERT_EQ(result.value(), indexes::RecordResult::kAdded);
```

Matches the existing pattern in `AddRecordAndCommitKey` (line 111 of the same file) that was updated in earlier `RecordResult` transitions. No other callsite in `text_test.cc` needed a change — grepped every `->AddRecord(` and `->ModifyRecord(` in `testing/` and confirmed the remaining raw-pointer callers either don't touch `.value()` or already use `ASSERT_EQ(..., kAdded)`.

### Verification after commit 3

- `./build.sh` compiles `libsearch.so` cleanly (all 269 targets, including the multi-language stemmers for all 12 Snowball languages).
- `nm libsearch.so` confirms the expected multi-language symbols are exported: `LanguageProcessor::Create`, `LanguageProcessor::Builder::{AddSegmenter, SetNormalizer, SetStemmer, SetStopWordFilter, SetQueryTokenizer, Build}`, plus `kModuleVersion` and `kRelease14`.
- Unit test binaries fail to link locally on this dev-desktop with `undefined symbol: OPENSSL_sk_pop_free` — this is an SSL library ABI mismatch on the local `AWS-LC-FIPS` install, unrelated to the merge. Upstream CI runs the tests in Docker with a matched OpenSSL/BoringSSL/grpc stack and will exercise them properly.

---

## Post-merge state

Local diff against `upstream/main` is 44 commits ahead / 0 behind — no divergence remaining apart from the multi-language feature itself. Ready to be rebased or fast-forwarded onto `ramvolet-fork/multi-language-support`, at which point the upstream PR [valkey-io/valkey-search#1263](https://github.com/valkey-io/valkey-search/pull/1263) can be raised.
