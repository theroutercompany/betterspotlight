# Milestone 3 Architecture Plan (Production Guidance)

**Project:** BetterSpotlight (macOS offline-first file search, Qt6/C++)

**Baseline:** Post-remediation commit `0cede3e` (per team analysis)

**Primary goal (M3):** Daily-driver readiness with **95%+ relevance on a 100-query corpus**, **zero crashes in 48h stress**, **<10MB RSS drift over 24h idle**, expanded Index Health, user-facing error messaging, Sparkle updates, clipboard/context awareness, hotkey conflict detection, result grouping, live `.bsignore` reload, CI green, notarization.

This plan is intentionally code-aware and biased toward minimal change that improves reliability without adding new ML models.

---

## 1) Gap Analysis

Acceptance criteria reference: `docs/milestones/acceptance-criteria.md` (M3 section).

### 1.1 Relevance: >= 95% on 100-query corpus

**Current state**

- Deterministic fixture relevance gate exists and blocks CI: `Tests/Integration/test_query_service_relevance_fixture.cpp` reading `Tests/relevance/baselines.json`.
- Current deterministic gate is **90%** over **11 evaluated cases** (12 total minus `typo_strict` skip): `Tests/relevance/baselines.json`.
- Live-index suite exists but is report-only / skippable: `Tests/Integration/test_ui_sim_query_suite.cpp` reading `Tests/relevance/ui_sim_query_suite.json`.
- Search pipeline and ranking are implemented in `src/services/query/query_service.cpp` and `src/core/ranking/scorer.cpp`.

**Delta required**

- Replace "11-case deterministic" gate with a **100-query deterministic corpus** (fixture-backed), composition aligned with M3 criteria.
- Ensure deterministic corpus **exercises semantic** (currently the fixture gate is lexical-only unless the test builds vectors).
- Update gating policy without destabilizing CI (see Section 6: staged promotion).

### 1.2 Zero crashes in 48-hour stress test

**Current state**

- Supervisor restart/crash accounting exists: `src/core/ipc/supervisor.cpp`, surfaced to app: `src/app/service_manager.cpp`.
- Stress scripts exist but are not production-grade for M3:
  - `Tests/benchmarks/stress_8h.sh` currently checks `--version` and samples the script's RSS (not the app/services), so it cannot prove "zero crashes" nor "no silent failures".

**Delta required**

- A real stress harness that:
  - Runs the app and services end-to-end.
  - Drives workload (search bursts, idle periods, index mutations, settings changes, rebuilds).
  - Captures crash evidence (process exits, Supervisor crash counters, OS crash logs if present).
  - Verifies DB integrity periodically.
- CI integration: run this as a **nightly / scheduled** job (48h is not PR-friendly) with artifacts.

### 1.3 Memory leak test: <10MB drift over 24h idle

**Current state**

- Memory benchmark script exists: `Tests/benchmarks/benchmark_memory.sh` (short duration sampling).
- `handleGetHealth` reports coverage and queue stats but not process memory: `src/services/query/query_service.cpp` (`QueryService::handleGetHealth`).

**Delta required**

- Add a 24h "idle drift" runner that samples RSS for:
  - UI process
  - Indexer service
  - Query service
  - Extractor service
- Add lightweight per-process memory metrics accessible in Health UI (see 1.4).

### 1.4 Expanded Index Health dashboard

**Current state**

- Health endpoint exists: `QueryService::handleGetHealth` in `src/services/query/query_service.cpp`.
- App already calls this and flattens nested fields for QML: `SearchController::getHealthSync()` in `src/app/search_controller.cpp`.
- Current health response includes:
  - Overall status + reason
  - Indexed item/chunk counts
  - Embedded vector counts and vector rebuild state
  - FTS/vector sizes
  - Queue stats (query service calls indexer RPC internally and app merges too)
  - Recent (filtered) failures

**Delta required** (per M3 criteria)

