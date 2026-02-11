# BetterSpotlight Evidence Log (Collected 2026-02-11)

## Environment
- Repository: `/Users/rexliu/betterspotlight`
- Host date: 2026-02-11
- Primary verification build dir: `/Users/rexliu/betterspotlight/build-local`
- Note: existing `/Users/rexliu/betterspotlight/build` was stale/inconsistent for this audit pass; a fresh `build-local` was used for decision-grade verification.

## Pre-remediation Baseline (from audit kickoff)
- `ctest --test-dir build -N`
  - Reported `Total Tests: 52`.
- `ctest --test-dir build --output-on-failure -R "test-query-service-core-improvements" --timeout 90`
  - Failed.
  - Error pattern included:
    - `bs.index: SQL error: SQL logic error`
    - `Failed to apply BM25 weights from settings`
- `ctest --test-dir build --output-on-failure -R "test-ui-sim-query-suite|test-ui-sim-query-suite-stress" --timeout 45`
  - Both timed out at 45s.

## Implementation Evidence (Code-level)
- BM25 control statement corrected to FTS5 `rank` command:
  - `/Users/rexliu/betterspotlight/src/core/index/schema.h:156`
  - `/Users/rexliu/betterspotlight/src/core/index/sqlite_store.cpp:1436`
- New BM25 regression test added:
  - `/Users/rexliu/betterspotlight/Tests/Unit/test_sqlite_store.cpp:27`
  - `/Users/rexliu/betterspotlight/Tests/Unit/test_sqlite_store.cpp:420`
- Adaptive merge expectation update in integration test:
  - `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp:327`
- Explicit tray state model and queue-informed updates:
  - `/Users/rexliu/betterspotlight/src/app/service_manager.h:21`
  - `/Users/rexliu/betterspotlight/src/app/service_manager.cpp:538`
  - `/Users/rexliu/betterspotlight/src/app/service_manager.cpp:561`
- Tray icon state rendering + click to Index Health:
  - `/Users/rexliu/betterspotlight/src/app/main.cpp:179`
  - `/Users/rexliu/betterspotlight/src/app/main.cpp:190`
  - `/Users/rexliu/betterspotlight/src/app/qml/Main.qml:55`
  - `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml:27`
- Onboarding-gated initial indexing:
  - `/Users/rexliu/betterspotlight/src/app/onboarding_controller.h:30`
  - `/Users/rexliu/betterspotlight/src/app/onboarding_controller.cpp:172`
  - `/Users/rexliu/betterspotlight/src/app/service_manager.cpp:370`
  - `/Users/rexliu/betterspotlight/src/app/main.cpp:243`
- Platform setting side-effect abstraction:
  - `/Users/rexliu/betterspotlight/src/app/platform_integration.h:9`
  - `/Users/rexliu/betterspotlight/src/app/platform_integration_mac.mm:21`
  - `/Users/rexliu/betterspotlight/src/app/settings_controller.cpp:247`
  - `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml:176`
- Settings/platform side-effect regression coverage:
  - `/Users/rexliu/betterspotlight/Tests/Unit/test_settings_controller_platform.cpp`
  - `/Users/rexliu/betterspotlight/Tests/CMakeLists.txt`
- Clipboard relevance signals (privacy-gated):
  - `/Users/rexliu/betterspotlight/src/app/settings_controller.h`
  - `/Users/rexliu/betterspotlight/src/app/settings_controller.cpp`
  - `/Users/rexliu/betterspotlight/src/app/search_controller.cpp`
  - `/Users/rexliu/betterspotlight/src/services/query/query_service.cpp`
  - `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml`
  - `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp`
- Long-run gate hardening + automation:
  - `/Users/rexliu/betterspotlight/src/core/ipc/service_base.cpp` (`BETTERSPOTLIGHT_SOCKET_DIR` override)
  - `/Users/rexliu/betterspotlight/Tests/benchmarks/stress_48h.sh` (artifact-dir + isolated socket support)
  - `/Users/rexliu/betterspotlight/Tests/benchmarks/memory_drift_24h.sh` (artifact-dir + isolated socket support + RPC retries)
  - `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml`
- Signing/notarization pipeline artifacts:
  - `/Users/rexliu/betterspotlight/scripts/release/notarize_with_sparkle.sh`
  - `/Users/rexliu/betterspotlight/packaging/macos/entitlements.plist`
  - `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml`

## Post-remediation Build + Test Commands

### 1) Fresh configure/build
```bash
cmake -S . -B build-local -DPOPPLER_QT6_FOUND=FALSE -DPOPPLER_CPP_FOUND=FALSE -DBETTERSPOTLIGHT_ENABLE_SPARKLE=OFF
cmake --build build-local -j8
```
Result:
- Build succeeded for app, services, and tests in `build-local`.

### 2) BM25 regression test presence
```bash
build-local/tests/test-sqlite-store -functions | rg "Bm25WeightsCanBeApplied"
```
Result:
- `testBm25WeightsCanBeApplied()` present.

