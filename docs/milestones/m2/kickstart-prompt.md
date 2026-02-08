# BetterSpotlight M2 Development Kickstart

**For: Lead Engineers + AI-Assisted Development (Code / Sub-Agents)**
**Date: 2026-02-07**
**Scope: M2 - ML Integration + Semantic Search + 80% Reliability**

---

## How To Use This Prompt

Same model as the M1 kickstart. This document defines **12 independent work streams** organized into 4 waves. Each work stream can be assigned to a separate sub-agent. Dependency constraints between waves must be respected; all streams within a wave are independent.

**Prerequisite:** M1 must be complete. All M1 acceptance criteria passing. The existing codebase provides: FTS5 search, FSEvents monitoring, content extraction pipeline, IPC services, Qt UI with search panel, ranking/scoring engine, and full test infrastructure.

**Execution strategy:**
1. Read the referenced documentation files BEFORE writing any code
2. Launch all sub-agents within a wave simultaneously
3. Wait for a wave to complete before starting the next wave
4. Each sub-agent produces a self-contained module with unit tests
5. Integration happens in Waves 3 and 4

---

## Project Context (Delta from M1)

M2 adds four major subsystems on top of the M1 foundation:

1. **Embedding Pipeline** - ONNX Runtime inference, BGE-small-en-v1.5, WordPiece tokenizer, batch processing
2. **Vector Search** - hnswlib HNSW index, KNN search, semantic-lexical merge algorithm
3. **Feedback & Interaction Tracking** - User behavior learning, path preferences, file type affinity
4. **Onboarding + Settings UI** - 3-step first-run flow, 5-tab settings, Index Health dashboard

The core principle: FTS5 lexical search remains the primary path. Semantic search augments it. If any ML component fails, the system falls back to FTS5-only with zero user impact.

---

## Documentation Suite (READ BEFORE CODING)

Every sub-agent MUST read its referenced docs before writing any code.

### Foundation Docs (from M1, still authoritative)

| Doc | Path | Relevant M2 Sections |
|-----|------|---------------------|
| Architecture Overview | `foundation/architecture-overview.md` | Sections 1.2-1.4 (UI layer, service topology) |
| Storage Schema | `foundation/storage-schema.md` | Section 3.12 (vector_map table), settings table |
| Indexing Pipeline | `foundation/indexing-pipeline.md` | Stage 8 (embedding stage) |
| IPC Boundaries | `foundation/ipc-service-boundaries.md` | New M2 methods on services |
| Ranking & Scoring | `foundation/ranking-scoring.md` | Section 6 (semantic boost), feedback loop |
| Build System | `foundation/build-system.md` | ONNX Runtime linking, hnswlib submodule |
| Acceptance Criteria | `milestones/acceptance-criteria.md` | M2 section (80% relevance, semantic criteria) |

### M2-Specific Docs (new, authoritative for M2)

| Doc | Path | Summary |
|-----|------|---------|
| Embedding Pipeline | `milestones/m2/embedding-pipeline.md` | ONNX session, tokenizer, batching, quantization |
| Vector Search | `milestones/m2/vector-search.md` | hnswlib config, persistence, merge algorithm |
| Feedback System | `milestones/m2/feedback-system.md` | InteractionTracker, path prefs, type affinity |
| Onboarding & Settings | `milestones/m2/onboarding-settings-spec.md` | 3-step flow, 5-tab UI, Index Health |
| Relevance Testing | `milestones/m2/relevance-testing.md` | 50-query corpus, fixture, scoring method |

---

## Key Architectural Decisions (Non-Negotiable for M2)

These are settled. Do not re-litigate.

