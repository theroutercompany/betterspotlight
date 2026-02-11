# Principal Engineering Audit Report

## BetterSpotlight (Audit Date: 2026-02-11)

## Executive Verdict
- **Technical verdict:** Architecture and implementation are now broadly aligned for current milestone intent.
- **Quality verdict:** Core correctness and integration gates are green in the audited environment (`54/54` tests passed).
- **Release verdict:** **Conditional GO** for internal dogfood / release-candidate progression.
- **External release remains gated** by proof obligations that require longer runtime windows or release credentials: full 48h stress, full 24h memory drift, and credentialed Sparkle notarization.
- **Operational note (2026-02-11):** full-duration stress/memory gates are currently infra-blocked due to unavailable self-hosted macOS runner capacity.

## What Was Remediated During This Audit Pass
1. **BM25 SQL logic defect fixed**
   - Root cause: incorrect FTS5 control row key caused repeated BM25 apply failures.
   - Fixes:
     - `/Users/rexliu/betterspotlight/src/core/index/schema.h:156`
     - `/Users/rexliu/betterspotlight/src/core/index/sqlite_store.cpp:1436`
   - Guardrail added:
     - `/Users/rexliu/betterspotlight/Tests/Unit/test_sqlite_store.cpp:420`

2. **Adaptive merge test contract corrected**
   - Runtime supports adaptive natural-language merge weighting; test previously assumed fixed weights.
   - Fix:
     - `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp:327`

3. **Onboarding/indexing lifecycle normalized**
   - Removed implicit auto-index start and introduced explicit onboarding-gated trigger.
   - Changes:
     - `/Users/rexliu/betterspotlight/src/app/service_manager.cpp:251`
     - `/Users/rexliu/betterspotlight/src/app/service_manager.cpp:370`
     - `/Users/rexliu/betterspotlight/src/app/onboarding_controller.h:30`
     - `/Users/rexliu/betterspotlight/src/app/onboarding_controller.cpp:172`
     - `/Users/rexliu/betterspotlight/src/app/main.cpp:243`

4. **Explicit tray-state model and stateful icon behavior introduced**
   - Added first-class tray state (`idle/indexing/error`) and queue-informed state transitions.
   - Changes:
     - `/Users/rexliu/betterspotlight/src/app/service_manager.h:21`
     - `/Users/rexliu/betterspotlight/src/app/service_manager.cpp:538`
     - `/Users/rexliu/betterspotlight/src/app/main.cpp:190`

5. **Tray click now routes directly to Index Health**
   - Wiring:
     - `/Users/rexliu/betterspotlight/src/app/status_bar_bridge.h:18`
     - `/Users/rexliu/betterspotlight/src/app/qml/Main.qml:55`
     - `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml:27`
     - `/Users/rexliu/betterspotlight/src/app/main.cpp:179`

6. **Settings platform side-effects abstracted with observable outcome**
   - Added platform adapter for `launchAtLogin` and `showInDock` and surfaced result status in UI.
   - Changes:
     - `/Users/rexliu/betterspotlight/src/app/platform_integration.h:9`
     - `/Users/rexliu/betterspotlight/src/app/platform_integration_mac.mm:21`
     - `/Users/rexliu/betterspotlight/src/app/settings_controller.h:34`
     - `/Users/rexliu/betterspotlight/src/app/settings_controller.cpp:247`
     - `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml:176`

7. **Clipboard-aware relevance signals implemented with privacy gating**
   - Added opt-in clipboard signal toggle and ephemeral signal extraction (no raw clipboard persistence).
   - Added query-context propagation + deterministic boost application.
   - Changes:
     - `/Users/rexliu/betterspotlight/src/app/settings_controller.h`
     - `/Users/rexliu/betterspotlight/src/app/settings_controller.cpp`
     - `/Users/rexliu/betterspotlight/src/app/search_controller.h`
     - `/Users/rexliu/betterspotlight/src/app/search_controller.cpp`
     - `/Users/rexliu/betterspotlight/src/services/query/query_service.cpp`
     - `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml`
     - `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp`

