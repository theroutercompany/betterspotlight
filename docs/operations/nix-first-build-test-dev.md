# Nix-First Build/Test/Dev Environment

Document status: Active
Last updated: 2026-02-13
Owner: Core Platform / Developer Experience

## 1. Scope and Boundaries

Nix-first for BetterSpotlight means:

- The developer toolchain and test/build environment are pinned and reproducible.
- Local and CI command contracts are executed through `nix develop -c ...` where practical.
- Dependency versions and shell tooling drift are controlled by `flake.lock`.

Nix-first does not mean:

- Replacing Apple code-signing trust.
- Replacing notarization trust.
- Forcing all `xcodebuild` packaging/signing flows to run inside Nix.

Runtime remains host-native macOS. We do not use VM runtime encapsulation for the desktop app.

## 2. Current State Snapshot (Repository)

As of this document update:

- No `flake.nix` or `flake.lock` exists in `/Users/rexliu/betterspotlight`.
- `/Users/rexliu/betterspotlight/.github/workflows/ci.yml` installs latest Homebrew packages directly.
- `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml` and `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml` run on self-hosted macOS labels.
- Release scripting is native and Apple-tooling driven in `/Users/rexliu/betterspotlight/scripts/release/build_macos_release.sh`.

This is the expected baseline before migrating to Nix-first contracts.

## 3. Target Architecture

## 3.1 Flake Outputs

Introduce a repo flake with these outputs:

1. `devShells.aarch64-darwin.default`
- Build/test toolchain for local development on Apple Silicon.
- Includes CMake/Ninja/Qt integration helpers and test dependencies.

2. `checks.aarch64-darwin`
- Deterministic check surfaces that can run in CI (`cmake configure`, `build`, selected `ctest` partitions, lint checks).

3. Optional: `devShells.x86_64-linux.default`
- Supports Linux smoke/check lanes on Namespace or other Linux runners for non-macOS-specific surfaces.

## 3.2 Directory Layout

Proposed layout:

- `/Users/rexliu/betterspotlight/flake.nix`
- `/Users/rexliu/betterspotlight/flake.lock`
- `/Users/rexliu/betterspotlight/nix/devshell.nix`
- `/Users/rexliu/betterspotlight/nix/checks.nix`
- `/Users/rexliu/betterspotlight/nix/packages/` (optional, if package-level derivations are added)

## 3.3 Lock Update Policy

- `flake.lock` changes require explicit PR review.
- Lock updates should be periodic and intentional, not opportunistic.
- CI can include a dedicated lock freshness lane, but release-critical lanes must remain lock-pinned.

## 4. Darwin-Specific Rules

## 4.1 Xcode Selection Contract

Before native app packaging/signing/notarization steps:

```bash
sudo xcode-select --switch /Applications/Xcode.app
xcodebuild -version
```

If multiple Xcode versions are installed, CI and local release scripts must pin the intended app bundle path.

## 4.2 Tooling Boundary Rule

- Use Nix shell for deterministic dependency/toolchain and non-signing build/test surfaces.
- Run `xcodebuild`, `codesign`, `notarytool`, and `stapler` in native host context when toolchain conflicts appear.

This follows the proven Ghostty pattern for macOS native app pipelines.

## 4.3 Environment Normalization (Darwin)

In Nix shell hooks for macOS, guard against SDK/tool selection drift:

- Ensure `xcodebuild` resolves to the intended system Xcode.
- Avoid stale `SDKROOT`/`DEVELOPER_DIR` overrides unless explicitly required by the lane.

## 5. Canonical Command Contract

These commands are the public interface for local and CI usage.

## 5.1 Local Development

1. Enter pinned shell:

```bash
nix develop
```

2. Configure:

```bash
nix develop -c cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
```

3. Build:

```bash
nix develop -c cmake --build build-release -j"$(sysctl -n hw.ncpu)"
```

4. Test (full deterministic labels):

```bash
nix develop -c ctest --test-dir build-release -L "^(unit|integration|service_ipc|relevance|docs_lint)$" --output-on-failure
```

5. Coverage gate:

```bash
nix develop -c env BS_COVERAGE_PHASE=phase1 /bin/bash tools/coverage/run_gate.sh
```

## 5.2 Native Release/Notarization (Outside Nix Shell)

```bash
BS_REQUIRE_SPARKLE=1 BS_CREATE_DMG=1 BS_NOTARIZE=1 \
  /Users/rexliu/betterspotlight/scripts/release/notarize_with_sparkle.sh
```

This preserves host-native Apple toolchain behavior.

## 6. CI Usage Pattern

## 6.1 Namespace-First Required Lanes

Recommended lane shape:

- `runs-on: namespace-profile-betterspotlight-macos-<size>`
- install Nix
- attach Namespace cache volume/action
- execute canonical contracts via `nix develop -c ...`

Representative pattern (adapted from Ghostty usage style):

```yaml
jobs:
  pr-build-test:
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
```

## 6.2 GitHub-Hosted Fallback/Canary Lane

Keep a minimal fallback lane on GitHub-hosted macOS runners (`macos-15` and/or `macos-26` labels as available) for:

- runner resilience,
- independent parity checks,
- migration safety during Namespace incidents.

Fallback lane should run the same canonical command contract with minimal branching.

## 7. Migration Plan (Nix-First Adoption)

1. Add `flake.nix` + `flake.lock` and `nix/devshell.nix`.
2. Make local contributor docs point to canonical Nix commands.
3. Add Namespace PR required lane running canonical Nix contract.
4. Add GitHub-hosted fallback/canary lane.
5. Promote branch protection required checks to new aggregate CI gate.
6. Keep release/notarization workflows separate from PR-required checks.

## 8. Ghostty Case Study: What to Copy vs Avoid

## 8.1 Copy

- Strong Nix-first discipline for test/build surfaces.
- Explicit recognition that native macOS app build/signing may need host toolchain execution.
- SHA-pinned action usage.
- Required-check aggregation pattern.

## 8.2 Avoid Copying Blindly

- Ghostty's exact custom runner labels (these are org-specific).
- Their full matrix breadth without considering BetterSpotlight constraints.
- Any action references pinned to moving refs (for example `@main`).

## 9. Sources

- Ghostty repository: <https://github.com/ghostty-org/ghostty>
- Ghostty flake: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/flake.nix>
- Ghostty test workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/test.yml>
- Ghostty release workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/release-tag.yml>
- Nix flakes concept: <https://nix.dev/concepts/flakes.html>
- Namespace cache action README: <https://raw.githubusercontent.com/namespacelabs/nscloud-cache-action/main/README.md>
- Namespace setup action README: <https://raw.githubusercontent.com/namespacelabs/nscloud-setup/main/README.md>
- GitHub hosted runners: <https://docs.github.com/en/actions/reference/runners/github-hosted-runners>
- GitHub larger runners: <https://docs.github.com/en/actions/reference/runners/larger-runners>
