# PR #1263 — Fork-only changes to revert before raising upstream

Context: audit of the `multi-language-support` branch diff against `valkey-io/valkey-search:main` (PR [#1263](https://github.com/valkey-io/valkey-search/pull/1263)). Everything below only exists on the fork branch and adds no value to the upstream PR.

## Action items

### 1. Delete `.coderabbit.yaml`

Fork-only CodeRabbit AI review config. Not present on upstream `main`. Whether upstream enables CodeRabbit is a maintainer decision, and the `multi-language-support` branch listed in `base_branches` will not exist after merge.

```bash
git rm .coderabbit.yaml
```

Full current content (for reference):

```yaml
reviews:
  auto_review:
    enabled: true
    base_branches:
      - "main"
      - "multi-language-support"
```

### 2. Remove `- multi-language-support` from CI workflow triggers

Seven workflow files each add the fork branch to their `pull_request` and/or `push` trigger `branches:` lists. After merge the branch won't exist, so these lines are dead noise upstream. Everything else in these files (the `[0-9].[0-9]` regex, `paths-ignore`, removal of the "Verify Branch" step, and the thread-safety `clang-tidy` invocation) is already on upstream `main` from PRs #1130 and #1082 — leave that alone.

Files to edit and lines to remove:

| File | Occurrences | Under |
|---|---|---|
| `.github/workflows/integration_tests.yml` | 2 | `pull_request.branches`, `push.branches` |
| `.github/workflows/integration_tests-asan.yml` | 2 | `pull_request.branches`, `push.branches` |
| `.github/workflows/unittests.yml` | 2 | `pull_request.branches`, `push.branches` |
| `.github/workflows/unittests-asan.yml` | 2 | `pull_request.branches`, `push.branches` |
| `.github/workflows/unittests-tsan.yml` | 2 | `pull_request.branches`, `push.branches` |
| `.github/workflows/macos.yml` | 2 | `pull_request.branches`, `push.branches` |
| `.github/workflows/clang_tidy_format.yml` | 1 | `push.branches` (alongside `main`, `fulltext`) |

Post-edit, each of the first six should trigger only on:

```yaml
branches:
  - main
  - "[0-9].[0-9]"
```

and `clang_tidy_format.yml` on:

```yaml
branches:
  - main
  - fulltext
```

Quick one-liner to strip them all:

```bash
sed -i '/^ *- multi-language-support *$/d' \
  .github/workflows/integration_tests.yml \
  .github/workflows/integration_tests-asan.yml \
  .github/workflows/unittests.yml \
  .github/workflows/unittests-asan.yml \
  .github/workflows/unittests-tsan.yml \
  .github/workflows/macos.yml \
  .github/workflows/clang_tidy_format.yml
```

Verify with:

```bash
git -P diff .github/workflows/
grep -R "multi-language-support" .github/ || echo "clean"
```

## Not dev artifacts — leave alone

These show up in the diff only because the branch's merge base is behind upstream `main`. A fresh rebase would drop them from the review view; if you don't rebase they'll still be fine to ship because they match what's already on `main`:

- `.clang-tidy` thread-safety changes (upstream #1082)
- `cmake/Modules/valkey_search.cmake` `-Wthread-safety` (upstream #1082)
- `.github/workflows/slash-commands.yml` `/duplicates` handler (+414 lines, already upstream)
- `.skills/README.md`, `.skills/stale-prs.md` (upstream #1038)
- Deletion of `.github/release-branches.txt` (upstream #1130)
- Removal of "Verify Branch" step from the six CI workflows (upstream #1130)

## Legitimate feature changes — leave alone

- `.config/typos.toml` — new exclusions (`stop_words.h`, `language_processor_test.cc`, `snowball_stem_test.cc`, `data_sets.py`) and words (`continuent`, `caf`, `mak`, `café`, `eit_*`)
- `integration/module/1.2.0-libsearch.so.zip` — RDB cross-version compat fixture
- `integration/run.sh` (+1) — exports `ROOT_DIR` for compat regen flow

## Source code sanity

No dev-only markers in any added source line. Scanned for: `std::cout`, `std::cerr`, `LOG(INFO)`, hardcoded `/tmp/` / `/home/` / `/Users/` paths, `FIXME`, `XXX`, `assert(false)`, `Debug print`, author-named `TODO(...)`. All clean.

## Optional but recommended

Rebase on latest upstream `main` before opening the PR to the community. That trims ~15 "already-on-main" files out of the review view, reducing the diff from 194 files to ~180 truly feature-related files.

```bash
git fetch upstream
git rebase upstream/main   # or: git merge upstream/main
```

## Heads-up: fork-only file NOT currently in the PR

`.github/workflows/rebase-multi-language-support.yml` exists on `neerajr0/valkey-search:multi-language-support` (commit `132cef4`, "ci: Add workflow to rebase multi-language-support from upstream main daily at 8am PDT") but is **not** on the PR head `VoletiRam/valkey-search:multi-language-support` (commit `1e8b2228`). Nothing to do today — PR #1263 does not contain this file.

Only relevant if you do any branch consolidation before raising (rebasing / cherry-picking / switching the PR head to point at `neerajr0/multi-language-support`). If any of that happens, this file must be dropped as well since it exists purely to automate rebases of the fork branch and has no place upstream.

## Verification the reverts list is complete

Full `git diff --name-status upstream/main..1e8b2228` (PR head) filtered to non-src/non-test files yields exactly these entries. Everything is either a legit feature change, already on upstream `main` (drops on rebase), or in the reverts list above:

| File | Category |
|---|---|
| `.clang-tidy` (M) | already on upstream main (#1082) |
| **`.coderabbit.yaml` (A)** | **fork-only — reverts list** |
| `.config/typos.toml` (M) | legit feature (new exclusions + words) |
| `.github/release-branches.txt` (D) | already deleted on upstream main (#1130) |
| **`.github/workflows/clang_tidy_format.yml` (M)** | **fork-only line — reverts list** |
| **`.github/workflows/integration_tests-asan.yml` (M)** | **fork-only line — reverts list** |
| **`.github/workflows/integration_tests.yml` (M)** | **fork-only line — reverts list** |
| **`.github/workflows/macos.yml` (M)** | **fork-only line — reverts list** |
| `.github/workflows/slash-commands.yml` (M) | already on upstream main |
| **`.github/workflows/unittests-asan.yml` (M)** | **fork-only line — reverts list** |
| **`.github/workflows/unittests-tsan.yml` (M)** | **fork-only line — reverts list** |
| **`.github/workflows/unittests.yml` (M)** | **fork-only line — reverts list** |
| `.skills/README.md` (A) | already on upstream main (#1038) |
| `.skills/stale-prs.md` (A) | already on upstream main (#1038) |
| `BACKPORTING.md` (A) | already on upstream main (#1055) |
| `COMPATIBILITY.md` (M) | already on upstream main (#1063) |
| `CONTRIBUTING.md` (M) | already on upstream main |
| `GOVERNANCE.md` (A) | already on upstream main (#1153) |
| `MAINTAINERS.md` (A) | already on upstream main (#1153) |
| `THIRD_PARTY_NOTICES` (M) | legit feature (CLDR + Lucene notices) |

Grepped every `+` line in the full PR diff for the usual dev smells — `coderabbit`, `multi-language-support`, internal Amazon URLs (`@amazon.com`, `quip-amazon`, `corp.amazon`), dev-desktop refs (`dev-dsk-`, `/home/*/`), author usernames (`neerajr0`, `VoletiRam`, `linbran5123`). Only the two items above surfaced. Source code is clean.

## Verification checklist before raising

- [ ] `.coderabbit.yaml` deleted (or `git revert e9927b258b3bbb14ca03571f1f35dc69552a94d6`)
- [ ] `grep -R "multi-language-support" .github/` returns nothing
- [ ] `git -P diff upstream/main...HEAD -- .github/ .coderabbit.yaml` shows only "already-on-main" workflow changes (branch regex, paths-ignore, Verify Branch removal) and no `.coderabbit.yaml`
- [ ] If any branch consolidation happened: `git -P ls-files .github/workflows/rebase-multi-language-support.yml` returns nothing
- [ ] Branch rebased on upstream `main` (optional but recommended — drops ~15 "already-on-main" files from the review view)
- [ ] Package builds green: `brazil-build release` (or local `./build.sh`)
- [ ] Unit + integration tests pass locally
