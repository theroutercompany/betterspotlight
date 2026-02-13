# Provenance System

Document status: Active
Last updated: 2026-02-13
Owner: Release Engineering / Security Engineering

## 1. Purpose

Define a verifiable provenance model for BetterSpotlight artifacts across CI and release workflows.

This provenance system addresses:

- who built an artifact,
- from what source state,
- on what runner class,
- with what workflow identity,
- with what digest output.

It also documents why Apple signing/notarization trust is a distinct external trust layer and not equivalent to dependency reproducibility.

## 2. Provenance Architecture

## 2.1 Source Identity

Required fields:

- repository (`owner/repo`)
- commit SHA
- branch or tag ref
- tree cleanliness (`clean` or `dirty` for local/internal builds)
- workflow file path and workflow run ID (for CI builds)

## 2.2 Builder Identity

Required fields:

- runner provider (`namespace` or `github-hosted`)
- runner label/profile
- OS image label and version details if available
- workflow/job name
- build start/end timestamps (UTC)

## 2.3 Artifact Identity

Required fields:

- artifact name
- artifact digest (SHA-256 minimum)
- artifact class (`unsigned-app`, `zip`, `dmg`, `symbols`, `logs`)
- attestation reference (if generated)

## 3. Public Contract: Release Manifest

Every release workflow must emit a `release-manifest.json` file.

Reference schema (minimum required fields):

```json
{
  "schemaVersion": "1.0",
  "project": "betterspotlight",
  "source": {
    "repository": "owner/repo",
    "commit": "<40-char-sha>",
    "ref": "refs/tags/vX.Y.Z"
  },
  "builder": {
    "provider": "namespace",
    "runnerLabel": "namespace-profile-betterspotlight-macos-md",
    "workflow": "release-tag.yml",
    "runId": "123456789"
  },
  "artifacts": [
    {
      "name": "BetterSpotlight.app.zip",
      "sha256": "<hex>",
      "class": "zip",
      "attestation": {
        "type": "github-artifact-attestation",
        "verified": false
      }
    }
  ],
  "appleTrust": {
    "signed": true,
    "notarized": true,
    "stapled": true
  },
  "generatedAt": "2026-02-13T00:00:00Z"
}
```

Retention policy:

- keep release manifests for the full supported lifecycle of each release,
- keep CI provenance manifests and logs for at least 90 days (or longer for compliance needs).

## 4. GitHub Artifact Attestations

## 4.1 Workflow Requirements

For attestation-enabled jobs:

- `permissions` must include `attestations: write` and `id-token: write`.
- attestation generation must run after artifact production and digest capture.

Example (shape only, version pin must be immutable):

```yaml
permissions:
  contents: read
  attestations: write
  id-token: write

steps:
  - uses: actions/checkout@<pinned-sha>
  - uses: actions/upload-artifact@<pinned-sha>
    with:
      name: build-artifacts
      path: dist/
  - uses: actions/attest-build-provenance@<pinned-sha>
    with:
      subject-path: dist/*
```

## 4.2 Verification Contract

From a clean machine/environment:

```bash
gh attestation verify <artifact-path-or-uri> --repo <owner/repo>
```

Verification must be captured in release QA notes with:

- command run,
- verifier identity,
- timestamp,
- result (`pass`/`fail`) and failure reason.

## 5. Apple Signing and Notarization as External Trust

Important distinction:

- Reproducible dependencies and deterministic build controls ensure that a given source state is buildable in a controlled way.
- Apple Developer ID signing and notarization are trust assertions issued through Apple's infrastructure.

Therefore:

1. An artifact can be reproducible yet still fail notarization.
2. An artifact can be notarized yet still be poorly reproducible if build inputs are not pinned.
3. Release quality requires both:
- reproducibility controls,
- successful external trust validation.

## 6. Verification Matrix

| Scenario | Required Checks | Expected Outcome |
|---|---|---|
| PR deterministic lane | Build/test contract + digest capture | Stable pass/fail and expected digest behavior |
| Namespace release candidate | Manifest + attestation + Apple signing | Verifiable provenance and trust status |
| GitHub-hosted canary | Same source and command contract | Parity signal and fallback readiness |
| Post-release audit | Manifest lookup + attestation verify + notarization evidence | End-to-end traceability |

## 7. Operational Runbook

1. Generate artifacts.
2. Compute and store SHA-256 digests.
3. Upload artifacts.
4. Generate artifact attestation.
5. Emit `release-manifest.json` and attach to workflow artifacts/release record.
6. Run `gh attestation verify` from clean environment.
7. Record Apple signing/notarization/staple evidence.
8. Publish only if all required checks pass.

## 8. Sources

- GitHub artifact attestations guide: <https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations/using-artifact-attestations-to-establish-provenance-for-builds>
- `gh attestation verify` CLI: <https://cli.github.com/manual/gh_attestation_verify>
- GitHub hosted runners: <https://docs.github.com/en/actions/reference/runners/github-hosted-runners>
- Apple notarization workflow customization: <https://developer.apple.com/documentation/security/customizing-the-notarization-workflow>
- Apple Platform Security (code signing): <https://support.apple.com/en-me/guide/security/sec3ad8e6e53/web>
- Apple Platform Security (Gatekeeper/notarization context): <https://support.apple.com/en-afri/guide/security/sec5599b66df/web>