1. **Embedding model:** BGE-small-en-v1.5, int8 dynamic quantized ONNX (~35MB), bundled in app
2. **Inference runtime:** ONNX Runtime with CoreML execution provider on Apple Silicon
3. **Tokenizer:** C++ WordPiece implementation (no HuggingFace/Rust dependency), BERT-base-uncased vocab
4. **Vector index:** hnswlib, InnerProduct space (L2-normalized vectors), M=16, efConstruction=200, efSearch=50
5. **Merge strategy:** Independent lexical_weight (0.6) and semantic_weight (0.4) multipliers, NOT a distribution
6. **Similarity threshold:** 0.7 cosine similarity minimum for semantic results
7. **Embedding scope:** First chunk only per file (no sliding window, no multi-chunk for M2)
8. **Query prefix:** BGE requires `"Represent this sentence for searching relevant passages: "` prepended to queries
9. **Interaction retention:** 180 days (longer than feedback's 90 days)
10. **Onboarding:** 3 steps (Welcome, FDA, Home Map), runs before first indexing

---

## Build Target Additions (M2)

M2 adds to the existing M1 build targets:

```
betterspotlight-core   (static lib)  - ADD: embedding/, vector/, feedback/ modules
betterspotlight-app    (app bundle)  - ADD: onboarding/, settings/ QML components
Resources/models/      (bundled)     - NEW: bge-small-en-v1.5-int8.onnx, vocab.txt
```

New CMake dependencies:
- `onnxruntime` (dynamic, Apache-2.0)
- `hnswlib` (header-only, Apache-2.0, Git submodule)

---

## Wave 0: ML Foundation (All Parallel)

### Sub-Agent 0A: ONNX Runtime Integration + WordPiece Tokenizer

**Read:** `milestones/m2/embedding-pipeline.md` (Sections 1-3), `foundation/build-system.md`

**Deliverables:**
```
src/core/embedding/
  tokenizer.h/.cpp        - WordPiece tokenizer (vocab loading, normalize, tokenize, special tokens)
  embedding_manager.h/.cpp - ONNX session lifecycle, single + batch inference, L2 normalization
  quantizer.h/.cpp        - float32 <-> int8 quantization
```

**Tokenizer requirements (embedding-pipeline.md Section 3):**
- Load `vocab.txt` (30,522 tokens from BERT-base-uncased) into `unordered_map<string, int>`
- Normalize: lowercase, strip accents (NFD), collapse whitespace
- Tokenize: greedy longest-match WordPiece with `##` continuation prefix
- Special tokens: `[CLS]` (101) at start, `[SEP]` (102) at end, `[PAD]` (0), `[UNK]` (100)
- Truncate to 512 tokens (510 content + 2 special)
- Generate attention_mask (1 for real, 0 for padding) and token_type_ids (all zeros)

**ONNX session requirements (embedding-pipeline.md Section 2):**
- Singleton `OrtEnvironment` (process lifetime)
- `OrtSessionOptions`: intra_op_num_threads=2, inter_op_num_threads=1, ORT_SEQUENTIAL
- CoreML provider: `COREML_FLAG_USE_CPU_AND_GPU`, cache compiled model at `~/Library/Caches/betterspotlight/coreml/`
- Fallback to CPU if CoreML unavailable (log warning, not error)
- Validate input shape `[batch, seq_len]`, output shape `[batch, 384]`

**L2 normalization (embedding-pipeline.md Section 4.3):**
```
norm = sqrt(sum(v[i]^2))
v[i] = v[i] / norm
```

**Int8 quantization (embedding-pipeline.md Section 4.4):**
- Per-vector: compute min/max, scale = (max-min)/255, zero_point = round(-min/scale)
- Store: scale (float32, 4B) + zero_point (int8, 1B) + q[384] (int8, 384B) = 389 bytes/vector
- Dequantize for hnswlib search (index stores float32 internally)

**Acceptance:** Unit tests: tokenize known sentence and verify token IDs match expected output. Embed known text and verify output is 384-dim float32 vector. Verify L2 norm of output is ~1.0. Batch of 32 items produces correct individual embeddings. Quantize-dequantize roundtrip error < 0.01 per dimension.

---

### Sub-Agent 0B: hnswlib Integration + Vector Store

**Read:** `milestones/m2/vector-search.md` (Sections 1-2), `foundation/storage-schema.md` (vector_map table)

**Deliverables:**
```
src/core/vector/
  vector_index.h/.cpp     - hnswlib wrapper (create, add, delete, search, persist, load)
  vector_store.h/.cpp     - vector_map table CRUD (item_id <-> hnsw_label mapping)
```

**hnswlib configuration (vector-search.md Section 1):**
- Space: `hnswlib::InnerProductSpace` (384 dimensions)
- M=16, efConstruction=200, efSearch=50
- Initial capacity: 100,000. Resize 2x at 80% fill.

**VectorIndex interface:**
```cpp
class VectorIndex {
public:
    bool create(int initialCapacity = 100000);
    bool load(const std::string& indexPath, const std::string& metaPath);
    bool save(const std::string& indexPath, const std::string& metaPath);

    uint64_t addVector(const float* embedding);  // returns hnsw_label
    bool deleteVector(uint64_t label);            // markDelete()

    struct KnnResult { uint64_t label; float distance; };
    std::vector<KnnResult> search(const float* query, int k = 50);

    int totalElements() const;
    int deletedElements() const;
    bool needsRebuild() const;  // deleted/total > 0.20
    bool rebuild();             // compact index
};
```

**VectorStore (SQLite, vector-search.md Section 2.5):**
```sql
CREATE TABLE IF NOT EXISTS vector_map (
    item_id     INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    hnsw_label  INTEGER NOT NULL UNIQUE,
    model_version TEXT NOT NULL DEFAULT 'bge-small-en-v1.5',
    embedded_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (item_id)
);
CREATE INDEX idx_vector_map_label ON vector_map(hnsw_label);
```

**Persistence (vector-search.md Section 2.5):**
- `vectors.hnsw`: hnswlib native binary at `~/.local/share/betterspotlight/vectors.hnsw`
- `vectors.meta`: JSON metadata (version, model, dimensions, counts, next_label, params, last_persisted)
- Persist every 1000 additions or every 60 seconds
- Atomic swap on rebuild (rename new over old)

**Acceptance:** Unit tests: create index, add 1000 random 384-dim vectors, search returns K nearest neighbors, delete 200 vectors, rebuild compacts index, save/load roundtrip preserves all vectors, vector_map table correctly maps item_id to hnsw_label.

---

## Wave 1: Core M2 Modules (All Parallel, After Wave 0)

### Sub-Agent 1A: Search Merger (Semantic + Lexical)

**Read:** `milestones/m2/vector-search.md` (Sections 3-4), `foundation/ranking-scoring.md` (Section 6)

**Deliverables:**
```
src/core/vector/
  search_merger.h/.cpp    - Score normalization + merge algorithm
```

**Score normalization (vector-search.md Section 4.1):**
- Lexical: `normalized = result.finalScore / max_lexical_score` (relative to best result)
- Semantic: `normalized = (cosine_similarity - threshold) / (1.0 - threshold)` (maps [0.7, 1.0] to [0.0, 1.0])

**Merge categories (vector-search.md Section 4.2):**
1. BOTH (item in lexical AND semantic): `merged = lexical_weight * norm_lexical + semantic_weight * norm_semantic`
2. LEXICAL_ONLY: `merged = lexical_weight * norm_lexical`
3. SEMANTIC_ONLY: `merged = semantic_weight * norm_semantic`

Weights: lexical_weight=0.6, semantic_weight=0.4 (independent, NOT a distribution).

**Post-merge (vector-search.md Section 4.3):**
1. Combine all results into single list
2. Add existing boosts (recency, frequency, context, pinned) to merged_score
3. Apply junk penalty
4. Sort descending, return top N

**Fallback (vector-search.md Section 4.5):**
If semantic unavailable: return lexical results only, treat semantic_weight as 0.0, log at DEBUG.

**Acceptance:** Unit tests with synthetic scores: verify BOTH items score higher than LEXICAL_ONLY or SEMANTIC_ONLY with equal normalized scores. Verify normalization produces [0,1] range. Verify fallback returns lexical-only results unchanged. Verify weights are loaded from settings table.

---

### Sub-Agent 1B: Interaction Tracker + Feedback Aggregator

**Read:** `milestones/m2/feedback-system.md` (Sections 2-5), `foundation/ranking-scoring.md` (Sections 3-4)

**Deliverables:**
```
src/core/feedback/
  interaction_tracker.h/.cpp    - Records and queries user interactions
  feedback_aggregator.h/.cpp    - Periodic feedback -> frequencies rollup
  path_preferences.h/.cpp       - Directory frequency cache (top 50)
  type_affinity.h/.cpp          - File type classification from history
```

**Interactions table (feedback-system.md Section 2.3):**
```sql
CREATE TABLE IF NOT EXISTS interactions (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    query           TEXT NOT NULL,
    query_normalized TEXT NOT NULL,
    item_id         INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    path            TEXT NOT NULL,
    match_type      TEXT NOT NULL,
    result_position INTEGER NOT NULL,
    app_context     TEXT,
    timestamp       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX idx_interactions_query ON interactions(query_normalized);
CREATE INDEX idx_interactions_item ON interactions(item_id);
CREATE INDEX idx_interactions_timestamp ON interactions(timestamp);
```

**Query normalization (feedback-system.md Section 2.4):** lowercase, trim, collapse spaces, strip trailing `*`. Do NOT stem.

**Interaction boost (feedback-system.md Section 2.5):** 5 points per past interaction (same query + same item, last 90 days), capped at 25.

**Path preferences (feedback-system.md Section 3):** Top 50 directories by selection count (last 90 days). Boost: `min(count / 5, 15)`. Cache in memory, refresh every 10 minutes.

**Type affinity (feedback-system.md Section 4):** Classify interactions as code/document/media/other. If one category >60% of total opens (last 30 days), apply +5 boost to matching results.

**Aggregation pipeline (feedback-system.md Section 5):** Run every 60 minutes, on startup, or manual trigger. Rolls up feedback table into frequencies table. Cleanup: delete feedback >90 days, interactions >180 days.

**Privacy controls (feedback-system.md Section 6):** Per-feature toggles (`enableFeedbackLogging`, `enableInteractionTracking`, `enablePathPreferences`, `enableFileTypeAffinity`). When OFF: stop collecting, disable boost, keep existing data.

**Acceptance:** Unit tests: record 10 interactions for same query, verify interaction boost scales correctly and caps at 25. Verify path preferences returns top 50. Verify type affinity detects majority category. Verify aggregation rolls up counts correctly. Verify privacy toggle disables collection.

---

### Sub-Agent 1C: Embedding Pipeline Integration (Stage 8)

**Read:** `milestones/m2/embedding-pipeline.md` (Sections 4-5), `foundation/indexing-pipeline.md` (Stage 8)

**Deliverables:**
```
src/core/embedding/
  embedding_pipeline.h/.cpp  - Batch queue, throttling, Stage 8 orchestration
```

This module connects the EmbeddingManager (Wave 0A) and VectorIndex (Wave 0B) to the existing indexing pipeline (M1's Pipeline class).

**Batch queue (embedding-pipeline.md Section 4.2):**
- Accumulate embedding requests from indexing pipeline
- Fire batch when queue reaches 32 items OR 500ms timeout
- Pad all sequences to longest in batch
- Single `OrtSession::Run` per batch

**Throttling (embedding-pipeline.md Section 7.1):**
- User-active (typed in search in last 30s): pause background embedding, only do query embedding
- Idle: batch at full speed
- Initial build: CPU capped at 50% (`intra_op_num_threads = max(1, cores/2)`)

**Integration with pipeline (embedding-pipeline.md Section 5.1):**
- Stage 8 runs AFTER Stage 7 (FTS5 INSERT), non-blocking
- Takes first chunk text from pipeline output
- For documents: embed and store directly to VectorIndex + vector_map
- For queries: embed and return float32[384] for KNN search
- If queue full (backpressure): Stage 7 still completes, file is FTS5-searchable immediately

**BGE query prefix (embedding-pipeline.md Section 3.2):**
- Document embedding: raw text, no prefix
- Query embedding: prepend `"Represent this sentence for searching relevant passages: "`

**Error handling (embedding-pipeline.md Section 6):**
All failures non-fatal. If single item fails: skip, log WARNING, item remains FTS5-only. If batch fails: retry individually, skip remaining failures. If model missing: `embeddingAvailable = false`, FTS5-only search.

**Acceptance:** Integration test: index 100 files, verify all are in FTS5 AND in vector_map. Verify query embedding includes BGE prefix. Verify throttling pauses background work when user active. Verify single embedding failure doesn't block other items.

---

### Sub-Agent 1D: IPC + Service Extensions

**Read:** `milestones/m2/feedback-system.md` (Section 7), `milestones/m2/embedding-pipeline.md` (Section 5.2), `foundation/ipc-service-boundaries.md`

**Deliverables:** Extensions to existing service binaries.

**New QueryService methods:**
```json
// Record interaction
{"method": "record_interaction", "params": {"query": "config", "item_id": 12345, "path": "/Users/rex/.zshrc", "match_type": "content", "result_position": 3, "app_context": "com.apple.Terminal"}}

// Get path preferences (debug/settings UI)
{"method": "get_path_preferences", "params": {"limit": 50}}

// Get file type affinity
{"method": "get_file_type_affinity", "params": {}}

// Trigger aggregation
{"method": "run_aggregation", "params": {}}

// Export interaction data (privacy)
{"method": "export_interaction_data", "params": {"format": "json"}}
```

**New ExtractorService (or EmbeddingService) methods:**
```json
// Single embedding
{"method": "embed_text", "params": {"item_id": 12345, "text": "...", "is_query": false}}

// Batch embedding
{"method": "embed_batch", "params": {"items": [{"item_id": 12345, "text": "..."}, ...]}}
```

**QueryService search method update:**
The existing `search()` method must be extended to:
1. Run FTS5 search (existing)
2. Embed query (new, parallel with step 1)
3. KNN search (new, after step 2)
4. Merge results (new, after steps 1 and 3)
5. Apply interaction boost (new)
6. Apply path preference boost (new)
7. Apply type affinity boost (new)
8. Return merged, boosted results

**Acceptance:** IPC roundtrip for each new method. search() returns merged semantic+lexical results. record_interaction() stores to interactions table. export_interaction_data() returns valid JSON.

---

## Wave 2: UI (All Parallel, After Wave 1)

### Sub-Agent 2A: Onboarding Flow (QML)

**Read:** `milestones/m2/onboarding-settings-spec.md` (Sections 1, 4)

**Deliverables:**
```
src/app/qml/onboarding/
  OnboardingWindow.qml      - Container with step navigation (back/next)
  WelcomeStep.qml           - Step 1: app intro, single "Get Started" button
  FdaStep.qml               - Step 2: FDA permission flow
  HomeMapStep.qml            - Step 3: directory classification with dropdowns

src/app/
  onboarding_controller.h/.cpp  - State machine for onboarding steps
```

**Step 1 (Welcome):** Static text, app icon, single "Get Started" button. No config.

**Step 2 (FDA):**
- "Open System Settings" button: `open "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"`
- "Verify Access" button: calls `FdaCheck::hasFullDiskAccess()` (M1 module)
- Status indicator: yellow "Not yet granted" or green "Access granted"
- "Skip for now" available with warning text
- "Continue" enabled regardless of FDA status

**Step 3 (Home Map):**
- Enumerate `~/` top-level directories via `QDir::entryList()`
- Apply `suggestedClassification()`:
  - Developer, Projects, Code -> Index + Embed
  - Documents, Desktop -> Index + Embed
  - Cloud folders (iCloud Drive, Dropbox, etc.) -> Index Only
  - .ssh, .gnupg, .aws -> Skip
  - Library, .Trash -> Skip
  - Everything else -> Index Only
- ComboBox dropdown per directory: "Index + Embed", "Index Only", "Skip"
- Selections saved to settings as `indexRoots` array

**Completion:** Save all settings (index roots, FDA status, onboarding_completed=true), start IndexerService + ExtractorService, show search panel with "Indexing in progress..." indicator.

**Resume behavior:** If user quits mid-onboarding, resume from current step on next launch.

**Acceptance:** Onboarding launches on fresh install. All 3 steps navigate correctly. FDA check returns correct status. Home Map enumerates real directories. Clicking "Start Indexing" saves settings and begins indexing. Closing and reopening resumes from saved step.

---

### Sub-Agent 2B: Settings UI (QML, 5 Tabs)

**Read:** `milestones/m2/onboarding-settings-spec.md` (Sections 2, 3)

**Deliverables:**
```
src/app/qml/settings/
  SettingsWindow.qml         - Container with tab bar
  GeneralTab.qml             - Hotkey, launch at login, dock, updates, max results
  IndexingTab.qml            - Roots list, per-root mode, PDF/OCR toggles, embedding toggle
  ExclusionsTab.qml          - Pattern list, .bsignore edit, syntax help, live validation
  PrivacyTab.qml             - Feedback/tracking toggles, retention, sensitive paths, data export
  IndexHealthTab.qml         - Status, stats, roots health, queue, errors, actions

src/app/qml/components/
  HotkeyRecorder.qml         - Custom key capture component
  DirectoryPicker.qml        - Root folder add/remove component
  PatternEditor.qml          - Exclusion pattern list editor

src/app/
  settings_controller.h/.cpp  - QML-exposed settings model
```

**General tab controls:** Hotkey (key recorder), launch at login (toggle), show in Dock (toggle), check for updates (toggle), max results (slider 5-50).

**Indexing tab controls:** Index roots (list with add/remove), per-root mode (dropdown), enable PDF (toggle), enable OCR (toggle), OCR languages (multi-select), enable semantic search (toggle), embedding model (read-only label), max file size (slider 1-500 MB), pause indexing (button).

**Exclusions tab:** Pattern list (editable), .bsignore path (read-only + edit button opening `~/.bsignore` in default editor), syntax help (collapsible), live validation (show match count as user types).

**Privacy tab:** Feedback logging toggle, interaction tracking toggle, retention dropdown (30/60/90/180 days), sensitive paths (editable list), clear all feedback data (with confirmation dialog), export my data (button, saves JSON to Downloads).

**Index Health tab:** See Sub-Agent 2C.

**Settings persistence (onboarding-settings-spec.md Section 3):**
- Write to `~/Library/Application Support/betterspotlight/settings.json`
- Emit `settingsChanged(key)` signal
- Services receive changes via IPC `update_settings` method
- No restart required (exception: hotkey re-registration is in-process)

**Acceptance:** All 5 tabs render. Settings changes persist to disk. App restart loads saved settings. Changing index root triggers indexing. Hotkey recorder captures and validates key combinations. Live pattern validation shows match count.

---

### Sub-Agent 2C: Index Health Dashboard (QML)

**Read:** `milestones/m2/onboarding-settings-spec.md` (Section 2.5), `milestones/m2/vector-search.md` (Section 6)

**Deliverables:** The IndexHealthTab.qml component and its backing C++ model.

**Dashboard sections:**
1. **Status indicator:** Green "Healthy" / Yellow "Degraded" (>5% failures or embedding <100%) / Red "Unhealthy" (>20% failures or integrity failed) / Blue "Rebuilding"
2. **Statistics:** Indexed files, content chunks, embedded vectors (count + percent), database size, vector index size, last scan time
3. **Index Roots:** Per-root file count and health status
4. **Queue:** Pending, in-progress, embedding queue depth
5. **Recent Errors:** Scrollable list with path and error message, "Show all..." link
6. **Actions:** Reindex Folder (folder picker), Rebuild All (confirmation), Rebuild Vector Index (confirmation), Clear Cache

**Vector index health data (vector-search.md Section 6):**
```json
{
  "vectorIndex": {
    "available": true,
    "totalVectors": 487231,
    "deletedVectors": 12044,
    "deletionRatio": 0.025,
    "modelVersion": "bge-small-en-v1.5",
    "indexFileSizeMB": 198,
    "lastPersisted": "2026-02-07T14:30:00Z",
    "needsRebuild": false,
    "embeddingProgress": 1.0
  }
}
```

**Refresh:** Button calls `get_health` IPC on all services.

**Actions:**
- Reindex Folder: native folder picker, sends `reindex` IPC with path
- Rebuild All: confirmation dialog, drops + recreates all index tables
- Rebuild Vector Index: confirmation dialog, triggers hnswlib rebuild (vector-search.md Section 2.4)
- Clear Cache: removes CoreML compiled model cache + hnswlib temp files, does NOT affect index data

**Acceptance:** Dashboard loads real data from services. Status indicator reflects actual health. Refresh button updates all stats. Reindex Folder triggers re-indexing of selected path. Vector index stats display correctly.

---

## Wave 3: Testing + Integration (All Parallel, After Wave 2)

### Sub-Agent 3A: Relevance Test Infrastructure

**Read:** `milestones/m2/relevance-testing.md` (entire document)

**Deliverables:**
```
tests/relevance/
  test_corpus.json           - 50 queries (from relevance-testing.md Section 3)
  run_relevance_test.sh      - Test runner script
  baselines.json             - Initial baseline (empty, populated on first run)

tests/fixtures/
  standard_home_v1/          - Controlled fixture (~500 files, ~50MB)
  generate_standard_home.sh  - Fixture regeneration script
```

**Test runner (relevance-testing.md Section 5):**
- Modes: `fts5` (baseline), `semantic` (M2), `both` (comparison)
- Index the fixture, run all 50 queries, capture top-3 results per query
- Compare against expected_files, score PASS/FAIL
- Produce CSV output + summary report to stdout
- Exit 0 if pass_rate >= 0.80, exit 1 otherwise

**A/B comparison (relevance-testing.md Section 4.4):**
- Run FTS5-only, record scores
- Run FTS5+semantic, record scores
- Report delta per category and total
- Fail if FTS5+semantic total <= FTS5-only total (semantic must improve)

**Fixture creation:**
- Build all files listed in relevance-testing.md Section 2.2
- Ensure content matches the verbatim requirements in Section 2.3
- Binary files (PDF, XLSX) must be real, parseable files

**CI integration (relevance-testing.md Section 7):**
- GitHub Actions step after build
- Upload results as artifact
- Gate M2 PRs that touch scoring/ranking/embedding/merge

**Acceptance:** run_relevance_test.sh executes end-to-end on the fixture. CSV output is correct. A/B comparison produces meaningful delta. CI step runs and gates correctly.

---

### Sub-Agent 3B: M2 Unit + Integration Tests

**Read:** `milestones/acceptance-criteria.md` (M2 Testing section)

**Deliverables:**
```
tests/unit/
  test_tokenizer.cpp          - WordPiece tokenization verification
  test_embedding.cpp          - Inference output shape, normalization, determinism
  test_quantizer.cpp          - Quantize/dequantize roundtrip accuracy
  test_vector_index.cpp       - hnswlib add/delete/search/persist
  test_search_merger.cpp      - Score normalization, merge categories, fallback
  test_interaction_tracker.cpp - Record, query, boost calculation, normalization
  test_feedback_aggregator.cpp - Rollup correctness, cleanup retention
  test_path_preferences.cpp   - Top-50 cache, boost calculation
  test_type_affinity.cpp      - Category detection, threshold logic

tests/integration/
  test_semantic_search.cpp    - Query "configuration" finds "settings.ini" (no keyword match)
  test_embedding_fallback.cpp - Disable model, verify FTS5-only works
  test_boost_verification.cpp - Open file 5x, search, verify top-3 appearance
  test_context_boost.cpp      - Mock IDE frontmost, verify code files ranked higher
  test_embedding_recovery.cpp - Disable model, search works, re-enable, semantic works
```

**Key test scenarios from acceptance criteria:**
- Scoring merger: lexical + semantic scores combine correctly, weights respected
- Embedding inference: same input always produces same output (deterministic)
- Boost calculation: frequency, recency, interaction, app-context all apply correctly
- Fallback: embedding fails -> FTS5-only results without crash
- Semantic search: "configuration" -> finds "settings.ini" (no exact match)
- Relevance: run test corpus, pass rate >= 80%
- Context boost: IDE frontmost -> code files rank higher

**Acceptance:** All unit tests pass. All integration tests pass. No crashes in any test. Test coverage for new M2 code > 70%.

---

### Sub-Agent 3C: Stress Test Extension

**Read:** `milestones/acceptance-criteria.md` (M2 Stress Test section)

**Deliverables:**
```
tests/benchmarks/
  stress_8h_m2.sh            - Extended 8-hour stress test with M2 scenarios
```

**M2-specific stress scenarios (in addition to M1 scenarios):**
- Rapid semantic + lexical searches alternating
- Embedding queue saturation (submit 10,000 items, verify no crash)
- Kill embedding service mid-batch, verify recovery
- Settings toggle: enable/disable semantic search during active use
- Index Health dashboard polling during heavy indexing
- Interaction recording under high search load

**Expected outcomes (8 hours):**
- Zero UI crashes
- Zero indexer crashes
- Zero silent failures
- No index corruption (PRAGMA integrity_check passes)
- Memory: no unbounded growth
- All semantic searches return results or gracefully fall back

**Acceptance:** Script runs for 8 hours without crash. Produces structured log with crash count, memory graph, latency histogram. Exit code 0 if zero crashes.

---

## Integration Points (Cross-Stream Contracts)

| Producer | Consumer | Interface | Contract |
|----------|----------|-----------|----------|
| EmbeddingManager (0A) | EmbeddingPipeline (1C) | `embed(text) -> float[384]` | Returns L2-normalized embedding or error |
| VectorIndex (0B) | SearchMerger (1A) | `search(query_vec, K) -> [(label, dist)]` | Returns K nearest, distance = 1 - cosine_sim |
| VectorStore (0B) | SearchMerger (1A) | `getItemId(label) -> item_id` | Maps hnsw_label to item_id |
| SearchMerger (1A) | QueryService (1D) | `merge(lexical[], semantic[]) -> merged[]` | Combined scored results, fallback if semantic empty |
| InteractionTracker (1B) | QueryService (1D) | `getBoost(query, item_id) -> int` | 0-25 point boost based on past interactions |
| PathPreferences (1B) | QueryService (1D) | `getBoost(dir) -> int` | 0-15 point boost for frequently selected directories |
| TypeAffinity (1B) | QueryService (1D) | `getBoost(extension) -> int` | 0 or 5 points if matches primary affinity |
| EmbeddingPipeline (1C) | Pipeline (M1) | Stage 8 callback | Non-blocking, first chunk text -> queue for embedding |
| SettingsController (2B) | All services | `settingsChanged(key)` signal + IPC | Services reload config without restart |
| IndexHealthTab (2C) | All services | `get_health` IPC | Returns health JSON including vector index stats |

---

## M2 Performance Gates

These numbers come from acceptance-criteria.md M2 section.

| Metric | Threshold | How to Measure |
|--------|-----------|----------------|
| Combined search P50 (lexical+semantic) | < 80ms | Relevance test runner timing |
| Combined search P95 (lexical+semantic) | < 300ms | Relevance test runner timing |
| Single embedding (Apple Silicon, CoreML) | < 15ms | Unit test timing |
| Single embedding (Apple Silicon, CPU) | < 50ms | Unit test timing |
| Batch of 32 (Apple Silicon, CoreML) | < 200ms | Unit test timing |
| KNN search (500K vectors, K=50) | < 5ms | Benchmark script |
| Embedding memory (ONNX session + model) | < 150MB RSS | Activity Monitor |
| Vector index memory (500K files) | < 500MB | Activity Monitor |
| Database size (500K files, with vectors) | < 2.5GB | stat on index.db + vectors.hnsw |
| Relevance test pass rate | >= 80% (40/50) | run_relevance_test.sh |
| 8-hour stress test crashes | 0 | stress_8h_m2.sh |

---

## What Is NOT In M2 Scope

Do not build these. They are M3 or later.

- Learned ranking (training a model on feedback data)
- Multi-language stemming (English only)
- Multi-chunk embedding (first chunk only for M2)
- Memory-mapped hnswlib loading (optimize in M3 if needed)
- Sliding window embedding for long documents
- Online weight learning from user feedback
- Custom theme or visual polish for onboarding/settings
- Clipboard monitoring and boost
- Project graph inference
- Auto-update mechanism (Sparkle)
- Encryption at rest
- DOCX/XLSX/PPTX content extraction
- Per-directory .bsignore files
- 100-query expanded test corpus (M3)
- 95% relevance threshold (M3)
- 48-hour stress test (M3)

---

## Sub-Agent Parallelization Summary

```
Wave 0 (ML Foundation)       Wave 1 (Core M2 Modules)        Wave 2 (UI)               Wave 3 (Testing)
─────────────────────       ────────────────────────        ──────────                ────────────────
┌──────────────────┐        ┌───────────────────┐           ┌──────────────────┐      ┌──────────────────┐
│ 0A: ONNX Runtime │───────>│ 1A: Search Merger │──────────>│ 2A: Onboarding   │─────>│ 3A: Relevance    │
│     + Tokenizer  │        │    (Merge Algo)   │           │    Flow (QML)    │      │    Test Infra    │
└──────────────────┘        └───────────────────┘           └──────────────────┘      └──────────────────┘
┌──────────────────┐        ┌───────────────────┐           ┌──────────────────┐      ┌──────────────────┐
│ 0B: hnswlib      │───────>│ 1B: Interaction   │──────────>│ 2B: Settings UI  │─────>│ 3B: M2 Unit +    │
│     + VectorStore│        │    Tracker +      │           │    (5 Tabs, QML) │      │    Integration   │
└──────────────────┘        │    Feedback Agg   │           └──────────────────┘      └──────────────────┘
                            └───────────────────┘           ┌──────────────────┐      ┌──────────────────┐
                            ┌───────────────────┐           │ 2C: Index Health │─────>│ 3C: Stress Test  │
                            │ 1C: Embedding     │──────────>│    Dashboard     │      │    Extension     │
                            │    Pipeline (S8)  │           └──────────────────┘      └──────────────────┘
                            └───────────────────┘
                            ┌───────────────────┐
                            │ 1D: IPC + Service │──────────────────┘
                            │    Extensions     │
                            └───────────────────┘

Total sub-agents: 12 (2 + 4 + 3 + 3 per wave)
Max parallel at any point: 4 (Wave 1)
Estimated wall-clock with full parallelization: 5-8 days (2-person team)
```

---

## Final Notes for Engineers

1. **FTS5 remains king.** Semantic search is an enhancement, not a replacement. If semantic search makes any FTS5-passing query regress, the merge weights are wrong. Fix the weights, don't remove FTS5.

2. **All ML failures are non-fatal.** No user should ever see an error caused by embedding failure, model loading failure, or vector index corruption. The system degrades to FTS5-only silently and recovers when the ML component is restored.

3. **The relevance test is the single most important deliverable in M2.** Without it, we have no way to know if semantic search actually helps. Build it first, run it often, use it to tune weights.

4. **Batch before singleton.** During initial indexing, always embed in batches of 32. Single-item embedding is only for query-time. The performance difference is 5-10x.

5. **When in doubt, the M2 doc wins.** If M2 docs and foundation docs disagree on an M2 topic, the M2 doc is authoritative (it was written later and accounts for decisions made during M1). For M1 topics, foundation docs still win.

6. **Test with the fixture, not random data.** The `standard_home_v1` fixture exists for a reason. Every semantic query in the corpus was designed to have exactly one correct answer in that fixture. Random test data produces random results.
