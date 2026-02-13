# Nix-First Build/Test/Dev Environment

Document status: Active
Last updated: 2026-02-13
Owner: Core Platform / Developer Experience

## 1. Scope and Boundaries

BetterSpotlight is Nix-first for development, configure/build/test, and CI command parity.

Nix-first means:

- reproducible toolchain inputs via `/Users/rexliu/betterspotlight/flake.nix` + `/Users/rexliu/betterspotlight/flake.lock`
- canonical command contracts through `/Users/rexliu/betterspotlight/scripts/ci/*.sh`
- CI and local execution through `nix develop -c <script>` in Nix-backed lanes

Nix-first does not mean:

- replacing Apple trust infrastructure (`codesign`, notarization, stapling)
- forcing `xcodebuild` and notarization to run inside Nix
- changing runtime architecture (runtime stays host-native macOS)

## 2. Implemented Repository Layout

Implemented files:

- `/Users/rexliu/betterspotlight/flake.nix`
- `/Users/rexliu/betterspotlight/flake.lock`
- `/Users/rexliu/betterspotlight/nix/devshell.nix`
- `/Users/rexliu/betterspotlight/nix/checks.nix`
- `/Users/rexliu/betterspotlight/scripts/ci/configure.sh`
- `/Users/rexliu/betterspotlight/scripts/ci/build.sh`
- `/Users/rexliu/betterspotlight/scripts/ci/test.sh`
- `/Users/rexliu/betterspotlight/scripts/ci/coverage.sh`

## 3. Command Contract (Local + CI)

Canonical contract:

1. Configure: `/Users/rexliu/betterspotlight/scripts/ci/configure.sh`
2. Build: `/Users/rexliu/betterspotlight/scripts/ci/build.sh`
3. Test: `/Users/rexliu/betterspotlight/scripts/ci/test.sh`
4. Coverage: `/Users/rexliu/betterspotlight/scripts/ci/coverage.sh`

Required Nix wrapper contract:

```bash
nix develop -c ./scripts/ci/configure.sh build-release Release
nix develop -c ./scripts/ci/build.sh build-release
nix develop -c ./scripts/ci/test.sh build-release
nix develop -c ./scripts/ci/coverage.sh
```

### 3.1 Deterministic Environment Defaults

`configure.sh` normalizes these by default:

- `LC_ALL=C`
- `LANG=C`
- `TZ=UTC`
- `SOURCE_DATE_EPOCH` (from latest commit timestamp when unset)

## 4. Darwin Host Boundary Rules

Apple-native release and trust operations remain host-native:

- `/Users/rexliu/betterspotlight/scripts/release/build_macos_release.sh`
- `/Users/rexliu/betterspotlight/scripts/release/notarize_with_sparkle.sh`

If release lanes require explicit Xcode selection:

```bash
sudo xcode-select --switch /Applications/Xcode.app
xcodebuild -version
```

## 5. CI Usage Pattern

Nix-backed CI lanes are implemented in:

- `/Users/rexliu/betterspotlight/.github/workflows/ci-v2.yml`

Lane contract:

- install Nix
- prefetch locked model assets
- run canonical scripts via `nix develop -c`

## 6. Lock Update Policy

- `flake.lock` changes are intentional and reviewed.
- Lock updates are not bundled with unrelated feature work.
- CI/release regressions after lock updates are treated as release-risk incidents.

## 7. Ghostty-Informed Guidance

Adopted from Ghostty practice:

- Nix-first discipline for build/test surfaces
- aggregate required-check pattern
- explicit split between PR CI and release paths

Not copied blindly:

- Ghostty runner labels
- `@main` action references
- platform matrix breadth irrelevant to BetterSpotlight

## 8. Sources

- Ghostty repository: <https://github.com/ghostty-org/ghostty>
- Ghostty flake: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/flake.nix>
- Ghostty CI workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/test.yml>
- Ghostty release workflow: <https://raw.githubusercontent.com/ghostty-org/ghostty/main/.github/workflows/release-tag.yml>
- Nix flakes concept: <https://nix.dev/concepts/flakes.html>