- Add missing dashboard capabilities:
  - Memory usage: app + indexer processes.
  - Failure list with error reason + "show more details" (pagination, filtering expected vs critical).
  - "Reindex folder" and "Rebuild all" actions surfaced in UI.
    - Indexer endpoints already exist: `IndexerService::handleReindexPath` and `IndexerService::handleRebuildAll` in `src/services/indexer/indexer_service.cpp`.
- Provide "exclusions summary" (built-ins + `.bsignore` + settings-driven user patterns) and last reload timestamp.

### 1.5 Error messaging (user-facing)

**Current state**

- IPC error codes exist: `src/core/shared/ipc_messages.h`.
- Services return `IpcMessage::makeError(...)` with a message string: `src/core/ipc/message.cpp`.
- Many surfaced errors are raw strings ("Database is not available", SQLite/open errors, extractor failures in health list).

**Delta required**

- A consistent "internal error" vs "user-facing message" pipeline:
  - Internal: structured code + debug detail for logs.
  - User-facing: short, actionable, localized-ready message in UI.
- Health dashboard must show actionable recovery steps (FDA revoked, disk full, corrupted index -> rebuild).

### 1.6 Sparkle auto-update integration

**Current state**

- Sparkle bridge is implemented in app layer:
  - `src/app/update_manager.mm`
  - `src/app/main.cpp` wiring and status reporting
  - `src/app/CMakeLists.txt` Sparkle linkage behind `BETTERSPOTLIGHT_ENABLE_SPARKLE`
- Settings toggle for update checks is implemented via `SettingsController::checkForUpdates`.
- Distribution notarization/signing was not previously wired into repo automation.

**Delta required**

- Verify Sparkle-enabled artifacts in CI/release automation and enforce signing/notarization/stapling gates.
- Define update channels (stable/beta optional), appcast signing keys, and release promotion criteria.

### 1.7 Clipboard/context awareness

**Current state**

- Context signals exist in ranking: `src/core/ranking/scorer.cpp` uses `QueryContext.cwdPath` and `QueryContext.frontmostAppBundleId`.
- Clipboard signal monitoring is now opt-in via settings:
  - `SettingsController::clipboardSignalEnabled`
  - Privacy tab toggle in `src/app/qml/SettingsPanel.qml`
- Search controller forwards ephemeral clipboard-derived path hints (`basename`, `dirname`, `extension`) in query context.
- QueryService applies deterministic boost based on clipboard hints without persisting raw clipboard text.

**Delta required**

- Expand automated coverage with dedicated UI-level tests for toggle behavior and signal freshness.
- Validate signal weights against the 100-query relevance corpus to avoid regressions.

### 1.8 Hotkey conflict detection

**Current state**

- Global hotkey registration exists via Carbon: `src/app/hotkey_manager.cpp`.
- `main.cpp` prints a warning on registration failure; no user prompt.

**Delta required**

- Detect conflict and prompt user to pick an alternative hotkey.
- Provide a "hotkey health" status field for Health/Settings to reduce support load.

### 1.9 Result grouping

**Current state**

- Search UI is a flat `ListView`: `src/app/qml/SearchPanel.qml` using `src/app/qml/ResultItem.qml`.
- Results already carry `kind`, `matchType`, frequency, and availability fields from QueryService.

**Delta required**

- Implement "grouped list model" (headers + items) while preserving keyboard navigation and actions.
- Grouping required by criteria: Files, Folders, Recently opened (optional).

### 1.10 `.bsignore` live reload (no restart)

**Current state**

- `.bsignore` is loaded when indexing starts: `IndexerService::handleStartIndexing` calls `m_pathRules.loadBsignore(~/.bsignore)`.
- `PathRules::loadBsignore` exists: `src/core/fs/path_rules.cpp`.
- No watcher to reload `.bsignore` while running.
- Query path filtering does not consult `.bsignore`; already-indexed items would still appear until reindex/purge.

**Delta required**

- Implement `.bsignore` reload on change and apply it:
  - At **query-time filtering** (so excluded items stop showing "on next search").
  - In indexing pipeline rules for future indexing.
  - Optionally enqueue purge/reindex for excluded paths for correctness, but UI-visible exclusion must happen immediately.

### 1.11 All unit and integration tests pass in CI

**Current state**

