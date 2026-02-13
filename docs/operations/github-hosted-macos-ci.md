# macOS CI on Namespace-First + GitHub-Hosted Fallback

Document status: Active
Last updated: 2026-02-13
Owner: Release Engineering

## 1. Runner Strategy

BetterSpotlight uses a dual-runner strategy:

1. Namespace-first required PR lanes for performance and repeatability.
2. GitHub-hosted macOS fallback/canary lane for resilience and independent validation.

This keeps CI operational during provider incidents and continuously validates portability across runner providers.

## 2. CI Lane Topology

## 2.1 Required PR Gates (Namespace)

Primary required checks run on Namespace macOS profiles.

Required lanes (target contract):

- `required-pr-core` (aggregate gate)
- `pr-build-release`
- `pr-build-sanitizers`
- `pr-coverage-gate`

All required lanes must use canonical command contracts and lock-pinned toolchain.

## 2.2 GitHub-Hosted Fallback/Canary

Run a minimal parity lane on GitHub-hosted macOS.

Lane contract:

- name: `canary-gh-hosted-macos`
- trigger: `pull_request`, `workflow_dispatch`, and optional nightly schedule
- scope: configure + build + deterministic test labels
- purpose: parity signal and outage readiness

## 2.3 Long-Run and Notarization Lanes

Long-duration stress and notarization verification workflows are non-PR-required operational gates:

- `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml`

These must remain separate from fast PR-required checks.

## 3. Trigger Policy Contract

- `pull_request`: required PR lanes + optional canary
- `push` to `main`: required PR-equivalent lanes for merge health
- `workflow_dispatch`: manual reruns and incident drills
- `schedule`: canary, long-run reliability, and drift checks

Release publication workflows use explicit manual/tag triggers only.

## 4. Branch Protection Model

Use an aggregate required-check job pattern.

Pattern:

1. Individual jobs run independently.
2. Final aggregate job evaluates `needs.*.result`.
3. Branch protection requires only the aggregate check name.

Benefits:

- stable required check contract,
- easy lane evolution without branch-protection churn,
- single pass/fail signal for reviewers.

## 4.1 Current to Target Check Name Mapping

Current job names in `/Users/rexliu/betterspotlight/.github/workflows/ci.yml`:

- `release-tests`
- `sanitizers`
- `coverage-gate`

Target canonical required-check contract:

- `required-pr-core` (aggregate required check)
- `pr-build-release`
- `pr-build-sanitizers`
- `pr-coverage-gate`

Migration rule: during transition, keep old and new checks green until branch protection is updated to require `required-pr-core`.

## 5. Recommended Workflow Shape

Representative shape (abbreviated):

```yaml
name: CI
on:
  pull_request:
  push:
    branches: [main]

jobs:
  pr-build-release:
    runs-on: namespace-profile-betterspotlight-macos-md
    steps:
      - uses: actions/checkout@<pinned-sha>
      - uses: DeterminateSystems/nix-installer-action@<pinned-sha>
      - uses: namespacelabs/nscloud-cache-action@<pinned-sha>
        with:
          path: |
            /nix
      - run: nix develop -c cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
      - run: nix develop -c cmake --build build-release -j"$(sysctl -n hw.ncpu)"
      - run: nix develop -c ctest --test-dir build-release -L "^(unit|integration|service_ipc|relevance|docs_lint)$" --output-on-failure

  canary-gh-hosted-macos:
    runs-on: macos-15
    steps:
      - uses: actions/checkout@<pinned-sha>
      - uses: DeterminateSystems/nix-installer-action@<pinned-sha>
      - run: nix develop -c cmake -S . -B build-canary -DCMAKE_BUILD_TYPE=Release
      - run: nix develop -c cmake --build build-canary -j"$(sysctl -n hw.ncpu)"
      - run: nix develop -c ctest --test-dir build-canary -L "^(unit|integration|service_ipc|relevance|docs_lint)$" --output-on-failure

  required-pr-core:
    runs-on: namespace-profile-betterspotlight-macos-sm
    needs: [pr-build-release, canary-gh-hosted-macos]
    steps:
      - run: |
          results='${{ toJSON(needs.*.result) }}'
          echo "$results" | grep -Eq 'failure|cancelled' && exit 1 || exit 0
```

## 6. Cost and Performance Governance

## 6.1 Runner Sizing Policy

| Lane Type | Recommended Profile Size | Notes |
|---|---|---|
| Fast PR checks | small/medium | prioritize queue throughput |
| Coverage + sanitizer | medium/large | heavier CPU and memory demand |
| Release packaging | medium/large macOS | includes packaging and signing operations |
| Long-run stress | dedicated long-running profile | isolated from PR queue |

## 6.2 Cache Policy

- Attach cache volumes for deterministic dependency directories (`/nix` and any compiler caches).
- Keep cache keys independent from nondeterministic fields.
- Periodically clean stale cache state during incident triage.

## 6.3 Concurrency Guardrails

- Use workflow-level `concurrency` groups.
- Cancel in-progress for non-main PR runs where safe.
- Keep release workflows single-flight when artifacts can be corrupted by parallelism.

## 7. Outage Fallback Runbook

When Namespace lanes are degraded:

1. Confirm incident scope and expected duration.
2. Temporarily switch branch protection required check from Namespace aggregate to GitHub-hosted aggregate.
3. Keep production release workflows paused unless emergency release is approved.
4. Disable non-critical heavy lanes first (for example long-run stress) before disabling core checks.
5. Restore Namespace required checks and revert temporary rules after recovery.

## 8. Ghostty Case Study: Adopt vs Avoid

## 8.1 Adopt

- Required-check aggregator pattern.
- Nix-first command execution in CI.
- Cache-aware CI structure.
- Explicit separation of test and release workflows.

## 8.2 Avoid Blind Copy

- Ghostty's runner labels (for example `namespace-profile-ghostty-*`) are org-specific and must not be copied verbatim.
- Do not copy `@main` action refs into required lanes.
- Do not copy full matrix breadth without BetterSpotlight-specific ROI.

## 9. Source Anchors in BetterSpotlight

- `/Users/rexliu/betterspotlight/.github/workflows/ci.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml`

## 10. Sources

- Ghostty CI workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/test.yml>
- Ghostty release workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/release-tag.yml>
- Namespace cache action README: <https://raw.githubusercontent.com/namespacelabs/nscloud-cache-action/main/README.md>
- Namespace setup action README: <https://raw.githubusercontent.com/namespacelabs/nscloud-setup/main/README.md>
- GitHub hosted runners: <https://docs.github.com/en/actions/reference/runners/github-hosted-runners>
- GitHub larger runners: <https://docs.github.com/en/actions/reference/runners/larger-runners>
