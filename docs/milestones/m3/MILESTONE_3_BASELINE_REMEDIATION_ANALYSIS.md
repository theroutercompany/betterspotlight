# BetterSpotlight Remediation Batch Analysis: Milestone 3 Baseline

**Principal Engineer Review**  
**Date:** February 9, 2026  
**Baseline Commit:** `0cede3e` - Implement retrieval remediation: gate split, typo/semantic tuning, and health calibration  
**Merge Commit:** `be81d40`

---

## Executive Summary

The recent remediation batch introduces **7 critical systems refinements** that establish the Milestone 3 baseline:

1. **Typo-auto policy** with bounded multi-token rewrite logic + confidence gating + lexical-strength guardrails
2. **Semantic routing** with dynamic threshold/floor/cap calibration by query type (NL vs path/code)
3. **Semantic-only safety rule** requiring high similarity or overlap signal
4. **Health signal calibration** separating critical vs expected-gap failures
5. **Health status reasoning** with calibrated overall status logic
6. **Debug observability** fields for rewrite decisioning
7. **Deterministic fixture gate** vs live-index suite split in Tests

---

## 1. TYPO-AUTO POLICY: Bounded Multi-Token Rewrite Logic

### Location
`/Users/rexliu/betterspotlight/src/services/query/query_service.cpp:736-914`

### Core Constants

```cpp
constexpr double kRewriteAggregateThreshold = 0.72;    // Line 736
constexpr double kRewriteCandidateThreshold = 0.66;    // Line 737
```

### Rewrite Decision Structure (Line 91-100)

```cpp
struct RewriteDecision {
    QString rewrittenQuery;           // Output: rewritten multi-token query
    bool hasCandidate = false;        // At least one token correction found
    bool applied = false;             // Rewrite actually applied to search
    double confidence = 0.0;          // Aggregate confidence across all replacements
    double minCandidateConfidence = 0.0;  // Minimum individual token confidence
    int candidatesConsidered = 0;     // Total tokens evaluated
    QString reason = QStringLiteral("not_attempted");  // Decision rationale
    QJsonArray correctedTokens;       // Debug: detailed token corrections
};
```

### Confidence Calculation (Line 111-141: `typoCandidateConfidence()`)

**Base confidence: 0.50** then accumulate:

```
+ 0.18 if editDistance == 1
+ 0.08 if editDistance == 2
+ 0.20 if docCount >= 20
+ 0.15 if docCount >= 10
+ 0.10 if docCount >= 5
+ 0.05 if docCount >= 3
+ 0.08 if first letter matches (case-insensitive)
+ 0.04 if token.size() >= 8 AND editDistance == 2
```

**Result:** Clamped to [0.0, 1.0], typically 0.50-1.00 range.

### Bounded Multi-Token Rewrite (Line 739-814: `buildTypoRewriteDecision()`)

**Token selection criteria (Line 752-760):**
- Token size >= 4 characters (minimum)
- NOT in stopword set
- Token NOT already in lexicon (exact match means no typo)
- **candidatesConsidered incremented for tracking**

**Per-token flow (Line 763-791):**

1. **Distance-1 correction attempt** → check confidence threshold (0.66)
2. **Distance-2 fallback** (if allowed, token size >= 8, docCount >= 5)
3. **Confidence gating:** if candidate confidence < 0.66, skip this token
4. **Lexical strength guardrail:** only proceed if rewritten hits improve upon original (Line 834)

**Multi-token limits:**
- **Relaxed mode:** max 2 replacements (Line 825)
- **Auto mode (weak):** max 2 replacements when strict results weak (Line 860)
- **Auto mode (strong):** max 1 replacement when strict results strong (Line 860)

**Aggregate confidence (Line 803-810):**
```cpp
aggregate = sum(appliedCandidateConfidences) / count(appliedCandidateConfidences)
minCandidate = minimum(appliedCandidateConfidences)
```

### Lexical-Strength Guardrail (Line 834)

```cpp
if (bestLexicalStrength(rewrittenHits) >= bestLexicalStrength(relaxedOriginalHits)) {
    rewriteDecision.applied = true;
    // Use rewritten query
} else {
    rewriteDecision.reason = QStringLiteral("rewritten_weaker_than_original");
    // Discard rewrite, keep original
}
```

