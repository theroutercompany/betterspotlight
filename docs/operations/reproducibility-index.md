# Reproducibility Program Index

Document status: Active
Last updated: 2026-02-13
Owner: Core Platform / Release Engineering

## Purpose

This is the entrypoint for BetterSpotlight's mission-critical reproducibility program.

Locked decisions for this program:

1. Runtime remains host-native on macOS. No VM runtime encapsulation for the desktop app.
2. CI runner strategy is Namespace-first with a GitHub-hosted macOS fallback and canary lane.
3. Nix-first is used for toolchain and build/test reproducibility, not as a replacement for Apple signing and notarization trust infrastructure.

## Program Assumptions

1. Namespace account, runner profiles, and billing are approved for required CI usage.
2. GitHub-hosted macOS runners remain configured for fallback/canary parity lanes.
3. This phase is documentation-first; workflow and code changes needed to fully enforce all controls are tracked as follow-on implementation work.

## Platform Support Contract

- Hardware: Apple Silicon (M1 or newer)
- Minimum memory: 16 GB RAM
- Supported OS targets: macOS 15 and macOS 26 (26 is treated as preview/canary until promoted)

## Read Order

1. [Nix-First Build/Test/Dev](nix-first-build-test-dev.md)
2. [Deterministic Build Hygiene](deterministic-build-hygiene.md)
3. [Provenance System](provenance-system.md)
4. [macOS CI: Namespace-First + GitHub-Hosted Fallback](github-hosted-macos-ci.md)

## Program Phases

### Phase 0 - Baseline and Contracts

- Publish and ratify the four documents linked above.
- Freeze canonical CI check names and branch protection expectations.
- Record reproducibility assumptions and accepted nondeterminism list.

### Phase 1 - Nix Foundation

- Introduce `flake.nix` and `flake.lock` for dev/build/test toolchain pinning.
- Move repeatable build/test invocations to `nix develop -c ...` command contracts.
- Keep native packaging/signing/notarization steps outside Nix shell when required by Apple tooling behavior.

### Phase 2 - Determinism Enforcement

- Pin GitHub Actions to immutable commit SHAs.
- Replace opportunistic network model fetches in CI with locked asset materialization.
- Remove machine-specific paths from deterministic test suites.

### Phase 3 - Provenance and Verification

- Emit artifact attestations for CI/release artifacts.
- Publish release manifest metadata with artifact digests and builder identity.
- Add reproducibility rehearsals across local, Namespace, and GitHub-hosted macOS lanes.

### Phase 4 - Operational Resilience

- Run scheduled fallback drills for GitHub-hosted lanes.
- Define incident response and temporary branch protection changes for runner outages.
- Promote canary runner/image labels only after pass-rate and drift review.

## Definition of Done (Program)

- [ ] Reproducibility docs are complete, source-backed, and versioned in repo.
- [ ] Canonical local and CI command contracts are documented and used by contributors.
- [ ] Required PR CI checks are separated from production release workflows.
- [ ] Namespace macOS required checks and GitHub-hosted fallback/canary checks are both operational.
- [ ] Deterministic build hygiene controls are documented with enforcement points.
- [ ] Provenance attestations and manifest verification are documented and executable.
- [ ] Apple signing and notarization trust model is documented as separate from dependency reproducibility.
- [ ] Cross-runner reproducibility rehearsals are defined and repeatable.

## Validation Scenarios

1. Local determinism rehearsal: same commit, same environment, two builds, identical normalized outputs.
2. Cross-runner rehearsal: Namespace macOS vs GitHub-hosted macOS canary build/test parity.
3. CI gate separation: PR workflows cannot trigger production release publication.
4. Provenance verification drill: generate and verify attestation from clean environment.
5. Fallback drill: simulate Namespace lane outage and temporarily elevate GitHub-hosted fallback checks.

## Related Repository Files

- Current primary CI workflow: `/Users/rexliu/betterspotlight/.github/workflows/ci.yml`
- Current long-run workflow: `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml`
- Current notarization verification workflow: `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml`
- Current model fetch script: `/Users/rexliu/betterspotlight/tools/fetch_embedding_models.sh`
- Current release scripts: `/Users/rexliu/betterspotlight/scripts/release/build_macos_release.sh`, `/Users/rexliu/betterspotlight/scripts/release/notarize_with_sparkle.sh`

## Sources

- Ghostty repository: <https://github.com/ghostty-org/ghostty>
- Ghostty flake: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/flake.nix>
- Ghostty CI workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/test.yml>
- Ghostty release workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/release-tag.yml>
- GitHub hosted runners: <https://docs.github.com/en/actions/reference/runners/github-hosted-runners>
- GitHub larger runners: <https://docs.github.com/en/actions/reference/runners/larger-runners>
- GitHub artifact attestations: <https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations/using-artifact-attestations-to-establish-provenance-for-builds>
- `gh attestation verify` CLI: <https://cli.github.com/manual/gh_attestation_verify>
- Nix flakes concept: <https://nix.dev/concepts/flakes.html>
- SOURCE_DATE_EPOCH spec: <https://reproducible-builds.org/specs/source-date-epoch/>
- Build path prefix map spec: <https://reproducible-builds.org/specs/build-path-prefix-map/>
- Apple notarization workflow customization: <https://developer.apple.com/documentation/security/customizing-the-notarization-workflow>
- Apple Platform Security (code signing): <https://support.apple.com/en-me/guide/security/sec3ad8e6e53/web>
- Apple Platform Security (Gatekeeper/notarization context): <https://support.apple.com/en-afri/guide/security/sec5599b66df/web>
