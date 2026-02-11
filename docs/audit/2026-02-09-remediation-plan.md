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
| R-008 | P3 | DONE | Update stale Sparkle gap-analysis statement in docs | `/Users/rexliu/betterspotlight/docs/milestones/m3/architecture-plan.md` | Documentation review | None; docs-only update |
| R-009 | P2 | DONE | Implement clipboard-aware relevance signal with privacy gating and settings toggle | `/Users/rexliu/betterspotlight/src/services/query/query_service.cpp`, `/Users/rexliu/betterspotlight/src/app/search_controller.*`, `/Users/rexliu/betterspotlight/src/app/settings_controller.*`, `/Users/rexliu/betterspotlight/src/app/qml/SettingsPanel.qml`, `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp` | Full `ctest` pass + new integration scenario asserting clipboard signal re-ranking | Fallback: disable `clipboardSignalEnabled` in settings defaults if regressions appear |
| R-010 | P1 | BLOCKED_INFRA | Run/automate 48h stress and 24h memory-drift gates for release readiness | `/Users/rexliu/betterspotlight/Tests/benchmarks/stress_48h.sh`, `/Users/rexliu/betterspotlight/Tests/benchmarks/memory_drift_24h.sh`, `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml`, `/Users/rexliu/betterspotlight/src/core/ipc/service_base.cpp` | Smoke runs passed locally (30s each) with deterministic artifact dirs; no self-hosted macOS runner capacity available as of `2026-02-11` for full-duration execution | Fallback: block external release until full-duration artifacts are attached |
| R-011 | P1 | IN_PROGRESS | Validate signing/notarization/stapling pipeline with Sparkle-enabled distribution artifacts | `/Users/rexliu/betterspotlight/scripts/release/notarize_with_sparkle.sh`, `/Users/rexliu/betterspotlight/packaging/macos/entitlements.plist`, `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml` | Local preflight signing/packaging smoke passed (ad-hoc signing, no notarization); credentialed workflow dispatched on `2026-02-10` (`run 21847178969`, status not re-queried in this snapshot) | Fallback: unsigned internal artifacts only until Apple credentialed run succeeds |
| R-012 | P2 | DONE | Add targeted tests for tray-state transitions and onboarding-indexing lifecycle | `/Users/rexliu/betterspotlight/Tests/Integration/test_app_lifecycle_states.cpp`, `/Users/rexliu/betterspotlight/Tests/CMakeLists.txt` | `test-app-lifecycle-states` added and passing; full `ctest` pass now `54/54` | Fallback: n/a (deterministic integration coverage landed) |
| R-013 | P2 | DONE | Add settings/platform side-effect integration coverage (`launchAtLogin`, `showInDock`) for success/failure persistence semantics | `/Users/rexliu/betterspotlight/Tests/Unit/test_settings_controller_platform.cpp`, `/Users/rexliu/betterspotlight/Tests/CMakeLists.txt` | `test-settings-controller-platform` passing; full `ctest` pass `54/54` | Fallback: keep current runtime behavior, but do not change settings/platform code without updating this gate |

## Explicit API / Interface Changes Landed
- `ServiceManager` now exposes tray state as a first-class interface:
  - `Q_PROPERTY(QString trayState READ trayState NOTIFY trayStateChanged)`
  - `Q_INVOKABLE void triggerInitialIndexing()`
- `OnboardingController` now emits lifecycle completion:
  - `signal onboardingCompleted()`
- `SettingsController` now exposes platform application status:
  - `platformStatusMessage`, `platformStatusKey`, `platformStatusSuccess`
- `SettingsController` now exposes clipboard privacy toggle:
  - `clipboardSignalEnabled`
- New platform abstraction layer:
  - `PlatformIntegration` with macOS implementation (`SMAppService`, `NSApplicationActivationPolicy`)
- `SearchController` now forwards opt-in clipboard path hints (`clipboardBasename`, `clipboardDirname`, `clipboardExtension`) to query context.
- `ServiceBase::socketPath(...)` now supports `BETTERSPOTLIGHT_SOCKET_DIR` override for isolated benchmark/test runs.

## Test Additions / Updates Landed
- Added BM25 regression unit test:
  - `testBm25WeightsCanBeApplied()` in `/Users/rexliu/betterspotlight/Tests/Unit/test_sqlite_store.cpp`
- Updated adaptive merge assertions in integration test:
  - `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp`
- Added clipboard context re-ranking assertions in integration test:
  - `/Users/rexliu/betterspotlight/Tests/Integration/test_query_service_core_improvements.cpp`
- Added long-run gate automation workflows:
  - `/Users/rexliu/betterspotlight/.github/workflows/long-run-gates.yml`
  - `/Users/rexliu/betterspotlight/.github/workflows/notarization-verify.yml`
- Added deterministic app lifecycle integration coverage:
  - `/Users/rexliu/betterspotlight/Tests/Integration/test_app_lifecycle_states.cpp`
  - `/Users/rexliu/betterspotlight/Tests/CMakeLists.txt`
- Added settings/platform side-effect coverage:
  - `/Users/rexliu/betterspotlight/Tests/Unit/test_settings_controller_platform.cpp`
  - `/Users/rexliu/betterspotlight/Tests/CMakeLists.txt`

## Current Gate Summary (Post-remediation)
- Full suite gate: `54/54` passing in `build-local`.
- UI simulation gate: both `test-ui-sim-query-suite` and stress variant passing.
- Long-run harness smoke runs passed locally with isolated sockets and artifact capture:
  - `stress_48h.sh` smoke (`30s`) -> pass
  - `memory_drift_24h.sh` smoke (`30s`) -> pass
- Full-duration/credentialed operational workflows dispatched on `2026-02-10`:
  - `Long-Run Gates`: `run 21847178417` (dispatched on self-hosted macOS runner; full-duration execution currently blocked by runner availability)
  - `Notarization Verify`: `run 21847178969` (dispatched on self-hosted macOS runner; status not re-queried in this snapshot)
- Residual release gates are now operationally wired but still require credentialed / full-duration evidence to close:
  - full `48h` stress artifact
  - full `24h` memory drift artifact
  - Sparkle-enabled notarized artifact + staple logs

## Interim Release Policy (While Runner Capacity Is Unavailable)
- As of `2026-02-11`, treat R-010 as `BLOCKED_INFRA`, not `FAILED`.
- Internal dogfood / RC promotion is allowed with current evidence (`54/54` tests + smoke harness passes).
- Public GA remains blocked until:
  - one uninterrupted `48h` stress artifact is captured, and
  - one uninterrupted `24h` memory drift artifact is captured.
