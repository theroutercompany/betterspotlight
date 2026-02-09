# Milestone 3 Baseline: Quick Reference

**Commit:** `0cede3e` (Feb 9, 2026)  
**Purpose:** Establish correct baseline for Milestone 3 development

---

## 1. Typo-Auto Policy (Lines 736-914 in query_service.cpp)

**Constants:**
- `kRewriteAggregateThreshold = 0.72`
- `kRewriteCandidateThreshold = 0.66`

**Confidence formula:** Base 0.50 + bonuses (editDistance, docCount, first letter, size) → clamped [0.0, 1.0]

**Multi-token limits:**
- Relaxed: 2 max
- Auto weak: 2 max
- Auto strong: 1 max

**Guardrail:** Lexical strength (BM25 score) must not degrade after rewrite

**Debug fields:** `rewriteApplied`, `rewriteConfidence`, `rewriteReason`, `correctedTokens`

---

## 2. Semantic Routing (Lines 1085-1212 in query_service.cpp)

**Query type detection:**
- **Natural Language:** 3+ signal tokens → threshold 0.62, floor 0.08, cap min(6, limit)
- **Path/Code:** Contains `/\~.::` or code chars → threshold 0.70, floor 0.15, cap min(3, limit/2)

**Safety rule:** Semantic-only results require:
1. Similarity ≥ 0.78 (auto-approve), OR
2. Query token appears in result path/filename (overlap gate)

**Constant:** `kSemanticOnlySafetySimilarity = 0.78`

**Debug fields:** `semanticThresholdApplied`, `semanticOnlyFloorApplied`, `semanticOnlyCapApplied`

---

## 3. Health Signal Calibration (SQLite: Lines 1289-1346)

**IndexHealth fields (new post-remediation):**
- `criticalFailures` — blocks health, triggers "degraded" status
- `expectedGapFailures` — does NOT affect health (non-blocking)

**Expected gap patterns (extraction):**
- PDF/OCR unavailable, unsupported ext, file size limit, cloud placeholder, encrypted PDF, file not readable

**Critical failures:** Everything else (metadata extraction, FTS indexing, chunking, write failures)

**Determination:** `isHealthy = (criticalFailures == 0)`

---

## 4. Health Status Reasoning (Lines 1596-1612 in query_service.cpp)

**Decision tree:**
1. Vector rebuild in progress OR no items → "rebuilding"
2. Indexer RPC unavailable → "degraded" / "indexer_unavailable"
3. `criticalFailures > 0` → "degraded" / "degraded_critical_failures"
4. Otherwise → "healthy" / "healthy"

**Response fields:** `overallStatus`, `healthStatusReason`, `criticalFailures`, `expectedGapFailures`

---

## 5. Debug Observability (Lines 1393-1466 in query_service.cpp)

**Conditional:** Enabled when client passes `debug=true`

**18+ fields in debugInfo:**
- Query processing: `queryMode`, `queryAfterParse`, `parsedTypes`, `plannerApplied`, `plannerReason`
- Typo system: `rewriteApplied`, `rewriteConfidence`, `rewriteReason`, `correctedTokens`, `rewriteCandidatesConsidered`, `rewriteMinCandidateConfidence`, `rewrittenQuery`
- Semantic: `semanticThresholdApplied`, `semanticOnlyFloorApplied`, `semanticOnlyCapApplied`, `semanticCandidates`, `fusionMode`
- Lexical: `lexicalStrictHits`, `lexicalRelaxedHits`
- Filters: `filters` (object with includePaths, excludePaths, fileTypes, etc)

---

## 6. Test Suite Split

**Fixture gate (deterministic):**
- File: `Tests/Integration/test_query_service_relevance_fixture.cpp`
- Uses: Sealed fixture files + fresh SQLite DB
- Gate: Default 90% pass rate on baselines.json cases
- Purpose: Catch ranking regressions early

**Live-index suite (observational):**
- File: `Tests/Integration/test_ui_sim_query_suite.cpp`
- Uses: Live `$HOME/Library/...` index (actual user data)
- Mode: Report-only (doesn't fail)
- Purpose: Real-world performance validation

---

## 7. Verification Checklist

- [ ] Constants: `kRewriteAggregateThreshold=0.72`, `kRewriteCandidateThreshold=0.66`
- [ ] Thresholds: NL=0.62/0.70 code, floor NL=0.08/0.15 code
- [ ] Safety constant: `kSemanticOnlySafetySimilarity=0.78`
- [ ] Health struct: `criticalFailures`, `expectedGapFailures` fields present
- [ ] Health logic: `isHealthy = (criticalFailures == 0)`
- [ ] Status: overallStatus in {healthy, rebuilding, degraded}
- [ ] Fixture test: Baseline gate at default 90%
- [ ] Debug fields: All 18+ populated when debug=true

---

## Key Files

| File | Purpose | Key Lines |
|------|---------|-----------|
| `src/services/query/query_service.cpp` | Typo, semantic, health, debug | 91-1466 |
| `src/core/index/sqlite_store.cpp` | Health calculation | 1289-1346 |
| `src/core/shared/index_health.h` | Health struct def | All |
| `Tests/Integration/test_query_service_relevance_fixture.cpp` | Fixture gate | Full |
| `Tests/CMakeLists.txt` | Test split config | Lines 41, 57 |