**bestLexicalStrength()** (Line 102-109): Returns max of absolute BM25 scores → ensures rewrite never degrades ranking.

### Rewrite Decision Reasons (debug field)

| Reason | Condition |
|--------|-----------|
| `strict_mode` | Strict queryMode, no rewrite attempted |
| `relaxed_mode_high_confidence` | Relaxed mode, applied with confidence >= 0.72 |
| `low_confidence` | Has candidate but aggregate < 0.72 |
| `rewritten_weaker_than_original` | Rewrite candidate confidence adequate but BM25 strength didn't improve |
| `no_corrections` | No tokens met correction criteria |
| `strict_hits_present` | Auto mode, strict hits strong, no rewrite needed |
| `strict_weak_or_empty` | Auto mode, strict weak/empty, relaxed rewrite applied |
| `strict_empty_relaxed_original` | Auto mode, strict empty, using relaxed without rewrite |

---

## 2. SEMANTIC ROUTING: Dynamic Threshold/Floor/Cap by Query Type

### Location
`/Users/rexliu/betterspotlight/src/services/query/query_service.cpp:1085-1212`

### Query Type Detection (Line 1085-1092)

```cpp
const bool pathOrCodeLikeQuery = looksLikePathOrCodeQuery(queryLower);  // Line 1085
const bool naturalLanguageQuery = !pathOrCodeLikeQuery 
    && looksLikeNaturalLanguageQuery(querySignalTokens);  // Line 1086-1087

// Dynamic thresholds per query type:
const float semanticThreshold = naturalLanguageQuery ? 0.62f : 0.70f;  // Line 1088
const float semanticOnlyFloor = naturalLanguageQuery ? 0.08f : 0.15f;  // Line 1089
const int semanticOnlyCap = naturalLanguageQuery 
    ? std::min(6, limit)
    : std::min(3, limit / 2);  // Line 1090-1092
```

### Path/Code Detection (Line 148-168: `looksLikePathOrCodeQuery()`)

Returns **true** if query contains:
- Forward slash `/` or backslash `\`
- Leading dot `.` or tilde `~`
- Double colon `::`
- Extension-like token: `[a-z0-9_-]+\.[a-z0-9]{1,8}`
- Code punctuation: `<>{}\[\]();=#`

### Natural Language Detection (Line 143-146: `looksLikeNaturalLanguageQuery()`)

Returns **true** if `signal tokens >= 3` (stopword-filtered, length >= 3)

### Semantic Routing Table

| Query Type | Threshold | OnlyFloor | OnlyCap |
|------------|-----------|-----------|---------|
| Natural Language | 0.62 | 0.08 | min(6, limit) |
| Path/Code-like | 0.70 | 0.15 | min(3, limit/2) |

**Rationale:**
- **NL queries:** Relaxed threshold (0.62) because semantic can bridge vocabulary gaps
- **Path/code queries:** Stricter threshold (0.70) to avoid false positives on structured input
- **NL cap:** Allows up to 6 semantic-only results for rich language understanding
- **Path/code cap:** Limits to ≤3 semantic-only (more conservative)

### Semantic-Only Filtering (Line 1144-1192)

**Candidates must pass TWO gates:**

1. **Similarity threshold** (Line 1107): cosine similarity >= `semanticThreshold`
2. **Normalized floor** (Line 1110-1114): normalized semantic score > `semanticOnlyFloor`
3. **Search filter compliance** (Line 1120-1126): must pass all user-provided filters
4. **Semantic-only safety rule** (Line 1157-1180) — detailed below

---

## 3. SEMANTIC-ONLY SAFETY RULE: High Similarity or Overlap Signal Required

### Location
`/Users/rexliu/betterspotlight/src/services/query/query_service.cpp:1149-1192`

### Two-Gate Approval System

**Gate 1: High Similarity (Line 1157)**
```cpp
bool allowSemanticOnly = semanticSimilarity >= kSemanticOnlySafetySimilarity;  // 0.78
```
If semantic similarity >= **0.78**, automatically allow (very high confidence).

