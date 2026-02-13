# Reproducibility Program Index

Document status: Active
Last updated: 2026-02-13
Owner: Core Platform / Release Engineering

## 1. Program Summary

This index is the entrypoint for BetterSpotlight's end-to-end reproducibility program.

Locked decisions:

1. runtime remains host-native macOS (no VM runtime encapsulation)
2. Namespace-first CI with GitHub-hosted fallback/canary
3. Nix-first dev/build/test surfaces with Apple trust flows kept host-native

## 2. Platform Support Contract

- Hardware: Apple Silicon (M1+)
- Memory: 16 GB minimum RAM
- OS policy: macOS 15 supported, macOS 26 remains canary until promoted

## 3. Read Order

1. [Nix-First Build/Test/Dev](nix-first-build-test-dev.md)
2. [Deterministic Build Hygiene](deterministic-build-hygiene.md)
3. [Provenance System](provenance-system.md)
4. [macOS CI: Namespace-First + GitHub-Hosted Fallback](github-hosted-macos-ci.md)

## 4. Implementation Artifacts

Core implementation files:

- `/Users/rexliu/betterspotlight/flake.nix`
- `/Users/rexliu/betterspotlight/flake.lock`
- `/Users/rexliu/betterspotlight/scripts/ci/configure.sh`
- `/Users/rexliu/betterspotlight/scripts/ci/build.sh`
- `/Users/rexliu/betterspotlight/scripts/ci/test.sh`
- `/Users/rexliu/betterspotlight/scripts/ci/coverage.sh`
- `/Users/rexliu/betterspotlight/tools/model_assets.lock.json`
- `/Users/rexliu/betterspotlight/tools/prefetch_model_assets.sh`
- `/Users/rexliu/betterspotlight/tools/ci/verify_workflow_action_pins.sh`
- `/Users/rexliu/betterspotlight/scripts/release/generate_release_manifest.py`
- `/Users/rexliu/betterspotlight/tools/verify_attestation.sh`

Workflow implementation set:

- `/Users/rexliu/betterspotlight/.github/workflows/ci-v2.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/provenance.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/release.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/fallback-drill.yml`
- `/Users/rexliu/betterspotlight/.github/workflows/provenance-drill.yml`

## 5. Definition of Done Checklist

- [x] Nix-first command contracts implemented and documented
- [x] deterministic model asset lock + hash enforcement implemented
- [x] absolute machine-path assumptions removed from deterministic test surfaces
- [x] action SHA pinning enforced by CI policy check
- [x] Namespace-first shadow CI with aggregate required gate implemented
- [x] release manifest + attestation generation and verification integrated
- [x] release workflow separated from PR-required CI
- [x] fallback/provenance drill workflows implemented
- [ ] branch protection cutover executed in GitHub settings
- [ ] sustained stability window evidence recorded for cutover

## 6. Validation Scenarios

1. local deterministic rebuild rehearsal (same commit, two clean builds)
2. cross-runner parity rehearsal (Namespace vs GitHub-hosted canary)
3. PR gate separation check (release paths not executed on PR)
4. model lock integrity check (tampered hash fails deterministically)
5. provenance verification drill (`gh attestation verify`)
6. fallback drill (GitHub-hosted lane validated for outage cutover)

## 7. Sources

- Ghostty repository: <https://github.com/ghostty-org/ghostty>
- Ghostty CI workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/test.yml>
- Ghostty release workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/release-tag.yml>
- Nix flakes concept: <https://nix.dev/concepts/flakes.html>
- SOURCE_DATE_EPOCH: <https://reproducible-builds.org/specs/source-date-epoch/>
- Build path prefix map: <https://reproducible-builds.org/specs/build-path-prefix-map/>
- GitHub artifact attestations: <https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations>
- `gh attestation verify`: <https://cli.github.com/manual/gh_attestation_verify>
