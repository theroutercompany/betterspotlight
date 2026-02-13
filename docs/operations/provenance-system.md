# Provenance System

Document status: Active
Last updated: 2026-02-13
Owner: Release Engineering / Security Engineering

## 1. Purpose

Provide cryptographic and operational traceability for BetterSpotlight artifacts by linking:

- source identity
- builder identity
- artifact identity
- Apple trust evidence

## 2. Provenance Architecture

## 2.1 Source Identity (Required)

- repository (`owner/repo`)
- commit SHA
- ref (`refs/heads/*` or `refs/tags/*`)
- workflow ref (when CI-built)
- tree state (`clean` or `dirty`)

## 2.2 Builder Identity (Required)

- provider (`namespace`, `github-hosted`, or `local`)
- runner class/label
- workflow name + run id + job

## 2.3 Artifact Identity (Required)

- artifact name
- path
- class
- SHA-256 digest
- size
- attestation verification contract

## 3. Release Manifest Contract

Manifest path:

- `/Users/rexliu/betterspotlight/dist/release/release-manifest.json`

Generator:

- `/Users/rexliu/betterspotlight/scripts/release/generate_release_manifest.py`

Release script integration:

- `/Users/rexliu/betterspotlight/scripts/release/build_macos_release.sh`

Top-level required fields:

- `schemaVersion`
- `source`
- `builder`
- `artifacts`
- `appleTrust`
- `generatedAt`

## 4. Attestation Workflows

Primary provenance workflow:

- `/Users/rexliu/betterspotlight/.github/workflows/provenance.yml`

Release workflow (tag/manual):

- `/Users/rexliu/betterspotlight/.github/workflows/release.yml`

Scheduled drill workflow:

- `/Users/rexliu/betterspotlight/.github/workflows/provenance-drill.yml`

## 5. Verification Contract

CLI verifier helper:

- `/Users/rexliu/betterspotlight/tools/verify_attestation.sh`

Canonical command:

```bash
gh attestation verify <artifact> --repo <owner/repo>
```

In CI drills/releases, verification is executed against `dist/release/release-manifest.json`.

## 6. Apple Trust Infrastructure (External)

Apple signing/notarization are external trust decisions, not substitutes for dependency reproducibility.

Implications:

1. Reproducible artifacts can still fail notarization.
2. Notarized artifacts can still come from nondeterministic builds.
3. Release confidence requires both reproducibility controls and Apple trust checks.

## 7. Verification Matrix

| Scenario | Required Outputs | Verification |
|---|---|---|
| PR deterministic lanes | deterministic build/test outcomes | CI parity + gate results |
| Provenance workflow | manifest + attestation | `gh attestation verify` |
| Release workflow | manifest + attestation + Apple evidence | internal QA verification |
| Provenance drill | scheduled attest + verify | drill history and run logs |

## 8. Retention Policy

- keep release manifests for each supported release lifecycle
- keep provenance drill/workflow artifacts at least 90 days (or stricter policy if required)

## 9. Sources

- GitHub artifact attestations: <https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations>
- `gh attestation verify`: <https://cli.github.com/manual/gh_attestation_verify>
- Apple notarization workflow: <https://developer.apple.com/documentation/security/customizing-the-notarization-workflow>
- Apple Platform Security (code signing): <https://support.apple.com/en-me/guide/security/sec3ad8e6e53/web>
- Apple Platform Security (Gatekeeper/notarization context): <https://support.apple.com/en-afri/guide/security/sec5599b66df/web>