**Gate 2: Overlap Signal (Line 1158-1180)**  
If similarity < 0.78, require **lexical overlap**:

```cpp
if (!querySignalTokens.isEmpty()) {
    // Tokenize result filename + parent directory
    const QStringList overlapTokens = tokenizeWords(
        (sr.name + " " + QFileInfo(sr.path).absolutePath()).toLower());
    
    // Check for ANY query signal token in filename/path
    for (const QString& token : overlapTokens) {
        if (querySignalTokens.contains(token)) {
            allowSemanticOnly = true;  // Overlap found!
            break;
        }
    }
}
```

### Safety Constant (Line 1093)
```cpp
constexpr float kSemanticOnlySafetySimilarity = 0.78f;
```

### Semantic-Only Capacity Gating (Line 1185-1188)

Once approval granted, results still bounded by `semanticOnlyCap`:
- NL: min(6, limit)
- Path/code: min(3, limit/2)

**Effect:** Prevents semantic-only results from dominating rankings even with valid approval.

---

## 4. HEALTH SIGNAL CALIBRATION: Critical vs Expected-Gap Failures

### Location
`/Users/rexliu/betterspotlight/src/core/index/sqlite_store.cpp:1263-1390`

### IndexHealth Structure (header: `/Users/rexliu/betterspotlight/src/core/shared/index_health.h`)

```cpp
struct IndexHealth {
    bool isHealthy = true;
    int64_t totalIndexedItems = 0;
    double lastIndexTime = 0.0;
    double indexAge = 0.0;
    int64_t ftsIndexSize = 0;
    int64_t itemsWithoutContent = 0;
    int64_t totalFailures = 0;
    int64_t criticalFailures = 0;     // POST-REMEDIATION: newly separated
    int64_t expectedGapFailures = 0;  // POST-REMEDIATION: newly separated
    int64_t totalChunks = 0;
};
```

### Two Failure Classes

**1. expectedGapFailures (Line 1289-1315: `sqlite_store.cpp`)**

Extraction failures that are **expected/non-blocking**:
- `PDF extraction unavailable (...)`
- `OCR extraction unavailable (...)`
- `Leptonica failed to read image...`
- `Extension % is not supported by extractor`
- `File size % exceeds configured limit %`
- `File does not exist or is not a regular file`
- `File is not readable`
- `Failed to load PDF document`
- `PDF is encrypted or password-protected`
- `File appears to be a cloud placeholder ...`

**SQL query (Line 1293-1310):**
```sql
SELECT COUNT(*)
FROM failures
WHERE stage = 'extraction'
  AND (error_message LIKE 'PDF extraction unavailable (%' OR ...)
```

**2. criticalFailures (Line 1317-1346)**

All OTHER failures — defined as the **inverse** of expectedGapFailures:
```sql
SELECT COUNT(*)
FROM failures
WHERE NOT (
    stage = 'extraction'
    AND (error_message LIKE 'PDF extraction unavailable ...' OR ...)
)
```

**Examples of critical:**
- Metadata extraction failures
- FTS indexing errors
- Chunking/hashing errors
- Index write failures

### Health Status Determination (Line 1386-1389)

```cpp
health.totalFailures = health.criticalFailures;  // Compatibility alias
health.isHealthy = (health.criticalFailures == 0);  // Only critical failures affect health
```

**Key:** expectedGapFailures do NOT degrade `isHealthy` status.

---

## 5. HEALTH STATUS REASONING: Calibrated Overall Status Logic

### Location
`/Users/rexliu/betterspotlight/src/services/query/query_service.cpp:1596-1612`

### Health Status Decision Tree (Line 1596-1608)

```cpp
QString overallStatus = QStringLiteral("healthy");
QString healthStatusReason = QStringLiteral("healthy");

if (rebuildStateCopy.status == VectorRebuildState::Status::Running
    || health.totalIndexedItems == 0) {
    overallStatus = QStringLiteral("rebuilding");
    healthStatusReason = QStringLiteral("rebuilding");
}
else if (queueSource != QLatin1String("indexer_rpc")) {
    overallStatus = QStringLiteral("degraded");
    healthStatusReason = QStringLiteral("indexer_unavailable");
}
else if (health.criticalFailures > 0) {
    overallStatus = QStringLiteral("degraded");
    healthStatusReason = QStringLiteral("degraded_critical_failures");
}
```

