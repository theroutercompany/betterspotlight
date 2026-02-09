# Milestone 3 Remediation Batch: Analysis Index

**Created:** February 9, 2026  
**Principal Engineer Review Document**

This directory now contains comprehensive documentation of the Milestone 3 baseline remediation batch.

---

## Documents

### 1. **MILESTONE_3_BASELINE_QUICK_REFERENCE.md**
- **Purpose:** One-page technical cheat sheet
- **Audience:** Developers, architects
- **Content:** Constants, thresholds, decision trees, verification checklist
- **Read time:** 3-5 minutes

**Use when:** You need to quickly verify a constant, understand a threshold, or check test configuration.

### 2. **MILESTONE_3_BASELINE_REMEDIATION_ANALYSIS.md**
- **Purpose:** Comprehensive technical analysis
- **Audience:** Principal engineers, technical leads
- **Content:** 7 systems with detailed code walkthroughs, SQL queries, confidence formulas, debug field mappings
- **Read time:** 30-45 minutes

**Use when:** You need deep understanding of how a system works, exact line numbers, or implementation details.

---

## The Remediation Batch: 7 Systems

### System 1: Typo-Auto Policy
**What changed:** Bounded multi-token rewrite logic with confidence gating + lexical-strength guardrails

**Key constants:**
- `kRewriteAggregateThreshold = 0.72`
- `kRewriteCandidateThreshold = 0.66`

**File:** `src/services/query/query_service.cpp:736-914`

**Impact:** Prevents unsafe typo corrections from dominating results; ensures rewrites only applied when they improve ranking.

---

### System 2: Semantic Routing
**What changed:** Dynamic threshold/floor/cap by query type (natural language vs path/code-like)

**Key thresholds:**
- Natural Language: threshold 0.62, floor 0.08, cap min(6, limit)
- Path/Code: threshold 0.70, floor 0.15, cap min(3, limit/2)

**File:** `src/services/query/query_service.cpp:1085-1092`

**Impact:** Prevents semantic search from misinterpreting structured queries; balances coverage for NL vs safety for code.

---

### System 3: Semantic-Only Safety Rule
**What changed:** High similarity (0.78) or overlap signal required for semantic-only results

**Key constant:**
- `kSemanticOnlySafetySimilarity = 0.78`

**File:** `src/services/query/query_service.cpp:1157-1180`

**Impact:** Two-gate approval system prevents unanchored semantic results from appearing without lexical justification.

---

### System 4: Health Signal Calibration
**What changed:** Separated critical failures from expected-gap failures

**New fields in IndexHealth:**
- `criticalFailures` — blocks health
- `expectedGapFailures` — doesn't affect health

**File:** `src/core/index/sqlite_store.cpp:1289-1346`

**Impact:** Expected gaps (PDF unavailable, unsupported formats, etc.) no longer degrade perceived index health.

---

### System 5: Health Status Reasoning
**What changed:** Calibrated overall status logic with healthStatusReason field

**Status values:**
- `healthy` / `rebuilding` / `degraded`

**File:** `src/services/query/query_service.cpp:1596-1612`

**Impact:** Clients can now distinguish between "temporarily rebuilding," "indexer disconnected," and "critical failures" — enabling smarter UX.

---

### System 6: Debug Observability
**What changed:** 18+ debug fields for rewrite decisioning (when debug=true)

**Key fields:**
- Rewrite: `rewriteApplied`, `rewriteConfidence`, `rewriteReason`, `correctedTokens`
- Semantic: `semanticThresholdApplied`, `semanticOnlyFloorApplied`
- Health: `healthStatusReason`, `criticalFailures`, `expectedGapFailures`

**File:** `src/services/query/query_service.cpp:1393-1466`

**Impact:** Enables forensic debugging and A/B testing of ranking changes.

---

### System 7: Test Suite Split
**What changed:** Deterministic fixture gate vs live-index suite

**Fixture gate:**
- `Tests/Integration/test_query_service_relevance_fixture.cpp`
- Uses sealed fixture + fresh DB
- Gates at default 90% pass rate

**Live-index suite:**
- `Tests/Integration/test_ui_sim_query_suite.cpp`
- Uses live user index
- Report-only mode (doesn't fail)

**File:** `Tests/CMakeLists.txt:41, 57`

**Impact:** Early detection of regressions (fixture) + real-world validation (live) without breaking CI.

---

## How to Use This Baseline

### For Code Review
1. Check **Quick Reference** for constants and thresholds
2. Reference exact line numbers from **Analysis** document
3. Verify against test output using debug fields

### For Implementation
1. Use **Quick Reference** verification checklist
2. Check test infrastructure in `Tests/CMakeLists.txt`
3. Understand impact of changes using **Analysis** system descriptions

### For Debugging
1. Enable `debug=true` in search request
2. Inspect debugInfo fields against the 18+ field list
3. Correlate with source code line ranges in **Analysis**

### For Milestone 3 Planning
1. Review "7 Systems" summary above
2. Use each system's "Impact" to understand downstream dependencies
3. Plan work waves that don't violate the invariants documented in **Analysis**

---

## Key Commits

| Hash | Message | Date |
|------|---------|------|
| `0cede3e` | Implement retrieval remediation: gate split, typo/semantic tuning, and health calibration | Feb 9, 2026 |
| `be81d40` | Merge pull request #1 from theroutercompany/codex/retrieval-remediation-20260209 | Feb 9, 2026 |

---

## Next Steps for Milestone 3

With the remediation batch established as baseline:

1. **Verify all constants** match Quick Reference values
2. **Run fixture gate** to confirm 90%+ pass rate
3. **Enable debug output** on sample queries to validate observability
4. **Plan Milestone 3 work** knowing these systems are locked at this revision
5. **Document any deviations** if requirements change

---

## Questions?

Refer to the appropriate document:
- **"What's the threshold for X?"** → Quick Reference
- **"How does the confidence calculation work?"** → Analysis Section 1
- **"What are the expected gap failure patterns?"** → Analysis Section 4
- **"Where do I find the test configuration?"** → Quick Reference, Key Files table

---

**Document Version:** 1.0  
**Last Updated:** February 9, 2026  
**Status:** Baseline for Milestone 3