### 3) Query-service core integration (verbose)
```bash
ctest --test-dir build-local -V -R "^test-query-service-core-improvements$" --timeout 120
```
Result:
- Pass.
- Log includes:
  - `bs.index: Applied BM25 weights (name=10.000000, path=5.000000, content=0.500000)`
- No `SQL logic error` BM25 failures in this run.

### 4) UI simulation suite (including stress variant)
```bash
ctest --test-dir build-local --output-on-failure -R "^test-ui-sim-query-suite$|^test-ui-sim-query-suite-stress$" --timeout 45
```
Result:
- `test-ui-sim-query-suite` passed in `32.47 sec` (latest run).
- `test-ui-sim-query-suite-stress` passed in `22.20 sec` (latest run).

### 5) Full suite verification
```bash
ctest --test-dir build-local --output-on-failure --timeout 120
```
Result:
- `100% tests passed, 0 tests failed out of 54`.
- Total runtime: `67.03 sec` (latest rerun after settings/platform test addition).

### 6) Inventory check
```bash
ctest --test-dir build-local -N | tail -n 6
```
Result:
- Shows tests `#49` through `#54`.
- `Total Tests: 54`.

### 6b) Settings/platform side-effect coverage
```bash
ctest --test-dir build-local --output-on-failure -R test-settings-controller-platform --timeout 120
```
Result:
- `test-settings-controller-platform` passed in `0.07 sec`.
- Confirms success/failure behavior and persistence semantics for `launchAtLogin` and `showInDock`.

## Additional Verification Snippets
- Ensure old broken BM25 control statement is gone:
```bash
rg -n "INSERT INTO search_index\(search_index, rank\) VALUES\('fts5'" src || true
```
Result:
- No matches.

### 7) Long-run harness smoke (artifact-dir + isolated-socket verification)
```bash
BS_STRESS_QUERY_BIN=build-local/src/services/query/betterspotlight-query \
BS_STRESS_INDEXER_BIN=build-local/src/services/indexer/betterspotlight-indexer \
BS_STRESS_EXTRACTOR_BIN=build-local/src/services/extractor/betterspotlight-extractor \
BS_STRESS_SAMPLE_INTERVAL=2 \
BS_STRESS_INTEGRITY_INTERVAL=10 \
BS_STRESS_ARTIFACT_DIR=/tmp/bs_stress_smoke \
./Tests/benchmarks/stress_48h.sh 30
```
Result:
- Pass (`errors=[]`, `integrityFailures=0`).
- Artifact directory created deterministically at `/tmp/bs_stress_smoke`.

```bash
BS_MEM_QUERY_BIN=build-local/src/services/query/betterspotlight-query \
BS_MEM_INDEXER_BIN=build-local/src/services/indexer/betterspotlight-indexer \
BS_MEM_EXTRACTOR_BIN=build-local/src/services/extractor/betterspotlight-extractor \
BS_MEM_SAMPLE_INTERVAL=5 \
BS_MEM_DRIFT_LIMIT_MB=50 \
BS_MEM_ARTIFACT_DIR=/tmp/bs_memory_smoke \
./Tests/benchmarks/memory_drift_24h.sh 30
```
Result:
- Pass for all services (`query`, `indexer`, `extractor`) under smoke threshold.
- Artifact directory created deterministically at `/tmp/bs_memory_smoke`.

### 8) Notarization pipeline preflight smoke
```bash
BS_SKIP_BUILD=1 \
BS_REQUIRE_SPARKLE=0 \
BS_NOTARIZE=0 \
BS_CREATE_DMG=0 \
BS_RELEASE_BUILD_DIR=/Users/rexliu/betterspotlight/build-local \
BS_RELEASE_OUTPUT_DIR=/tmp/bs_notary_smoke \
DEVELOPER_ID=- \
./scripts/release/notarize_with_sparkle.sh
```
Result:
- Pass (ad-hoc sign + bundle zip generation).
- Produced `/tmp/bs_notary_smoke/release_summary.json`.
- Full notarization remains credential-gated (`APPLE_ID`, `APPLE_APP_SPECIFIC_PASSWORD`, `TEAM_ID`).

## Evidence-based Conclusions
- BM25 configuration bug is remediated and verified by integration runtime logs plus unit coverage.
- Adaptive merge debug contract mismatch is remediated and integration-verified.
- Clipboard-aware relevance signal path is implemented and integration-verified.
- App lifecycle targeted coverage is now landed with deterministic integration assertions for tray-state transitions and onboarding/indexing first-run gating.
- Settings/platform side-effect behavior now has deterministic regression coverage.
- Prior timeout-prone UI-sim tests continue to complete under the same 45s timeout budget.
- Long-run and notarization gates are now automated/scripted and smoke-validated locally.
- Workflow dispatch evidence recorded for self-hosted operational gates:
  - Long-Run Gates: [run 21847178417](https://github.com/theroutercompany/betterspotlight/actions/runs/21847178417) (dispatched; current status not re-queried in this snapshot).
  - Notarization Verify: [run 21847178969](https://github.com/theroutercompany/betterspotlight/actions/runs/21847178969) (dispatched; current status not re-queried in this snapshot).
- Current source state in `build-local` meets full-suite pass gate (54/54).