- Existing unit/integration suites are present and currently green per baseline.
- Deterministic relevance gate blocks when below threshold: `Tests/Integration/test_query_service_relevance_fixture.cpp`.

**Delta required**

- Expand test corpus and add new tests (grouping model, error formatter, hotkey conflict prompt behavior, `.bsignore` reload) without destabilizing CI.

### 1.12 App notarized and ready for distribution

**Current state**

- Notarization/signing guidance exists: `docs/foundation/build-system.md` (codesign, notarytool, entitlements).
- Packaging artifacts/automation may not be wired end-to-end in CI for M3.

**Delta required**

- Finalize packaging pipeline, verify entitlements, sign all embedded frameworks (including Sparkle), notarize DMG, and staple.

---

## 2) Ranking Architecture Expansion Strategy (88.37% -> 95% without new models)

### 2.1 Where the misses are (based on baseline)

Observed weak categories (baseline analysis):

- `typo_auto`: 80% pass.
- `semantic_probe`: 62.5% pass.

These map directly onto the current pipeline in `src/services/query/query_service.cpp`:

- Typo rewrite decision logic: `typoCandidateConfidence(...)` and `buildTypoRewriteDecision(...)`.
- Semantic retrieval + merge:
  - Thresholding: `semanticThreshold`, `semanticOnlyFloor`, `semanticOnlyCap`, `kSemanticOnlySafetySimilarity`.
  - Fusion: `SearchMerger::merge(...)` with `MergeConfig` defaults in `src/core/vector/search_merger.h`.

### 2.2 Principle: keep the pipeline, make the signals more explicit

Current pipeline effectively computes multiple unrelated "scores" in different phases:

1. Lexical candidate generation via FTS5 (strict/relaxed + typo rewrite).
2. M1 scoring (match type + recency/frequency/context/pinned/junk): `Scorer::rankResults`.
3. Semantic merge, which **overwrites** `SearchResult.score` with a rank-fusion score: `SearchMerger::merge`.
4. M2 boost pass that adds ad hoc boosts/penalties and re-sorts.

For M3, we want higher relevance without a rewrite. The minimal architecture expansion is:

- Preserve "what" each phase is responsible for.
- Make fusion and semantic contribution visible and testable.
- Add **query-type adaptive knobs** that are driven by classification already present (path/code-like vs NL-like).

### 2.3 Targeted improvements

#### 2.3.1 Typo-auto: increase recall without unsafe rewrites

Current behavior (Auto mode):

- Only consider tokens of length >= 4, not in stopwords, not already in lexicon.
- Distance-2 only when token length >= 8 and docCount >= 5.
- Rewrite applied only when aggregate confidence >= 0.72 and rewritten BM25 is not weaker.
- In Auto mode: allow 2 replacements only when strict is weak/empty (`bestLexicalStrength(strictHits) < 2.0`).

Issues to address

- Long multi-token queries can have 1-2 typos that block strict FTS and still fail rewrite gating.
- Short-but-important tokens (e.g., `jwt`, `kub`, `nix`) are currently skipped because length < 4.
- Distance-2 rule is too conservative for certain high-signal tokens, but distance-2 is also the primary source of unsafe rewrites.

Concrete changes (recommended)

1. **Token eligibility improvements** in `buildTypoRewriteDecision(...)` (`src/services/query/query_service.cpp`):
   - Allow length==3 tokens when they are ALLCAPS or alnum-heavy and not stopwords (e.g., `JWT`, `SQL`).
   - Don't attempt rewrite for tokens recognized as file type hints by `QueryParser` (already extracted in `parsed.extractedTypes`).
2. **Contextual rewrite budget**:
   - Replace "strictWeakOrEmpty ? 2 : 1" with a budget derived from query length and token signal count.
   - Example: 3+ signal tokens and strictWeak => allow 2; 5+ signal tokens => allow 2 even if strict has hits but weak lexical strength.
3. **Candidate scoring improvements** in `typoCandidateConfidence(...)`:
   - Add a penalty for corrections that change token prefix beyond first char (reduces wild rewrites).
   - Add a boost for corrections that produce a token that appears in multiple directory names (file-system-specific signal; can be derived from lexicon docCount buckets).
