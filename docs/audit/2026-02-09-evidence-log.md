# BetterSpotlight Evidence Log (Collected 2026-02-10)

## Environment
- Repository: `/Users/rexliu/betterspotlight`
- Host date: 2026-02-10
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
- `test-ui-sim-query-suite` passed in `38.37 sec`.
- `test-ui-sim-query-suite-stress` passed in `19.94 sec`.

### 5) Full suite verification
```bash
ctest --test-dir build-local --output-on-failure --timeout 120
```
Result:
- `100% tests passed, 0 tests failed out of 52`.
- Total runtime: `70.78 sec`.

### 6) Inventory check
```bash
ctest --test-dir build-local -N | tail -n 6
```
Result:
- Shows tests `#49` through `#52`.
- `Total Tests: 52`.

## Additional Verification Snippets
- Ensure old broken BM25 control statement is gone:
```bash
rg -n "INSERT INTO search_index\(search_index, rank\) VALUES\('fts5'" src || true
```
Result:
- No matches.

## Evidence-based Conclusions
- BM25 configuration bug is remediated and verified by integration runtime logs plus unit coverage.
- Adaptive merge debug contract mismatch is remediated and integration-verified.
- Prior timeout-prone UI-sim tests now complete under the same 45s timeout budget.
- Current source state in `build-local` meets full-suite pass gate (52/52).
