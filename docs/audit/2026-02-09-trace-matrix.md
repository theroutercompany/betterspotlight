# BetterSpotlight Trace Matrix (Audited 2026-02-11)

## Scope
- Baseline docs:
  - `/Users/rexliu/betterspotlight/docs/foundation/architecture-overview.md`
  - `/Users/rexliu/betterspotlight/docs/milestones/acceptance-criteria.md`
  - `/Users/rexliu/betterspotlight/docs/milestones/m3/architecture-plan.md`
  - `/Users/rexliu/betterspotlight/docs/EXECUTIVE_RETRIEVAL_REMEDIATION_MEMO_2026-02-09.md`
  - `/Users/rexliu/betterspotlight/docs/operations/manual-inspection-m1-m2.md`
- Active implementation scope:
  - `/Users/rexliu/betterspotlight/src/**`
  - `/Users/rexliu/betterspotlight/Tests/**`
  - `/Users/rexliu/betterspotlight/CMakeLists.txt`
  - `/Users/rexliu/betterspotlight/tests/CMakeLists.txt` (doc path; repo folder is `Tests/`)

## Legend
- `CONFORMANT`: Requirement is implemented and evidenced.
- `EVOLVED_INTENTIONAL`: Implementation is materially evolved vs older spec wording and accepted.
- `DRIFT_DEFECT`: Behavior diverges from intended outcome and needs remediation.
- `DRIFT_DOC_STALE`: Code is coherent; docs are stale.
- `UNVERIFIED`: Not yet proven by current evidence set.

