# Deterministic Build Hygiene

Document status: Active
Last updated: 2026-02-13
Owner: Core Platform / Quality Engineering

## 1. Objective

Define mandatory controls so the same source revision produces equivalent build and test outcomes across:

- local developer machine,
- Namespace macOS CI,
- GitHub-hosted macOS fallback/canary CI.

This document is strict by design and intended for mission-critical reliability.

## 2. Threat Model for Nondeterminism

| Threat | Typical Cause | Impact | Required Control |
|---|---|---|---|
| Toolchain drift | Unpinned package manager installs | Different compiler/linker behavior | Pin via `flake.lock`; forbid floating CI toolchain versions |
| Action drift | `uses: org/action@vX` mutable tags | CI behavior changes without repo changes | Pin all Actions to commit SHA |
| Timestamp drift | Build IDs, archive mtimes, metadata | Hash mismatch for equivalent outputs | Set stable build timestamp inputs (`SOURCE_DATE_EPOCH`) |
| Absolute path leakage | Debug paths, macros, diagnostics | Output differs by workspace path | Apply path-prefix-map controls and normalize compare artifacts |
| Network fetch drift | Download latest assets during build | Reproducibility and availability failures | Locked artifact manifests with hash verification |
| Environment drift | Locale/TZ/random seeds/CPU feature dispatch | Test flakiness and binary differences | Stable env contract and deterministic test partitioning |
| Host trust variance | Signing/notarization external service | Signature and notarization metadata differ | Treat trust metadata separately from unsigned artifact reproducibility |

## 3. Mandatory Controls

## 3.1 Toolchain and Dependency Pinning

1. Introduce and enforce `flake.lock` in repo root.
2. CI required lanes must use lock-pinned Nix toolchain, not ad hoc latest Homebrew versions.
3. Lockfile updates require dedicated PR review.

## 3.2 GitHub Action Pinning

- Every `uses:` entry in workflow files must pin to a commit SHA.
- Floating references (`@v4`, `@main`, branch refs) are not allowed for required lanes.

## 3.3 Offline/Locked Model Asset Strategy

Current script `/Users/rexliu/betterspotlight/tools/fetch_embedding_models.sh` pulls from network URLs.

Required target state:

1. Produce a versioned model manifest with:
- logical model ID,
- source URL,
- expected digest,
- size floor,
- fetch timestamp and provenance fields.

2. CI required lanes must consume locked, hash-verified assets.
3. Network fetches in required deterministic lanes are prohibited unless pinned digest verification is enforced.
4. CMake option `BETTERSPOTLIGHT_FETCH_MODELS` should default to deterministic behavior in CI (off unless explicitly staged).

## 3.4 Stable Environment Contract

Set and enforce in deterministic lanes:

- `LC_ALL=C`
- `LANG=C`
- `TZ=UTC`
- `SOURCE_DATE_EPOCH=<commit timestamp or release timestamp>`

Where compiler support exists, use path normalization flags for reproducibility:

- Clang/GCC prefix-map options (for example `-ffile-prefix-map` / `-fdebug-prefix-map` style controls)
- apply consistently in deterministic compare builds.

## 3.5 Test Determinism Hardening

Deterministic test suites must not depend on machine-specific absolute paths.

Known path-sensitive tests to fix first:

- `/Users/rexliu/betterspotlight/Tests/Unit/test_cross_encoder_reranker.cpp`
- `/Users/rexliu/betterspotlight/Tests/Unit/test_model_registry.cpp`
- `/Users/rexliu/betterspotlight/Tests/Unit/test_reranker_cascade.cpp`
- `/Users/rexliu/betterspotlight/Tests/Unit/test_qa_extractive_model.cpp`

Required policy:

1. Deterministic unit/integration lanes use repo-relative or temp-directory paths only.
2. Host-conformance tests (FDA, FSEvents, mdls/mdimport, notarization) are isolated into separate labels/lanes.

## 3.6 CI/Release Separation

- PR required CI checks do not run production release publication/signing pipelines.
- Release workflows are manual/tag gated and run separately from required PR checks.

## 4. Current Repository Gaps (Must Be Tracked)

1. `/Users/rexliu/betterspotlight/.github/workflows/ci.yml` currently installs mutable Homebrew dependencies.
2. `/Users/rexliu/betterspotlight/CMakeLists.txt` defines `BETTERSPOTLIGHT_FETCH_MODELS` with current default behavior that is not deterministic enough for CI unless explicitly controlled.
3. Machine-specific absolute paths exist in several tests (listed above).
4. Current workflow Actions use tag refs (`@v4`) instead of immutable SHAs.

## 5. Reproducibility Verification Protocol

## 5.1 Build Surfaces Under Comparison

Use unsigned, unstapled artifacts for strict reproducibility comparison.

Do not compare notarized/signature-bearing artifacts byte-for-byte, because trust metadata is expected to differ.

## 5.2 Protocol Steps

1. Select commit `X` and set fixed env contract.
2. Build twice on local machine with clean build directories.
3. Build once on Namespace required lane.
4. Build once on GitHub-hosted fallback/canary lane.
5. Collect:
- artifact digests,
- build metadata (toolchain identity, runner label, command contract version),
- test result summaries.
6. Compare normalized outputs and document deviations.

## 5.3 Acceptance Criteria

- Deterministic surfaces: equivalent digests after normalization.
- Test outcomes: no unexpected pass/fail divergence for deterministic labels.
- Any divergence must have root cause classification and owner.

## 5.4 Acceptable Nondeterministic Fields

Allowed differences (must be documented):

- code signature blobs,
- notarization tickets/staples,
- release packaging timestamps if not normalized,
- external trust-service response metadata.

## 6. Incident Playbook (Determinism Regression)

1. Detect: reproducibility check or parity lane fails.
2. Contain:
- freeze lockfile/toolchain updates,
- pause non-critical workflow changes.
3. Triage categories:
- toolchain drift,
- action drift,
- network asset drift,
- environment drift,
- source-level nondeterministic code path.
4. Mitigate with smallest reversible patch.
5. Backfill:
- add regression test/check,
- add policy guard where gap was found.
6. Record in incident log with timeline, impact, fix commit, and prevention action.

## 7. Pull Request Hygiene Checklist

- [ ] No mutable Action references in touched workflows.
- [ ] Lockfile changes reviewed and justified.
- [ ] No new absolute machine paths in deterministic tests.
- [ ] Network downloads in required lanes are hash-locked or removed.
- [ ] Command contract remains stable across local and CI lanes.

## 8. Sources

- SOURCE_DATE_EPOCH specification: <https://reproducible-builds.org/specs/source-date-epoch/>
- Build path prefix map specification: <https://reproducible-builds.org/specs/build-path-prefix-map/>
- Clang command line reference: <https://clang.llvm.org/docs/ClangCommandLineReference.html>
- GCC option reference: <https://gcc.gnu.org/onlinedocs/gcc/Overall-Options.html>
- GitHub hosted runners: <https://docs.github.com/en/actions/reference/runners/github-hosted-runners>
- Ghostty CI workflow (Nix-first pattern): <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/test.yml>
- BetterSpotlight CI workflow (current state): `/Users/rexliu/betterspotlight/.github/workflows/ci.yml`
