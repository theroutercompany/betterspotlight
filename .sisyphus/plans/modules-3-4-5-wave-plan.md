# BetterSpotlight Modules 3/4/5 Implementation Plan

## Context
- Build: CLEAN, 30/30 unit tests pass
- Relevance: 28/30 (93.33%) on 30 active cases, 23 cases SKIPPED (53 total)
- Two modules done: TypoLexicon + Hybrid BM25+Vector
- Remaining: Query Normalization, Metadata Filter Engine, Query Parser
- Two failing cases: TA02 (Acocmmodation), TA10 (reigster in FTS path)

## Constraints
- All C++/Qt6, no Swift
- Must not break 30 unit tests
- P95 < 300ms search latency
- Each work unit = one agent, no mid-task coordination
- Namespace: bs, logging: LOG_INFO/LOG_WARN/LOG_DEBUG/LOG_ERROR(category, fmt, ...)

---

## Wave 1 — Fix 2 failing active cases + build normalization primitives

All three tasks are parallel-safe (different files).

### W1-A: TypoLexicon double-letter compression (fix TA02 + helps N05)

**Files modified:**
- `src/core/index/typo_lexicon.h`
- `src/core/index/typo_lexicon.cpp`

**Change:**
- Add `static QString compressRuns(const QString& s)` that collapses consecutive duplicate letters: `acocmmodation` → `acomodation`, `modelll` → `model`
- In `TypoLexicon::correct()`, add a secondary distance check: compute DL distance between `compressRuns(inputToken)` and `compressRuns(candidate.text)`. If this compressed distance ≤ maxDistance AND the original distance > maxDistance, use the compressed match.
- Keep returning the original candidate `term.text` (not the compressed form)
- Only trigger compression path when standard DL fails (distance > maxDistance)

**Success criteria:**
- `TypoLexicon::correct("acocmmodation", 2)` returns `"accommodation"`
- `TypoLexicon::correct("modelll", 2)` returns `"model"` (if in lexicon)
- Existing corrections (e.g. "emebdding"→"embedding", "crdeit"→"credit") still work unchanged
- Build clean, 30/30 unit tests pass

**Tests impacted:** Fixes TA02, helps N05
**Dependencies:** None
**Effort:** Short (1-4h)

---

### W1-B: Implement QueryNormalizer (standalone utility, no service wiring)

**Files added:**
- `src/core/query/query_normalizer.h`
- `src/core/query/query_normalizer.cpp`

**Files modified (build system):**
- `src/core/CMakeLists.txt` — add new source files to core library

**Change:**
- Create `bs::QueryNormalizer` class with static method:
  ```cpp
  struct NormalizedQuery {
      QString original;
      QString normalized; // for matching
  };
  static NormalizedQuery normalize(const QString& raw);
  ```
- Normalization steps (O(n) char-scan):
  1. Trim leading/trailing whitespace
  2. Collapse multiple whitespace chars to single space
  3. Remove noise punctuation: `!`, `?`, `$`, `@`, `#`, `%`, `^`, `&`, `*`, `(`, `)`, `{`, `}`, `[`, `]`, `~`, `` ` ``
  4. Normalize en-dash (U+2013) and em-dash (U+2014) to hyphen `-`
  5. Collapse repeated hyphens/dashes (`---` → `-`)
  6. Preserve intra-word hyphens (e.g. `sound-barrier` stays)
  7. Strip outer quotes (both `"` and `'`) from the entire string
  8. Case-fold to lowercase for matching

**Success criteria:**
- `"   BREAKING!!! sound---barrier??? docx   "` → `"breaking sound-barrier docx"`
- `"\"coinbase\"   commerce   payments    issues"` → `"coinbase commerce payments issues"`
- `"travel cost accommodation $$$ receipt"` → `"travel cost accommodation receipt"`
- `"ON WATERLOO REGIONAL PS REPORT"` → `"on waterloo regional ps report"`
- `"embedding   modelll trcaes"` → `"embedding modelll trcaes"` (typo correction is separate)
- Build clean, 30/30 unit tests pass

**Tests impacted:** Enables N01-N04 once integrated + unskipped
**Dependencies:** None
**Effort:** Short (1-4h)

---

### W1-C: TA10 fix — apply TypoLexicon in QueryService relaxed/auto FTS path