4. **Safety guardrail remains**:
   - Keep "rewrite only if rewritten BM25 >= original" as the final gate.
   - Add instrumentation output in debugInfo (already returns rewriteApplied/confidence/reason) so failures are diagnosable.

Expected impact

- Improves typo_auto recall by making rewrite more likely when it's safe, while preserving the existing lexical-strength guardrail.

#### 2.3.2 Semantic-probe: allow semantic-only results when semantics are actually needed

Current semantic-only safety (Auto):

- Semantic-only results are allowed when either:
  - similarity >= 0.78 (`kSemanticOnlySafetySimilarity`), or
  - there is token overlap between query signal tokens and result name/parent directory tokens.

This can block legitimate "concept-only" searches (e.g., `settings` -> `config.ini`) because there is no lexical overlap and similarity may be < 0.78 even when it's the best semantic match.

Concrete changes (recommended)

1. **Make merge weights query-adaptive**:
   - In `QueryService` search handler (`src/services/query/query_service.cpp`), set `MergeConfig.lexicalWeight` and `MergeConfig.semanticWeight` based on query class:
     - NL-like queries: 0.55/0.45 (or 0.5/0.5 if strict lexical is weak).
     - Path/code-like queries: 0.7/0.3.
   - Keep `rrfK=60` initially to avoid destabilizing fused ranks.
2. **Relax semantic-only admission when strict lexical is weak**:
   - If `strictWeakOrEmpty` is true (already computed earlier) AND query is NL-like:
     - lower `kSemanticOnlySafetySimilarity` to 0.74-0.76 for this case only.
     - keep `semanticOnlyCap` to avoid flooding.
3. **Use semantic similarity as a real scoring signal** (minimal change):
   - Extend `SearchResult` (shared model) to carry `semanticSimilarity` (float, optional) and/or `semanticNormalized`.
   - During merge, set this field for items present in semanticResults.
   - In the M2 boost loop, add a small boost proportional to normalized semantic similarity for NL-like queries:
     - e.g., `m2Boost += 18.0 * normalizedSemantic` when semantic-only OR when lexical overlap is low.
   - This avoids a full scoring rewrite while making semantic-probe wins more consistent.
4. **Instrument semantic gating**:
   - Add debugInfo fields for: semanticOnlySuppressedCount, semanticOnlyAllowedBy (similarity/overlap), and effective merge weights.

Expected impact

- Increases semantic-probe pass rate by removing an overly strict "0 overlap implies must be >= 0.78" rule in exactly the situations where lexical is known to be weak.

### 2.4 Query understanding improvements (no new models)

Recommended improvements are low-risk and leverage existing signals:

- Expand NL vs path/code classifier:
  - Today: `looksLikePathOrCodeQuery(...)` and `looksLikeNaturalLanguageQuery(...)`.
  - Add a third class: "short ambiguous" (1-2 tokens) and treat it as lexical-first, semantic-light.
- File-type hint normalization already exists (`normalizeFileTypeToken`). Extend hints to cover a few common aliases (`jpg`/`jpeg`, `md`/`markdown`) in `QueryParser` to reduce false misses.
- Make consumer prefilter opt-out for power users (config), but keep default (it helps noise suppression) and instrument how often it triggers (`plannerApplied`, `plannerReason` already reported in debugInfo).

### 2.5 Test corpus expansion strategy (43 -> 100 queries)

We already have two corpus representations:

- Fixture-backed corpus in `Tests/relevance/test_corpus.json` (query -> expected_files + pass_rank).
- Deterministic IPC baselines in `Tests/relevance/baselines.json` (query -> expectedFileName + topN).

Recommendation: make `Tests/relevance/test_corpus.json` the "source of truth" and generate the deterministic baselines from it (or update the integration test to read it directly).

Corpus composition for M3 (from `docs/milestones/acceptance-criteria.md`):

- 20 exact filename
- 20 partial filename
- 25 content-based
- 20 semantic/conceptual
- 15 edge cases (unicode, accents, CJK, special chars)

