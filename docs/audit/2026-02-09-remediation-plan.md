# BetterSpotlight Remediation Plan (Prioritized)

## Objective
Convert audit findings into a release-safe backlog with concrete file targets, test coverage, and rollback strategy.

## Priority Backlog

| ID | Priority | Status | Remediation Item | File Targets | Tests / Validation | Rollback / Fallback |
| --- | --- | --- | --- | --- | --- | --- |
| R-001 | P1 | DONE | Fix BM25 FTS5 control statement (`'rank'` command) to remove SQL logic errors | `/Users/rexliu/betterspotlight/src/core/index/schema.h`, `/Users/rexliu/betterspotlight/src/core/index/sqlite_store.cpp` | `test-query-service-core-improvements` (verbose run), `test-sqlite-store` | Revert two statement changes if unexpected ranking regressions appear; default BM25 remains available |
| R-002 | P1 | DONE | Add BM25 regression test to block recurrence | `/Users/rexliu/betterspotlight/Tests/Unit/test_sqlite_store.cpp` | `test-sqlite-store` | Remove new test only if false-positive is proven and replacement coverage is introduced |
| R-003 | P2 | DONE | Align adaptive merge test contract with runtime adaptive weighting | `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp` | `test-query-service-core-improvements` | Revert to fixed-weight assertion if adaptive behavior is removed from query service |
| R-004 | P1 | DONE | Gate initial indexing on onboarding completion instead of unconditional auto-start | `/Users/rexliu/betterspotlight/src/app/service_manager.cpp`, `/Users/rexliu/betterspotlight/src/app/main.cpp`, `/Users/rexliu/betterspotlight/src/app/onboarding_controller.h`, `/Users/rexliu/betterspotlight/src/app/onboarding_controller.cpp` | Full `ctest` pass, onboarding signal wiring inspection | If onboarding flow regresses, temporarily call `triggerInitialIndexing()` immediately after `allServicesReady` |
| R-005 | P2 | DONE | Introduce explicit tray state model (`idle/indexing/error`) with queue-informed updates | `/Users/rexliu/betterspotlight/src/app/service_manager.h`, `/Users/rexliu/betterspotlight/src/app/service_manager.cpp`, `/Users/rexliu/betterspotlight/src/app/main.cpp` | Full `ctest` pass, compile/run verification | Fallback to tooltip-only status if queue polling causes instability |
| R-006 | P2 | DONE | Make tray click open Index Health directly | `/Users/rexliu/betterspotlight/src/app/status_bar_bridge.h`, `/Users/rexliu/betterspotlight/src/app/qml/Main.qml`, `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml`, `/Users/rexliu/betterspotlight/src/app/main.cpp` | UI-sim suite pass, runtime wiring review | Fallback to prior `showSearchRequested` trigger if product direction changes |
| R-007 | P2 | DONE | Add platform side-effect abstraction for settings (`launchAtLogin`, `showInDock`) with user-visible success/failure | `/Users/rexliu/betterspotlight/src/app/platform_integration.h`, `/Users/rexliu/betterspotlight/src/app/platform_integration.cpp`, `/Users/rexliu/betterspotlight/src/app/platform_integration_mac.mm`, `/Users/rexliu/betterspotlight/src/app/settings_controller.h`, `/Users/rexliu/betterspotlight/src/app/settings_controller.cpp`, `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml`, `/Users/rexliu/betterspotlight/src/app/CMakeLists.txt` | Full app build + `ctest` pass | Fallback to persistence-only behavior by routing to default platform integration and retaining status message |
| R-008 | P3 | OPEN | Update stale Sparkle gap-analysis statement in docs | `/Users/rexliu/betterspotlight/docs/milestones/m3/architecture-plan.md` | Documentation review | None; docs-only update |
| R-009 | P2 | OPEN | Implement clipboard-aware relevance signal with privacy gating and settings toggle | `/Users/rexliu/betterspotlight/src/services/query/query_service.cpp`, `/Users/rexliu/betterspotlight/src/app/settings_controller.*`, `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml` | New unit/integration tests + relevance fixture delta report | Fallback: keep clipboard feature disabled by default and compile-time guard |
| R-010 | P1 | OPEN | Run/automate 48h stress and 24h memory-drift gates for release readiness | `/Users/rexliu/betterspotlight/tests/benchmarks/stress_48h.sh`, `/Users/rexliu/betterspotlight/tests/benchmarks/memory_drift_24h.sh`, CI workflows | Published artifacts + pass/fail thresholds | Fallback: block external release and ship internal-only builds |
| R-011 | P1 | OPEN | Validate signing/notarization/stapling pipeline with Sparkle-enabled distribution artifacts | Packaging scripts / release workflow docs | Successful notarization + staple logs | Fallback: unsigned internal artifacts only |
| R-012 | P2 | OPEN | Add targeted tests for tray-state transitions and onboarding-indexing lifecycle | New tests under `/Users/rexliu/betterspotlight/Tests/Integration/` | New tests in CI, deterministic pass | Fallback: manual QA checklist until deterministic tests land |

## Explicit API / Interface Changes Landed
- `ServiceManager` now exposes tray state as a first-class interface:
  - `Q_PROPERTY(QString trayState READ trayState NOTIFY trayStateChanged)`
  - `Q_INVOKABLE void triggerInitialIndexing()`
- `OnboardingController` now emits lifecycle completion:
  - `signal onboardingCompleted()`
- `SettingsController` now exposes platform application status:
  - `platformStatusMessage`, `platformStatusKey`, `platformStatusSuccess`
- New platform abstraction layer:
  - `PlatformIntegration` with macOS implementation (`SMAppService`, `NSApplicationActivationPolicy`)

## Test Additions / Updates Landed
- Added BM25 regression unit test:
  - `testBm25WeightsCanBeApplied()` in `/Users/rexliu/betterspotlight/Tests/Unit/test_sqlite_store.cpp`
- Updated adaptive merge assertions in integration test:
  - `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp`

## Current Gate Summary (Post-remediation)
- Full suite gate: `52/52` passing in `build-local`.
- UI simulation gate: both `test-ui-sim-query-suite` and stress variant passing.
- Residual release gates are operational (stress/memory/notarization), not immediate correctness defects.