**Files modified:**
- `src/services/query/query_service.cpp`

**Change:**
- In the auto-mode fallback path (where strict FTS returns too few results and code falls back to relaxed), apply TypoLexicon token correction BEFORE sending the query to `m_store->searchFts5(..., relaxed=true)`
- Specifically: tokenize the query, for each token ≥ 4 chars that's not a stopword, attempt `m_typoLexicon.correct(token, maxDistance)` where maxDistance = (token.size() >= 8) ? 2 : 1
- Max 2 replacements per query
- Replace the corrected tokens in the query string before passing to searchFts5()
- Do NOT modify strict mode behavior

**Success criteria:**
- Query `"how do you reigster as a non profit then in ontari"` in auto mode: `"reigster"` is corrected to `"register"` before FTS5 search
- Strict mode queries unchanged
- Build clean, 30/30 unit tests pass

**Tests impacted:** Fixes TA10
**Dependencies:** TypoLexicon already available in QueryService (it is)
**Effort:** Quick (<1h)

---

## Wave 2 — Metadata Filter Engine (parallel-safe by file partition)

W2-A and W2-B can run in parallel (different files). W2-C depends on both.

### W2-A: SearchOptions struct

**Files added:**
- `src/core/shared/search_options.h`

**Files modified (build system):**
- `src/core/CMakeLists.txt` — add header to core library (header-only)

**Change:**
- Define `bs::SearchOptions` struct (header-only):
  ```cpp
  struct SearchOptions {
      std::vector<QString> fileTypes;      // extensions: "pdf", "docx"
      std::vector<QString> excludePaths;   // path prefixes to exclude
      std::optional<double> modifiedAfter; // epoch seconds
      std::optional<double> modifiedBefore;
      std::optional<int64_t> minSizeBytes;
      std::optional<int64_t> maxSizeBytes;
      
      bool hasFilters() const;  // returns true if any filter is set
  };
  ```

**Success criteria:**
- Header compiles without errors
- `hasFilters()` returns false for default-constructed SearchOptions
- Build clean, 30/30 unit tests pass

**Tests impacted:** Foundation for W2-B, W2-C, W4-A
**Dependencies:** None
**Effort:** Quick (<1h)

---

### W2-B: SQL-level filtering in SQLiteStore

**Files modified:**
- `src/core/index/sqlite_store.h`
- `src/core/index/sqlite_store.cpp`

**Change:**
- Add new overloads (keep old signatures for backward compat):
  ```cpp
  std::vector<SearchResult> searchFts5(const QString& query, int limit, bool relaxed, const SearchOptions& opts);
  std::vector<SearchResult> searchByNameFuzzy(const QString& query, int limit, const SearchOptions& opts);
  ```
- Implement SQL WHERE clause construction:
  - `fileTypes`: `AND LOWER(i.extension) IN (?, ?, ...)` or if no `extension` column, parse from `i.path`
  - `excludePaths`: `AND i.path NOT LIKE ?` for each prefix (append `%`)
  - `modifiedAfter`: `AND i.modified_at >= ?`
  - `modifiedBefore`: `AND i.modified_at <= ?`
  - `minSizeBytes`: `AND i.size >= ?`
  - `maxSizeBytes`: `AND i.size <= ?`
- Use parameterized queries (no string interpolation for SQL injection safety)
- When SearchOptions.hasFilters() is false, behave identically to old signatures

**Success criteria:**
- Old signatures still compile and work unchanged
- New overloads correctly restrict results at SQL layer
- Build clean, 30/30 unit tests pass

**Tests impacted:** Enables end-to-end metadata filtering
**Dependencies:** W2-A (SearchOptions definition)
**Effort:** Medium (1-2d)

---

### W2-C: Wire QueryService to pass SearchOptions through all retrieval paths

**Files modified:**
- `src/services/query/query_service.cpp`

**Change:**
- Parse `SearchOptions` from incoming request params (if present)
- Pass SearchOptions to:
  - strict FTS path: `m_store->searchFts5(query, limit, false, opts)`
  - relaxed FTS path: `m_store->searchFts5(query, limit, true, opts)`
  - fuzzy name fallback: `m_store->searchByNameFuzzy(query, limit, opts)`
  - semantic path: filter semantic results by checking item metadata against opts after vector fetch