### Status Values

| Status | Reason | Trigger |
|--------|--------|---------|
| `healthy` | `healthy` | Default (vector rebuild complete, indexer RPC connected, no critical failures) |
| `rebuilding` | `rebuilding` | Vector rebuild in progress OR totalIndexedItems == 0 |
| `degraded` | `indexer_unavailable` | Cannot connect to indexer service |
| `degraded` | `degraded_critical_failures` | criticalFailures > 0 (with indexer available) |

### Response Fields (Line 1610-1641)

```cpp
indexHealth[QStringLiteral("overallStatus")] = overallStatus;
indexHealth[QStringLiteral("healthStatusReason")] = healthStatusReason;
indexHealth[QStringLiteral("isHealthy")] = health.isHealthy;
indexHealth[QStringLiteral("totalFailures")] = health.totalFailures;         // == criticalFailures
indexHealth[QStringLiteral("criticalFailures")] = health.criticalFailures;   // NEW
indexHealth[QStringLiteral("expectedGapFailures")] = health.expectedGapFailures;  // NEW
```

---

## 6. DEBUG OBSERVABILITY: Rewrite Decisioning Fields

### Location
`/Users/rexliu/betterspotlight/src/services/query/query_service.cpp:1393-1466`

### Conditional Debug Output (Line 1393)

```cpp
if (debugRequested) {  // Client passes debug=true
    QJsonObject debugInfo;
    // ... populate fields ...
    result[QStringLiteral("debugInfo")] = debugInfo;
}
```

### Debug Fields (alphabetical)

| Field | Type | Source | Purpose |
|-------|------|--------|---------|
| `correctedTokens` | QJsonArray | `rewriteDecision.correctedTokens` | Per-token correction details: from, to, editDistance, docCount, candidateConfidence |
| `filters` | QJsonObject | searchOptions | Active search filters (includePaths, excludePaths, fileTypes, sizes, dates) |
| `fusionMode` | string | Hardcoded | "weighted_rrf" (describes semantic merge strategy) |
| `lexicalRelaxedHits` | int | `relaxedHits.size()` | Count of FTS5 results from relaxed search |
| `lexicalStrictHits` | int | `strictHits.size()` | Count of FTS5 results from strict search |
| `parsedTypes` | QJsonArray | `parsed.filters.fileTypes` | Extracted file type hints from query |
| `plannerApplied` | bool | `plannerApplied` flag | Whether consumer prefilter applied |
| `plannerReason` | string | `plannerReason` | "query_location_hint", "consumer_curated_prefilter", "none" |
| `queryAfterParse` | string | `query` | Normalized query post-parsing |
| `queryMode` | string | `queryMode` | "strict", "relaxed", or "auto" |
| `rewriteApplied` | bool | `rewriteDecision.applied` | Whether typo rewrite actually used |
| `rewriteCandidatesConsidered` | int | `rewriteDecision.candidatesConsidered` | Total tokens evaluated for typo correction |
| `rewriteConfidence` | double | `rewriteDecision.confidence` | Aggregate confidence of applied corrections |
| `rewriteMinCandidateConfidence` | double | `rewriteDecision.minCandidateConfidence` | Weakest token in rewrite |
| `rewriteReason` | string | `rewriteDecision.reason` | Decision rationale (see Section 1) |
| `rewrittenQuery` | string | `rewrittenRelaxedQuery` | Actual query sent to FTS5 (if rewritten) |
| `semanticCandidates` | int | `semanticResults.size()` | Count of KNN hits passing semantic threshold |
| `semanticOnlyCapApplied` | int | `semanticOnlyCap` | Max semantic-only results allowed |
| `semanticOnlyFloorApplied` | float | `semanticOnlyFloor` | Minimum normalized semantic score |
| `semanticThresholdApplied` | float | `semanticThreshold` | Cosine similarity threshold used |