| Requirement | Source | Implementation Ref | Evidence | Status | Severity | Decision | Owner |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Global hotkey triggers search panel | acceptance-criteria M1 functional (hotkey open panel) | `/Users/rexliu/betterspotlight/src/app/main.cpp`, `/Users/rexliu/betterspotlight/src/app/qml/Main.qml` | Full suite pass includes hotkey-related tests; manual path intact | CONFORMANT | P2 | Keep current implementation | App UX |
| Hotkey conflict detection + alternatives | acceptance-criteria M3 hotkey conflict; m3 architecture 1.8 | `/Users/rexliu/betterspotlight/src/app/hotkey_manager.cpp`, `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml` | Full `ctest` pass (54/54); conflict plumbing present | CONFORMANT | P2 | Keep | App UX |
| Search actions: Open / Reveal / Copy Path | acceptance-criteria M1 keyboard/actions | `/Users/rexliu/betterspotlight/src/app/search_controller.cpp`, `/Users/rexliu/betterspotlight/src/app/qml/SearchPanel.qml` | Integration and unit suites pass; code path present | CONFORMANT | P2 | Keep | App UX |
| Status icon visible with explicit states | acceptance-criteria M1 status state | `/Users/rexliu/betterspotlight/src/app/service_manager.h`, `/Users/rexliu/betterspotlight/src/app/service_manager.cpp`, `/Users/rexliu/betterspotlight/src/app/main.cpp`, `/Users/rexliu/betterspotlight/Tests/Integration/test_app_lifecycle_states.cpp` | Added explicit `trayState` model + icon mapping and pulse; deterministic transition test now covers idle/indexing/error states | CONFORMANT | P2 | Keep | App UX |
| Status tooltip provides brief state description | acceptance-criteria M1 status tooltip | `/Users/rexliu/betterspotlight/src/app/main.cpp` | Tooltip strings now state `ready/indexing/error` | CONFORMANT | P3 | Keep | App UX |
| Clicking status icon opens Index Health | acceptance-criteria M1 status click behavior | `/Users/rexliu/betterspotlight/src/app/main.cpp`, `/Users/rexliu/betterspotlight/src/app/status_bar_bridge.h`, `/Users/rexliu/betterspotlight/src/app/qml/Main.qml`, `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml` | Tray click emits `showIndexHealthRequested` and routes to `openIndexHealth()` | CONFORMANT | P2 | Keep | App UX |
| Initial indexing starts only after onboarding completion | acceptance-criteria M2 onboarding/home map before first indexing | `/Users/rexliu/betterspotlight/src/app/service_manager.cpp`, `/Users/rexliu/betterspotlight/src/app/onboarding_controller.h`, `/Users/rexliu/betterspotlight/src/app/onboarding_controller.cpp`, `/Users/rexliu/betterspotlight/src/app/main.cpp`, `/Users/rexliu/betterspotlight/Tests/Integration/test_app_lifecycle_states.cpp` | Removed unconditional auto-start, added gated `triggerInitialIndexing()`; integration test validates readiness gate + one-shot trigger and onboarding completion persistence | CONFORMANT | P1 | Keep | App Platform |
| `.bsignore` live reload without restart | m3 architecture 1.10; acceptance M3 exclusions | `/Users/rexliu/betterspotlight/src/services/indexer/indexer_service.cpp`, `/Users/rexliu/betterspotlight/src/services/query/query_service.cpp` | QFileSystemWatcher + reload paths present in both services | CONFORMANT | P2 | Keep | Indexing + Query |
| FTS5 BM25 configuration applies without SQL logic errors | acceptance M1 FTS5 criteria; m3 relevance quality | `/Users/rexliu/betterspotlight/src/core/index/schema.h`, `/Users/rexliu/betterspotlight/src/core/index/sqlite_store.cpp`, `/Users/rexliu/betterspotlight/Tests/Unit/test_sqlite_store.cpp` | Query-service integration run logs “Applied BM25 weights”; no SQL logic error in post-remediation run | CONFORMANT | P1 | Keep fixed `rank` control statement | Core Indexing |
| BM25 persistence + apply is regression-tested | remediation requirement | `/Users/rexliu/betterspotlight/Tests/Unit/test_sqlite_store.cpp` | New `testBm25WeightsCanBeApplied()` present and passing | CONFORMANT | P1 | Keep test | Core Indexing |
| Adaptive lexical/semantic merge debug contract is test-aligned | acceptance M2 merged scoring | `/Users/rexliu/betterspotlight/src/services/query/query_service.cpp`, `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp` | Integration assertion now branches on `adaptiveMergeWeightsApplied` | CONFORMANT | P2 | Keep adaptive contract | Query Relevance |
| Query service reliability integration gate | m3 architecture 1.11; acceptance CI/testing | `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp` | `ctest -V -R ^test-query-service-core-improvements$` passes | CONFORMANT | P1 | Keep in CI gate set | Query Relevance |
| UI-sim relevance suite and stress variant | m3 relevance gate + reliability checks | `/Users/rexliu/betterspotlight/Tests/Integration/test_ui_sim_query_suite.cpp` | `test-ui-sim-query-suite` and `...-stress` both pass (32.47s / 22.20s latest run) | CONFORMANT | P1 | Keep as deterministic gate/report | Query Relevance |
| Full test inventory and suite health | acceptance CI criteria, m3 architecture 1.11 | `/Users/rexliu/betterspotlight/Tests/CMakeLists.txt` | `ctest --test-dir build-local -N` reports 54 tests; full run passes 54/54 | CONFORMANT | P1 | Keep | QA/Infra |
| Settings tab structure includes Index Health | acceptance M2 settings tabs | `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml` | Tab exists and health refresh logic present | CONFORMANT | P3 | Keep | App UX |
| Settings persistence for launch-at-login/show-in-dock includes platform side effects | acceptance M2 General tab behavior | `/Users/rexliu/betterspotlight/src/app/settings_controller.h`, `/Users/rexliu/betterspotlight/src/app/settings_controller.cpp`, `/Users/rexliu/betterspotlight/src/app/platform_integration.h`, `/Users/rexliu/betterspotlight/src/app/platform_integration.cpp`, `/Users/rexliu/betterspotlight/src/app/platform_integration_mac.mm` | Added platform adapter + success/failure feedback path | EVOLVED_INTENTIONAL | P2 | Keep abstraction; complete packaging helper for guaranteed launch-at-login success | App Platform |
| Settings platform side effects are regression-tested (success + failure, persistence + surfaced status) | execution-plan test scenario: settings integration | `/Users/rexliu/betterspotlight/Tests/Unit/test_settings_controller_platform.cpp`, `/Users/rexliu/betterspotlight/Tests/CMakeLists.txt` | `test-settings-controller-platform` validates launch-at-login/show-in-dock success/failure semantics and JSON persistence behavior | CONFORMANT | P2 | Keep this as the coverage gate for platform toggle semantics | App Platform |
| Show-in-Dock preference applies immediately at OS level | acceptance M2 General tab | `/Users/rexliu/betterspotlight/src/app/platform_integration_mac.mm` | Uses `NSApp setActivationPolicy(...)`; status surfaced to UI | CONFORMANT | P2 | Keep | App Platform |
| Launch-at-login is actionable and observable | acceptance M2 General tab | `/Users/rexliu/betterspotlight/src/app/platform_integration_mac.mm`, `/Users/rexliu/betterspotlight/src/app/settings_controller.cpp`, `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml` | Uses `SMAppService mainAppService`; emits explicit error/success message | EVOLVED_INTENTIONAL | P2 | Keep current behavior, add helper/service packaging follow-up | App Platform |
| Sparkle integration availability | acceptance M3 updates; m3 architecture 1.6 | `/Users/rexliu/betterspotlight/src/app/update_manager.mm`, `/Users/rexliu/betterspotlight/src/app/main.cpp`, `/Users/rexliu/betterspotlight/src/app/CMakeLists.txt` | Sparkle bridge and initialization present | CONFORMANT | P2 | Keep | Distribution |
| “No Sparkle integration in tree” statement | m3 architecture-plan.md line 107 | `/Users/rexliu/betterspotlight/docs/milestones/m3/architecture-plan.md` | M3 architecture doc updated to reflect implemented Sparkle bridge and remaining distribution deltas | CONFORMANT | P3 | Keep doc aligned with code and release workflow | Docs |
| Clipboard monitoring for relevance boost (not copy-path) | acceptance M3 clipboard/context awareness | `/Users/rexliu/betterspotlight/src/app/search_controller.cpp`, `/Users/rexliu/betterspotlight/src/app/settings_controller.cpp`, `/Users/rexliu/betterspotlight/src/services/query/query_service.cpp`, `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml` | New opt-in clipboard signals wired end-to-end; integration test validates re-ranking using clipboard context | CONFORMANT | P2 | Keep privacy-gated clipboard-signal path | Query Relevance |
| 48-hour crash-free stress gate | m3 architecture 1.2; acceptance M3 reliability | `/Users/rexliu/betterspotlight/Tests/benchmarks/stress_48h.sh`, `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml` | Harness now supports deterministic artifact dirs and isolated sockets; local `30s` smoke passed; full 48h run pending due to unavailable self-hosted macOS runner capacity (`2026-02-11`) | UNVERIFIED | P1 | Trigger self-hosted 48h run and publish artifacts before external release | QA/Infra |
| 24h memory drift gate (<10MB) | m3 architecture 1.3; acceptance M3 performance | `/Users/rexliu/betterspotlight/Tests/benchmarks/memory_drift_24h.sh`, `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml` | Harness now supports deterministic artifact dirs and isolated sockets; local `30s` smoke passed; full 24h run pending due to unavailable self-hosted macOS runner capacity (`2026-02-11`) | UNVERIFIED | P1 | Trigger self-hosted 24h run and publish artifacts before external release | QA/Infra |
| Notarization/distribution readiness | m3 architecture 1.12 | `/Users/rexliu/betterspotlight/scripts/release/notarize_with_sparkle.sh`, `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml`, `/Users/rexliu/betterspotlight/packaging/macos/entitlements.plist` | Sparkle-aware signing/notarization pipeline scripted and workflowed; local preflight passed with ad-hoc sign; credentialed notarization still pending | UNVERIFIED | P1 | Run credentialed workflow and attach staple logs before GA | Distribution |

## Summary
- Net status after this remediation wave:
  - `CONFORMANT`: 21
  - `EVOLVED_INTENTIONAL`: 2
  - `DRIFT_DEFECT`: 0
  - `DRIFT_DOC_STALE`: 0
  - `UNVERIFIED`: 3
- Highest-priority residuals: operational release gates (`P1` unverified: full-duration stress, full-duration memory drift, credentialed notarization).