For each query, store:

- expected file(s)
- pass rank (top 3)
- category
- rationale
- whether it is semantic-required (used to decide whether semantic-only admission rules are allowed)

---

## 3) Work Breakdown (Phased Waves)

Sizing is relative (S/M/L). Risk is about regressions and integration complexity.

### Wave A (Quality Core): 100-query corpus + ranking knobs

1. **Promote 100-query deterministic corpus (fixture-backed)**
   - Files: `Tests/relevance/test_corpus.json`, `Tests/relevance/baselines.json`, `Tests/Integration/test_query_service_relevance_fixture.cpp`
   - Complexity: M
   - Dependencies: none
   - Risk: Medium (gate instability if promoted too early)

2. **Typo-auto improvements (safe recall)**
   - Files: `src/services/query/query_service.cpp` (`typoCandidateConfidence`, `buildTypoRewriteDecision`)
   - Complexity: M
   - Dependencies: 1
   - Risk: Medium (rewrite regressions; mitigated by debug instrumentation + gate)

3. **Semantic-probe improvements (adaptive weights + admission)**
   - Files: `src/services/query/query_service.cpp`, `src/core/vector/search_merger.h`, `src/core/vector/search_merger.cpp`, `src/core/shared/search_result.h`
   - Complexity: M
   - Dependencies: 1
   - Risk: Medium (semantic-only safety changes)

4. **Ranking observability (debug + health counters)**
   - Files: `src/services/query/query_service.cpp` (debugInfo additions), `src/services/query/query_service.h`
   - Complexity: S
   - Dependencies: 2, 3
   - Risk: Low

### Wave B (Polish + UX correctness): grouping, hotkey conflict, `.bsignore` reload

5. **Result grouping model in UI**
   - Files: `src/app/search_controller.cpp`, `src/app/search_controller.h`, `src/app/qml/SearchPanel.qml`, `src/app/qml/ResultItem.qml`
   - Complexity: M
   - Dependencies: none
   - Risk: Medium (keyboard navigation edge cases)

6. **Hotkey conflict detection + user prompt**
   - Files: `src/app/hotkey_manager.cpp`, `src/app/main.cpp`, `src/app/qml/components/HotkeyRecorder.qml`, `src/app/qml/SettingsPanel.qml`
   - Complexity: M
   - Dependencies: none
   - Risk: Medium (macOS input permission edge cases)

7. **`.bsignore` live reload + query-time filtering**
   - Files: `src/services/indexer/indexer_service.cpp`, `src/services/query/query_service.cpp`, `src/core/fs/path_rules.{h,cpp}`, `src/core/fs/bsignore_parser.{h,cpp}`
   - Complexity: L
   - Dependencies: none
   - Risk: High (privacy-sensitive; must be correct and fast)

### Wave C (Reliability): stress + memory drift + health expansion

8. **48-hour stress framework (service-driven in CI, UI-driven manually)**
   - Files: `Tests/benchmarks/stress_48h.sh` (new), `src/core/ipc/supervisor.{h,cpp}`, `src/app/service_manager.{h,cpp}`
   - Complexity: L
   - Dependencies: none
   - Risk: Medium (flaky automation if too UI-dependent)

9. **24-hour idle memory drift runner + Health memory metrics**
   - Files: `Tests/benchmarks/memory_drift_24h.sh` (new), `src/services/*` (new `getProcessStats`), `src/services/query/query_service.cpp` (health fields)
   - Complexity: M
   - Dependencies: 8
   - Risk: Medium (measurement noise; define tolerance)

10. **Expanded Health dashboard UI + failure viewer + reindex/rebuild wiring**
   - Files: `src/app/qml/SettingsPanel.qml`, `src/app/search_controller.cpp`, `src/services/query/query_service.cpp`, `src/services/indexer/indexer_service.cpp`
   - Complexity: L
   - Dependencies: 9
   - Risk: Medium

### Wave D (Distribution): Sparkle + notarization

