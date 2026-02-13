# Deterministic Build Hygiene

Document status: Active
Last updated: 2026-02-13
Owner: Core Platform / Quality Engineering

## 1. Objective

Guarantee stable build/test outcomes for the same commit across:

- local macOS developer environment
- Namespace macOS CI lanes
- GitHub-hosted macOS canary/fallback lanes

## 2. Determinism Threat Model

Primary nondeterminism vectors:

- mutable tool versions and action refs
- opportunistic network downloads
- absolute path leakage in tests and filters
- environment drift (`LANG`, `TZ`, timestamps)
- trust-service side effects (signing/notarization metadata)

## 3. Mandatory Controls (Implemented)

## 3.1 Toolchain Pinning

- `/Users/rexliu/betterspotlight/flake.nix`
- `/Users/rexliu/betterspotlight/flake.lock`

All Nix-backed CI lanes execute canonical scripts with `nix develop -c`.

## 3.2 Action SHA Pinning + Enforcement

- Workflows pin every `uses:` reference to immutable commit SHA.
- Enforcement script:
  - `/Users/rexliu/betterspotlight/tools/ci/verify_workflow_action_pins.sh`
- Enforcement wired into CI:
  - `/Users/rexliu/betterspotlight/.github/workflows/ci.yml`
  - `/Users/rexliu/betterspotlight/.github/workflows/ci-v2.yml`

## 3.3 Locked Model Asset Supply Chain

Implemented model lock contract:

- lockfile: `/Users/rexliu/betterspotlight/tools/model_assets.lock.json`
- required fields per asset: `name`, `url`, `sha256`, `size_bytes`, `version`
- verifier/fetcher: `/Users/rexliu/betterspotlight/tools/fetch_embedding_models.sh`
- bootstrap helper: `/Users/rexliu/betterspotlight/tools/prefetch_model_assets.sh`

Behavior:

- hash and size are always verified
- mismatch fails fast
- required CI lanes prefetch explicitly and configure CMake with `-DBETTERSPOTLIGHT_FETCH_MODELS=OFF`

## 3.4 Test Path Determinism Hardening

Path-sensitive unit tests were refactored to remove machine-local fallbacks:

- `/Users/rexliu/betterspotlight/Tests/Unit/test_cross_encoder_reranker.cpp`
- `/Users/rexliu/betterspotlight/Tests/Unit/test_model_registry.cpp`
- `/Users/rexliu/betterspotlight/Tests/Unit/test_reranker_cascade.cpp`
- `/Users/rexliu/betterspotlight/Tests/Unit/test_qa_extractive_model.cpp`

Shared fixture resolver:

- `/Users/rexliu/betterspotlight/Tests/Utils/model_fixture_paths.h`
- `/Users/rexliu/betterspotlight/Tests/Utils/model_fixture_paths.cpp`

## 3.5 Coverage Path Stability

- exclusions now repo-relative:
  - `/Users/rexliu/betterspotlight/Tests/coverage_exclusions.txt`
- gate resolves relative exclusions against repo root:
  - `/Users/rexliu/betterspotlight/tools/coverage/run_gate.sh`

## 3.6 Benchmark Path Stability

Removed absolute local build paths:

- `/Users/rexliu/betterspotlight/Tests/benchmarks/benchmark_search.sh`
- `/Users/rexliu/betterspotlight/Tests/benchmarks/benchmark_indexing.sh`

## 3.7 Environment Contract

Deterministic lanes standardize:

- `LC_ALL=C`
- `LANG=C`
- `TZ=UTC`
- `SOURCE_DATE_EPOCH`

## 4. Reproducibility Verification Protocol

1. Build the same commit twice locally with clean build dirs.
2. Build/test on Namespace lane.
3. Build/test on GitHub-hosted canary lane.
4. Compare deterministic outputs and test outcomes.

Comparison surface for strict parity:

- unsigned, unstapled artifacts
- normalized metadata fields only

Allowed nondeterministic fields:

- code signatures
- notarization tickets/staples
- external trust-service response metadata

## 5. Incident Playbook (Determinism Regression)

1. Detect divergence via CI parity or local rehearsal.
2. Freeze lock/action/tool updates while triaging.
3. Classify source: toolchain, actions, network asset, environment, source code.
4. Apply minimal fix with rollback path.
5. Add regression guard (test/script/policy) before closing incident.

## 6. Sources

- SOURCE_DATE_EPOCH: <https://reproducible-builds.org/specs/source-date-epoch/>
- Build path prefix map: <https://reproducible-builds.org/specs/build-path-prefix-map/>
- Ghostty CI reference: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/test.yml>