- When no options present, use default (empty) SearchOptions — no behavior change

**Success criteria:**
- Filters apply consistently across all retrieval paths
- No behavior change when options are omitted
- Build clean, 30/30 unit tests pass

**Tests impacted:** Completes Metadata Filter Engine module
**Dependencies:** W2-A + W2-B
**Effort:** Short (1-4h)

---

## Wave 3 — QueryNormalizer integration + unskip Noise

W3-A then W3-B (sequential).

### W3-A: Integrate QueryNormalizer at start of handleSearch()

**Files modified:**
- `src/services/query/query_service.cpp`

**Change:**
- At the top of `handleSearch()`, after extracting rawQuery from params:
  ```cpp
  #include "core/query/query_normalizer.h"
  // ...
  auto normalizedResult = bs::QueryNormalizer::normalize(rawQuery);
  const QString& queryForSearch = normalizedResult.normalized;
  ```
- Use `queryForSearch` instead of `rawQuery` for:
  - FTS5 query strings
  - Semantic embedding input
  - Token analysis for typo correction
- Keep `rawQuery` in logs for debugging

**Success criteria:**
- `"   BREAKING!!! sound---barrier??? docx   "` → FTS5 receives `"breaking sound-barrier docx"`
- Phrase tests (P01-P08) don't regress
- Build clean, 30/30 unit tests pass

**Tests impacted:** Enables N01-N04 (+ N05 with W1-A)
**Dependencies:** W1-B (QueryNormalizer exists)
**Effort:** Quick (<1h)

---

### W3-B: Unskip Noise category in relevance suite

**Files modified:**
- `Tests/Integration/test_ui_sim_query_suite.cpp`

**Change:**
- Remove `"noise"` from the skip condition (keep `typo_strict` and `semantic_probe` skipped for now)
- Update: change skip condition from:
  ```cpp
  if (testCase.category == "semantic_probe" || testCase.category == "noise" || testCase.category == "typo_strict")
  ```
  to:
  ```cpp
  if (testCase.category == "semantic_probe" || testCase.category == "typo_strict")
  ```

**Success criteria:**
- Noise cases (N01-N05) execute instead of being skipped
- N01-N05 pass on live index
- Active case count increases from 30 to 35
- Build clean, 30/30 unit tests pass (relevance gate separate)

**Tests impacted:** Activates N01-N05
**Dependencies:** W3-A + W1-A (for N05 combo typo+noise)
**Effort:** Quick (<1h)

---

## Wave 4 — Query Parser + unskip Semantic Probe

W4-A and W4-C can run in parallel (different files). W4-B depends on W4-A.

### W4-A: QueryParser (conservative intent extraction)

**Files added:**
- `src/core/query/query_parser.h`
- `src/core/query/query_parser.cpp`

**Files modified (build system):**
- `src/core/CMakeLists.txt` — add new source files

**Change:**
- Create `bs::QueryParser` class:
  ```cpp
  struct ParsedQuery {
      QString remainingText;     // query with filter tokens removed
      SearchOptions inferredOptions;
  };
  static ParsedQuery parse(const QString& query);
  ```
- Rules (intentionally narrow to avoid false positives):
  - **File-type hints:** Detect bare extension tokens: `pdf`, `docx`, `md`, `txt`, `csv`, `xlsx`, `pptx`
    - Only extract if token appears to be a filter (not part of a phrase)
  - **Category keywords:** `spreadsheet` → {xlsx,csv,numbers}, `photo`/`image` → {jpg,jpeg,png,heic,webp}, `document` → {pdf,docx,doc,rtf,pages}, `video` → {mp4,mov,avi,mkv}, `audio` → {mp3,wav,aac,flac}
  - **Date expressions (high-confidence only):**
    - `today` → modifiedAfter = start of today
    - `yesterday` → modifiedAfter/Before = yesterday range
    - `last week` → modifiedAfter = 7 days ago
    - `last month` → modifiedAfter = 30 days ago
    - `Month Year` pattern (e.g. `May 2024`) → month date range
  - Preserve all non-filter tokens in `remainingText` in original order

**Success criteria:**
- `"find my pdf report"` → options.fileTypes=["pdf"], remaining="find my report"
- `"May 2024 report"` → date range set to May 2024, remaining="report"
- `"spreadsheet budget"` → options.fileTypes=["xlsx","csv","numbers"], remaining="budget"
- `"hello world"` → no options extracted, remaining="hello world"
- Build clean, 30/30 unit tests pass