11. **Sparkle 2 integration via Objective-C++ bridge**
   - Files: `src/app/CMakeLists.txt`, `src/app/update_manager.mm` (new), `src/app/update_manager.h` (new), `src/app/settings_controller.cpp`, `src/app/qml/SettingsPanel.qml`
   - Complexity: L
   - Dependencies: 12 (notarization constraints) partially
   - Risk: High (signing, entitlements, framework bundling)

12. **Notarization pipeline hardened for release artifacts**
   - Files: `docs/foundation/build-system.md` (verify/align), `src/app/Info.plist`, packaging scripts (if present)
   - Complexity: M
   - Dependencies: 11
   - Risk: Medium (CI secrets + Apple tooling)

---

## 4) Technical Recommendations

### 4.1 48-hour stress test framework

Recommended architecture: two layers.

1. **CI-grade service stress (deterministic, no UI dependencies)**

- Implement a driver that:
  - Launches the app (or services) and waits for Supervisor readiness.
  - Sends IPC requests to Query/Indexer repeatedly (search bursts + rebuilds + reindexPath).
  - Mutates a small temp fixture directory (create/modify/delete/move) and verifies search catches up.
  - Runs periodic `PRAGMA integrity_check` against the active DB.
- Place in `Tests/benchmarks/stress_48h.sh` (and a shorter `stress_1h.sh` for PR runs).

2. **Manual UI stress (acceptance evidence)**

- Keep a documented runbook (not necessarily fully automated):
  - Hotkey spamming
  - Navigation + open/reveal/copy actions
  - Settings changes while indexing
- Store output logs and crash evidence for sign-off.

Key instrumentation hooks

- Supervisor crash counters already exist and should be exported to logs at end of run.
- QueryService should expose a "stats snapshot" endpoint (latency histogram, rewrite counts, semantic admission counts) for postmortem.

### 4.2 Sparkle integration approach

Recommendation: Sparkle 2 embedded framework + Objective-C++ wrapper.

- Add a small `UpdateManager` (Objective-C++ `.mm`) that:
  - Creates Sparkle updater controller.
  - Runs update check on startup (if `checkForUpdates` enabled).
  - Exposes Q_INVOKABLE methods to QML: `checkNow()`, `setAutomaticallyChecks(bool)`, `lastUpdateCheckStatus()`.
- Integrate with existing settings: `SettingsController::checkForUpdates` (`src/app/settings_controller.cpp`).
- Packaging constraints:
  - Sparkle framework must be code-signed and compatible with Hardened Runtime.
  - Ensure notarization includes the embedded framework.

### 4.3 Clipboard/context awareness

Recommendation: add a privacy-preserving signal that boosts "clipboard-related paths" without storing clipboard content.

- App layer: introduce a `ClipboardMonitor` using `QClipboard::changed`.
- Extract only path-like / filename-like tokens (e.g., when clipboard contains `/Users/.../foo.txt`).
- Store ephemeral state in memory (no disk persistence, no logs).
- Pass a summarized context field with each query (e.g., "clipboardPathBasename=foo.txt", "clipboardPathDir=/Users/.../Projects").
- QueryService: apply a deterministic boost in the M2 boost loop when result.path matches the clipboard-derived path or basename.

### 4.4 Health dashboard expansion

Recommendation: extend health via new endpoints instead of overloading `getHealth`.

- Keep `getHealth` as quick summary.
- Add `getHealthDetails` returning:
  - process RSS/CPU for each service (self-reported)
  - failures with pagination and classification (expected vs critical)
  - `.bsignore` status: lastLoadedAt, patternCount
  - query stats: recent rewrite count, semantic-only admitted/suppressed

UI wiring

- `SearchController::getHealthSync()` already fetches and flattens health. Extend it to call new endpoint and populate richer UI.
- Use existing indexer actions: `reindexPath`, `rebuildAll` through `ServiceManager`.

### 4.5 Error messaging architecture

Recommendation: "structured error + user-facing formatter".

- Extend IPC error payloads to include:
  - `code` (existing)
  - `message` (internal)
  - `userMessage` (new, optional)
  - `recoveryAction` (new, optional enum/string, e.g., `open_full_disk_access_settings`, `rebuild_index`)