8. **Long-run gates and notarization pipeline operationalized**
   - Added deterministic artifact-dir support and isolated socket namespace for benchmark harnesses.
   - Added self-hosted long-run workflow and notarization verification workflow.
   - Added release script for Sparkle-aware signing/notarization/stapling.
   - Changes:
     - `/Users/rexliu/betterspotlight/src/core/ipc/service_base.cpp`
     - `/Users/rexliu/betterspotlight/Tests/benchmarks/stress_48h.sh`
     - `/Users/rexliu/betterspotlight/Tests/benchmarks/memory_drift_24h.sh`
     - `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml`
     - `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml`
     - `/Users/rexliu/betterspotlight/scripts/release/notarize_with_sparkle.sh`
     - `/Users/rexliu/betterspotlight/packaging/macos/entitlements.plist`

9. **Targeted lifecycle integration coverage added**
   - Added deterministic tests for tray state transitions and onboarding/indexing first-run gating semantics.
   - Changes:
     - `/Users/rexliu/betterspotlight/Tests/Integration/test_app_lifecycle_states.cpp`
     - `/Users/rexliu/betterspotlight/Tests/CMakeLists.txt`

10. **Settings/platform side-effect test gap closed**
   - Added deterministic unit coverage for `launchAtLogin` and `showInDock` success/failure semantics and persistence behavior.
   - Changes:
     - `/Users/rexliu/betterspotlight/Tests/Unit/test_settings_controller_platform.cpp`
     - `/Users/rexliu/betterspotlight/Tests/CMakeLists.txt`

## Evidence Snapshot
- Build evidence:
  - `cmake --build build-local -j8` completed successfully for app/services/tests.
- Suite evidence:
  - `ctest --test-dir build-local --output-on-failure --timeout 120`
  - Result: **100% pass, 0 failed, 54 total**.
- Settings/platform coverage evidence:
  - `ctest --test-dir build-local --output-on-failure -R test-settings-controller-platform --timeout 120`
  - Verifies side-effect success/failure handling and persistence semantics.
- Targeted integration evidence:
  - `ctest --test-dir build-local -V -R ^test-query-service-core-improvements$ --timeout 120`
  - Contains `Applied BM25 weights...` and no BM25 SQL error.
- UI-sim evidence:
  - `test-ui-sim-query-suite` passed in 32.47s (latest rerun).
  - `test-ui-sim-query-suite-stress` passed in 22.20s (latest rerun).
- Long-run harness smoke evidence:
  - `stress_48h.sh` smoke (`30s`) passed with isolated sockets/artifacts.
  - `memory_drift_24h.sh` smoke (`30s`) passed with isolated sockets/artifacts.
- Notarization pipeline smoke evidence:
  - `notarize_with_sparkle.sh` preflight passed in ad-hoc sign mode (`DEVELOPER_ID=-`, notarization disabled).
- Operational workflow dispatch evidence (`2026-02-10`):
  - `Long-Run Gates`: [run 21847178417](https://github.com/theroutercompany/betterspotlight/actions/runs/21847178417) (dispatched on self-hosted macOS runner; current status not re-queried in this snapshot).
  - `Notarization Verify`: [run 21847178969](https://github.com/theroutercompany/betterspotlight/actions/runs/21847178969) (dispatched on self-hosted macOS runner; current status not re-queried in this snapshot).

## Findings by Severity
- **P1 (core correctness):** Closed in this pass.
  - BM25 application failure fixed and regressed.
- **P2 (product behavior gaps):** Clipboard-aware relevance, app-lifecycle coverage, and settings/platform side-effect coverage are closed in this pass.
- **P3 (documentation hygiene):** Sparkle stale statement is closed in this pass.

## Required Gates Before External Release
1. Execute and pass full 48-hour crash-free stress run with artifact retention.
2. Execute and pass full 24-hour memory drift gate (<10MB drift target).
3. Execute credentialed notarization/signing/stapling flow with Sparkle-enabled distribution build on self-hosted runner.

## Interim Policy While Runner Is Unavailable
- Keep internal RC flow enabled.
- Keep public GA blocked.
- Reclassify the two long-duration reliability gates as `blocked by infrastructure` (not product-code failure) until runner capacity is restored.

## Recommendation
Proceed with internal RC promotion immediately; block public GA until operational gates and remaining P2/P3 deltas above are closed or formally accepted as scope adjustments.