**Tests impacted:** Module 5 completion (no current suite cases test this directly)
**Dependencies:** W2-A (SearchOptions)
**Effort:** Medium (1-2d)

---

### W4-B: Integrate QueryParser in QueryService pipeline

**Files modified:**
- `src/services/query/query_service.cpp`

**Change:**
- New pipeline order at top of handleSearch():
  1. `parsed = QueryParser::parse(rawQuery)`
  2. `normalized = QueryNormalizer::normalize(parsed.remainingText)`
  3. `finalOptions = merge(explicitParamOptions, parsed.inferredOptions)` — explicit wins
- Pass `finalOptions` to all search paths
- Semantic embedding uses normalized remaining query

**Success criteria:**
- No regressions when users pass explicit options (explicit wins over inferred)
- Parser never strips important tokens
- Build clean, 30/30 unit tests pass

**Tests impacted:** Module 5 end-to-end
**Dependencies:** W4-A + W3-A (normalizer already integrated)
**Effort:** Short (1-4h)

---

### W4-C: Unskip Semantic Probe in test harness

**Files modified:**
- `Tests/Integration/test_ui_sim_query_suite.cpp`

**Change:**
- Remove `"semantic_probe"` from skip condition
- For semantic_probe cases, initialize the semantic pipeline:
  - Instantiate `EmbeddingManager` with model path discovery (same as QueryService)
  - Load `VectorIndex` / vector store
  - For each semantic_probe case:
    - Run FTS5 search (lexical)
    - Run semantic search via embedding + vector index KNN
    - Merge via `SearchMerger::merge()` (RRF fusion)
    - Check expected file in topN of merged results
  - If model assets missing: `QSKIP("Semantic model not found")` for semantic_probe only (don't skip entire suite)

**Success criteria:**
- semantic_probe cases execute on machines with model files
- S01-S08 pass when vector index is populated
- Graceful skip when model missing
- Active case count increases from 35 to 43 (with typo_strict still skipped at 12)
- Build clean, 30/30 unit tests pass

**Tests impacted:** Activates S01-S08
**Dependencies:** M2 code exists (it does). Better after W3-A so query is normalized.
**Effort:** Medium (1-2d)

---

## Dependency Graph

```
Wave 1 (all parallel):
  W1-A (TypoLexicon compression) ──────────────────────────┐
  W1-B (QueryNormalizer standalone) ───┐                    │
  W1-C (TA10 fix in QueryService) ─── │ ──────────────────── │
                                       │                    │
Wave 2 (W2-A parallel, W2-B after A, W2-C after B):       │
  W2-A (SearchOptions struct) ─────────┤                    │
  W2-B (SQL filtering) ←── W2-A       │                    │
  W2-C (Wire QueryService) ←── W2-B   │                    │
                                       │                    │
Wave 3 (sequential):                   │                    │
  W3-A (Integrate normalizer) ←── W1-B ┘                    │
  W3-B (Unskip noise) ←── W3-A + W1-A ─────────────────────┘
                                       
Wave 4 (W4-A and W4-C parallel, W4-B after A):
  W4-A (QueryParser) ←── W2-A
  W4-B (Integrate parser) ←── W4-A + W3-A
  W4-C (Unskip semantic_probe) ←── (independent, better after W3-A)
```

## Expected Pass Rate Trajectory

| After | Active | Expected Pass | Rate |
|-------|--------|---------------|------|
| Current | 30 | 28 | 93.33% |
| W1 (A+C) | 30 | 30 | 100% |
| W3-B | 35 | 35 | 100% |
| W4-C | 43 | 43 | 100% |

(typo_strict 12 cases remain intentionally skipped as negative-path tests)

## Risk Watchlist
1. **Quote stripping**: Removing quotes globally can reduce phrase strictness. Mitigate: only strip outer quotes.
2. **SQL filter performance**: Multiple NOT LIKE clauses add overhead. Keep excludePaths small.
3. **Semantic test environment**: Model assets may be missing on CI. Use QSKIP gracefully.
4. **W1-C + W2-C + W3-A + W4-B all touch query_service.cpp**: Must be sequential, not parallel.