- Implement a single formatter in app layer that maps `IpcErrorCode` and known failure patterns to user text.
- Ensure Health dashboard uses the same formatter.

### 4.6 Result grouping design

Recommendation: group in the app layer first (lowest risk) and keep QueryService API stable.

- Add a grouped list representation in `SearchController`:
  - Convert flat results into a list of "rows", each row is either a header or a result.
  - Example headers: `Folders`, `Files`, `Recently opened`.
- Update keyboard navigation to skip headers.
- Keep "open/reveal/copy" actions working only for result rows.

---

## 5) Risk Assessment (Top Risks + Mitigations)

1. **Semantic-only admission changes increase junk**
   - Mitigation: only relax when strict lexical is weak AND query is NL-like; cap semantic-only results; add debug counters and corpus coverage.

2. **`.bsignore` live reload privacy expectations**
   - Mitigation: implement query-time filtering first (immediate effect), then background purge/reindex; add tests that verify excluded items never appear after reload.

3. **Sparkle integration breaks signing/notarization**
   - Mitigation: integrate Sparkle early in a feature branch; validate codesign/notarytool workflow per `docs/foundation/build-system.md` before merging.

4. **48h stress test flakiness in CI**
   - Mitigation: make CI stress service-driven and scheduled nightly; keep PR-level short stress (10-30 min) deterministic.

5. **Relevance gate churn blocks development**
   - Mitigation: staged promotion: 100-query corpus starts report-only, then becomes blocking once >=95% is stable.

---

## 6) Test Strategy (100-query corpus, gates, CI stability)

### 6.1 Corpus architecture

Single source of truth: `tests/relevance/baselines.json`.

- Corpus size: 100 deterministic cases.
- Composition: `exact 20`, `typo_auto 20`, `typo_strict 10`, `phrase 15`, `semantic_probe 20`, `noise 15`.
- Case metadata: `semanticRequired`, `requiresVectors`, and `notes`.

### 6.2 Gate policy (retain deterministic blocker, live report-only)

Keep:

- Deterministic fixture relevance as **blocking**.
- Live suite as **report-only** (do not block CI).

Promotion plan

1. Keep deterministic gate at `gatePassRate=90.0` while expanding/tuning.
2. Require 3 consecutive clean runs of `test-query-service-relevance-fixture` on the 100-case corpus.
3. Promote `gatePassRate` to `95.0` in `tests/relevance/baselines.json`.

### 6.3 Semantic coverage in deterministic fixture

M3 acceptance explicitly requires semantic/conceptual queries. Deterministic gate must therefore include semantic assets.

Recommendation:

- Ensure CI has model assets available to `betterspotlight-query`.
- In fixture test (`tests/Integration/test_query_service_relevance_fixture.cpp`):
  - Trigger `rebuildVectorIndex` on fixture corpus root.
  - Poll `getHealth` for `vectorRebuildStatus`.
  - Classify vector-unavailable cases explicitly as `semantic_unavailable`.

### 6.4 CI stability

- Keep deterministic fixture isolated using temporary HOME (already done).
- Avoid UI-driven automation in PR CI.
- Run scheduled jobs:
  - nightly stress (48h)
  - nightly memory drift (24h)
- Always store artifacts: relevance reports, stress logs, integrity-check logs.

### 6.5 Wave A promotion checklist

1. `ctest --output-on-failure` passes fully.
2. `test-query-service-relevance-fixture` has:
   - `invalid_fixture_case = 0`
   - `semanticUnavailableCount = 0` in CI-capable environments
   - pass rate `>= 90.0` (transitional), then `>= 95.0` after promotion.
3. `test-ui-sim-query-suite` remains `report_only` and reports `fixture_mismatch` separately from `ranking_miss`.
4. Query latency benchmark remains below p95 200 ms on `tests/benchmarks/benchmark_search.sh`.

---

### Optional future considerations (not required for M3)

- Add a dedicated "final scoring" layer that unifies lexical, semantic similarity, and boosts into one monotonic score (reduces phase coupling).
- Add per-directory `.bsignore` support (M3 only requires `~/.bsignore`).