### Representative JSON Output

```json
{
  "debugInfo": {
    "queryMode": "auto",
    "queryAfterParse": "elasticsearch clustering",
    "lexicalStrictHits": 12,
    "lexicalRelaxedHits": 42,
    "semanticCandidates": 8,
    "semanticThresholdApplied": 0.62,
    "semanticOnlyFloorApplied": 0.08,
    "semanticOnlyCapApplied": 6,
    "correctedTokens": [
      {
        "from": "elasticsearch",
        "to": "elasticsearch",
        "editDistance": 1,
        "docCount": 45,
        "candidateConfidence": 0.86
      }
    ],
    "rewriteApplied": true,
    "rewriteConfidence": 0.86,
    "rewriteMinCandidateConfidence": 0.86,
    "rewriteCandidatesConsidered": 1,
    "rewriteReason": "strict_weak_or_empty",
    "rewrittenQuery": "elasticsearch clustering",
    "plannerApplied": true,
    "plannerReason": "consumer_curated_prefilter",
    "filters": {
      "hasFilters": true,
      "includePaths": ["/Users/me/Documents"],
      "excludePaths": [],
      "fileTypes": []
    }
  }
}
```

---

## 7. TEST SUITE SPLIT: Deterministic Fixture vs Live-Index

### Location
`/Users/rexliu/betterspotlight/Tests/CMakeLists.txt` (remediation commit `0cede3e`)

### Fixture-Based Gate (Line 41)

```cmake
bs_add_test(test-query-service-relevance-fixture Integration/test_query_service_relevance_fixture.cpp)
target_compile_definitions(test-query-service-relevance-fixture PRIVATE
    BS_RELEVANCE_BASELINES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/relevance/baselines.json"
)
```

**Test:** `/Tests/Integration/test_query_service_relevance_fixture.cpp`

**Purpose:** Deterministic ranking gate against fixed baseline fixture

**Workflow:**
1. Load `baselines.json` (query cases + gatePassRate threshold)
2. Resolve fixture files from `Tests/Fixtures/{fixtureId}/`
3. Seed into temporary SQLite database
4. Launch query service with isolated environment
5. Execute all test queries via IPC
6. Verify top-N results against expected file names
7. Pass/fail by pass rate vs gatePassRate

**Key determinism mechanisms:**
- **Sealed fixture directory** with known file content (no filesystem changes)
- **Fresh SQLite database** for each test run
- **Deterministic test query cases** (not randomized)
- **IPC-based search** (avoids UI/service startup vagaries)
- **Baseline JSON** gates exact pass rates: default 90%

### Live-Index Suite (Line 57)

```cmake
bs_add_test(test-ui-sim-query-suite Integration/test_ui_sim_query_suite.cpp)
target_compile_definitions(test-ui-sim-query-suite PRIVATE
    BS_RELEVANCE_SUITE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/relevance/ui_sim_query_suite.json"
)
set_tests_properties(test-ui-sim-query-suite PROPERTIES
    ENVIRONMENT "BS_RELEVANCE_GATE_MODE=report_only;BS_RELEVANCE_REPORT_PATH=/tmp/bs_ui_sim_query_suite_report.json"
)
```

**Purpose:** Observational reporting on live user-home index (report-only mode)

**Differences from fixture gate:**
- **No embedded test database** — runs against live `$HOME/Library/Application Support/betterspotlight/index.db`
- **Report-only mode** — generates JSON report but doesn't fail test
- **Larger query set** — exercises full-scale retrieval on user's actual content
- **Stress variant** (Line 61) — runs test twice with same report path

**Why dual approach?**
- **Fixture gate:** Catches regressions in core ranking logic early
- **Live-index suite:** Validates real-world performance without blocking CI

---

## Milestone 3 Baseline: Summary Table

