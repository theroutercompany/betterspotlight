# Principal Engineering Audit Report

## BetterSpotlight (Audit Date: 2026-02-10)

## Executive Verdict
- **Technical verdict:** Architecture and implementation are now broadly aligned for current milestone intent.
- **Quality verdict:** Core correctness and integration gates are green in the audited environment (`52/52` tests passed).
- **Release verdict:** **Conditional GO** for internal dogfood / release-candidate progression.
- **External release remains gated** by operational proof obligations not executed in this pass: 48h stress, 24h memory drift, and notarization/signing verification.

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

## Evidence Snapshot
- Build evidence:
  - `cmake --build build-local -j8` completed successfully for app/services/tests.
- Suite evidence:
  - `ctest --test-dir build-local --output-on-failure --timeout 120`
  - Result: **100% pass, 0 failed, 52 total**.
- Targeted integration evidence:
  - `ctest --test-dir build-local -V -R ^test-query-service-core-improvements$ --timeout 120`
  - Contains `Applied BM25 weights...` and no BM25 SQL error.
- UI-sim evidence:
  - `test-ui-sim-query-suite` passed in 38.37s.
  - `test-ui-sim-query-suite-stress` passed in 19.94s.

## Findings by Severity
- **P1 (core correctness):** Closed in this pass.
  - BM25 application failure fixed and regressed.
- **P2 (product behavior gaps):** One residual functional gap remains.
  - Clipboard-aware relevance signal path (monitor + privacy toggle + boost) not implemented yet.
- **P3 (documentation hygiene):** One stale spec statement identified.
  - M3 architecture plan still states no Sparkle integration despite current implementation.

## Required Gates Before External Release
1. Execute and pass 48-hour crash-free stress run with artifact retention.
2. Execute and pass 24-hour memory drift gate (<10MB drift target).
3. Validate notarization/signing/stapling flow with Sparkle-enabled distribution build.
4. Close clipboard-aware relevance capability or explicitly de-scope and update acceptance criteria.
5. Update stale Sparkle statement in `/Users/rexliu/betterspotlight/docs/milestones/m3/architecture-plan.md`.

## Recommendation
Proceed with internal RC promotion immediately; block public GA until operational gates and remaining P2/P3 deltas above are closed or formally accepted as scope adjustments.
