# macOS CI on Namespace-First + GitHub-Hosted Fallback

Document status: Active
Last updated: 2026-02-13
Owner: Release Engineering

## 1. Strategy

Locked strategy:

1. Namespace-first required PR gates.
2. GitHub-hosted macOS fallback/canary lane kept active.
3. Release/notarization paths are separate from PR-required gates.

## 2. Implemented Workflows

Primary/legacy CI:

- `/Users/rexliu/betterspotlight/.github/workflows/ci.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/ci-v2.yml`

Release/provenance/drills:

- `/Users/rexliu/betterspotlight/.github/workflows/release.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/provenance.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/fallback-drill.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/provenance-drill.yml`

Operational/manual lanes:

- `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml`

## 2.1 Baseline Snapshot (2026-02-13)

Legacy baseline jobs preserved for shadow migration context:

- `Release Build + CTest`
- `Debug ASAN/UBSAN`
- `Coverage Gate`

Shadow migration is implemented by running `/Users/rexliu/betterspotlight/.github/workflows/ci.yml` alongside `/Users/rexliu/betterspotlight/.github/workflows/ci-v2.yml` until branch-protection cutover.

## 3. Required Check Contract

Canonical gate name:

- `required-pr-core`

Canonical lane names:

- `pr-build-release`
- `pr-build-sanitizers`
- `pr-coverage-gate`
- `canary-gh-hosted-macos`

Trigger contract:

- `pull_request`
- `push` to `main`
- `workflow_dispatch`
- scheduled canary/drill workflows

## 4. Repository Variables and Secrets Contract

Required repository variables:

- `NS_RUNNER_MACOS_SM`
- `NS_RUNNER_MACOS_MD`
- `NS_RUNNER_MACOS_LG`
- `BS_MODEL_MIRROR_BASE_URL`

Required release/notarization secrets:

- `DEVELOPER_ID`
- `APPLE_ID`
- `APPLE_APP_SPECIFIC_PASSWORD`
- `TEAM_ID`

## 5. Branch Protection Cutover (Two-Step)

## Step 1: Shadow

- keep old and new checks required temporarily
- require green runs over a stability window before switching

## Step 2: Switch

- require only `required-pr-core`
- remove old required check names after stable window

## 6. CI/Release Separation Policy

PR-required CI lanes must never publish production release artifacts.

Production publication/signing is restricted to:

- `/Users/rexliu/betterspotlight/.github/workflows/release.yml`
- manual notarization verification in `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml`

## 7. Outage Fallback Runbook

If Namespace lanes are degraded:

1. confirm incident scope
2. temporarily elevate GitHub-hosted gate as required
3. keep heavy/non-critical lanes disabled first (long-run stress before core checks)
4. restore Namespace requirement after recovery

Scheduled fallback rehearsal workflow:

- `/Users/rexliu/betterspotlight/.github/workflows/fallback-drill.yml`

## 8. Cost and Performance Governance

- small/medium profiles for fast PR checks
- medium/large profiles for sanitizer/coverage/release lanes
- explicit concurrency groups on long-running or release workflows
- cache `/nix` on Namespace-backed lanes where applicable

## 9. Ghostty Lessons Applied

Adopted:

- aggregate required check pattern
- Nix-first build/test discipline
- strong action SHA pinning
- CI vs release separation

Not copied directly:

- Ghostty runner labels
- any floating action refs (`@main`)
- Ghostty matrix breadth not justified for BetterSpotlight constraints

## 10. Sources

- Ghostty test workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/test.yml>
- Ghostty release workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/release-tag.yml>
- GitHub-hosted runners: <https://docs.github.com/en/actions/reference/runners/github-hosted-runners>
- GitHub larger runners: <https://docs.github.com/en/actions/reference/runners/larger-runners>