| System | Component | Threshold | Guardrail | Debug Field |
|--------|-----------|-----------|-----------|-------------|
| **Typo-auto** | Candidate confidence | 0.66 | Lexical strength (BM25) | rewriteConfidence, correctedTokens, rewriteReason |
| **Typo-auto** | Aggregate confidence | 0.72 | Must >= threshold to apply | rewriteApplied |
| **Typo-auto** | Token selection | >= 4 chars | Stopword filter, in-lexicon check | rewriteCandidatesConsidered |
| **Typo-auto** | Multi-token limit | 1-2 tokens | Depends on query mode & strength | rewriteMinCandidateConfidence |
| **Semantic routing** | NL threshold | 0.62 | Dynamic per query type | semanticThresholdApplied |
| **Semantic routing** | Code threshold | 0.70 | Stricter (false-positive avoidance) | semanticOnlyFloorApplied |
| **Semantic routing** | NL only-cap | min(6, limit) | Balances coverage vs safety | semanticOnlyCapApplied |
| **Semantic routing** | Code only-cap | min(3, limit/2) | Conservative for structured queries | semanticCandidates |
| **Semantic safety** | Direct approve | >= 0.78 | High similarity (very confident) | (no debug field) |
| **Semantic safety** | Overlap gate | Query tokens in path/name | Requires lexical anchor | (no debug field) |
| **Health signals** | Critical failures | > 0 → degraded | Blocks only on true failures | criticalFailures |
| **Health signals** | Expected gaps | Counted separately | Don't affect isHealthy | expectedGapFailures |
| **Health signals** | Vector rebuilding | In progress | overallStatus=rebuilding | healthStatusReason |
| **Tests** | Fixture gate | >= 90% (default) | Deterministic baseline | testFixtureRelevanceGateViaIpc |
| **Tests** | Live-index suite | Report-only | Observational only | (separate JSON report) |

---

## Verification Checklist for Milestone 3

Use this to validate post-remediation state:

- [ ] **Typo rewrite logic**: Verify `kRewriteAggregateThreshold = 0.72` and `kRewriteCandidateThreshold = 0.66`
- [ ] **Lexical guardrail**: Confirm rewrite only applied if `bestLexicalStrength(rewritten) >= bestLexicalStrength(original)`
- [ ] **Multi-token bounds**: Check max 2 replacements relaxed/weak-auto, max 1 strong-auto
- [ ] **Semantic NL threshold**: `0.62` (relaxed) vs code `0.70` (strict)
- [ ] **Semantic only cap**: NL min(6, limit) vs code min(3, limit/2)
- [ ] **Semantic safety**: `kSemanticOnlySafetySimilarity = 0.78` with overlap fallback
- [ ] **Health struct**: Separated `criticalFailures` and `expectedGapFailures` fields present
- [ ] **Expected gaps SQL**: Matches patterns for PDF/OCR unavailable, unsupported ext, cloud placeholders
- [ ] **Health status**: overallStatus={healthy, rebuilding, degraded}, healthStatusReason populated
- [ ] **Debug fields**: All 18+ fields in debugInfo when debug=true
- [ ] **Fixture gate**: `test_query_service_relevance_fixture.cpp` runs with baselines.json
- [ ] **Live suite**: `test_ui_sim_query_suite.cpp` runs in report-only mode
- [ ] **Fixture split**: Deterministic vs live tests in separate CMake targets

---

## Code References (Exact Line Numbers)

| Concept | File | Lines |
|---------|------|-------|
| RewriteDecision struct | query_service.cpp | 91-100 |
| Confidence calculation | query_service.cpp | 111-141 |
| Rewrite thresholds | query_service.cpp | 736-737 |
| buildTypoRewriteDecision | query_service.cpp | 739-814 |
| Query type detection | query_service.cpp | 1085-1092 |
| Semantic thresholds | query_service.cpp | 1088-1093 |
| Semantic-only safety | query_service.cpp | 1157-1180 |
| Health status logic | query_service.cpp | 1596-1612 |
| Debug info output | query_service.cpp | 1393-1466 |
| getHealth() handler | query_service.cpp | 1471-1671 |
| expectedGapFailures SQL | sqlite_store.cpp | 1289-1315 |
| criticalFailures SQL | sqlite_store.cpp | 1317-1346 |
| Health determination | sqlite_store.cpp | 1386-1389 |
| Fixture test | test_query_service_relevance_fixture.cpp | Full file |
| CMake gate split | CMakeLists.txt | 41+57 |

