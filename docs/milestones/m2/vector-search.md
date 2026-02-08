# M2: Vector Search & Semantic-Lexical Merge

**Scope:** hnswlib index lifecycle, persistence, KNN search, score normalization, and the algorithm that merges semantic results with FTS5 lexical results.

**References:** Extends [foundation/storage-schema.md](../../foundation/storage-schema.md) Section 3.12 (vector_map table) and [foundation/ranking-scoring.md](../../foundation/ranking-scoring.md) Section 6 (semantic boost). Does not repeat parameters already specified there.

---

## 1. hnswlib Configuration

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Space | Inner Product (`hnswlib::InnerProductSpace`) | L2-normalized vectors make IP equivalent to cosine similarity |
| Dimensions | 384 | BGE-small-en-v1.5 output size |
| M | 16 | Connectivity parameter. 16 is standard for recall/speed balance at our scale |
| efConstruction | 200 | Build-time search breadth. Higher = better recall, slower build |
| efSearch | 50 | Query-time search breadth. 50 gives >95% recall@10 at our scale |
| Max elements | Dynamically sized | Initial capacity 100K, resize by 2x when 80% full |

### 1.1 Why Inner Product Space

BGE embeddings are L2-normalized after inference (see embedding-pipeline.md Section 4.3). For unit vectors, inner product equals cosine similarity. Using IP space avoids the overhead of computing cosine at search time while producing identical rankings.

---

## 2. Index Lifecycle

### 2.1 Creation

On first M2 launch (or when `vectors.hnsw` doesn't exist):

```
1. Create hnswlib::HierarchicalNSW<float>(space, initial_capacity=100000, M=16, efConstruction=200)
2. Create empty vector_map table (if not exists)
3. Set embedding_enabled = true in settings
4. Begin background embedding of all indexed files (first chunk only)
```

### 2.2 Adding Vectors

When a file is newly indexed or re-indexed:

```
1. Receive float32[384] embedding from EmbeddingManager
2. Assign hnswlib label = next_label++ (monotonic counter, stored in settings table)
3. index.addPoint(embedding, label)
4. INSERT INTO vector_map (item_id, hnsw_label, model_version, embedded_at) VALUES (?, ?, ?, ?)
5. Persist index to disk periodically (every 1000 additions or every 60 seconds)
```

The `hnsw_label` is an internal integer ID used by hnswlib. It maps to `item_id` via the `vector_map` table. This indirection is necessary because hnswlib labels must be dense integers, while item_ids may be sparse.

### 2.3 Deleting Vectors

When a file is deleted from the main index:

```
1. SELECT hnsw_label FROM vector_map WHERE item_id = ?
2. index.markDelete(hnsw_label)
3. DELETE FROM vector_map WHERE item_id = ?
```

`markDelete()` does not reclaim space in the hnswlib index. Deleted labels accumulate until a rebuild. Track `deleted_count` in the settings table.

### 2.4 Rebuild Triggers

Rebuild the hnswlib index from scratch when:
- `deleted_count / total_count > 0.20` (20% deleted)
- Model version changes (re-embedding)
- User triggers manual rebuild from Index Health UI
- Index file is corrupted (fails validation on load)

Rebuild process:
```
1. Create new empty index with current capacity
2. SELECT item_id, hnsw_label FROM vector_map WHERE model_version = current_version
3. For each entry, load embedding from old index (or re-embed if old index corrupt)
4. addPoint() to new index with fresh sequential labels
5. UPDATE vector_map SET hnsw_label = new_label
6. Persist new index, delete old index
7. Reset deleted_count = 0
```

During rebuild, search uses the old index. Swap is atomic (rename new file over old).

### 2.5 Persistence

Two files on disk:

**`vectors.hnsw`** -- hnswlib's native binary serialization
- Location: `~/.local/share/betterspotlight/vectors.hnsw`
- Written via `index.saveIndex(path)`
- Loaded via `index.loadIndex(path, space, max_elements)`
- File size: ~(389 bytes/vector * N) + index structure overhead. For 500K vectors: ~200MB.

**`vectors.meta`** -- JSON metadata
```json
{
  "version": 1,
  "model": "bge-small-en-v1.5",
  "dimensions": 384,
  "total_elements": 487231,
  "deleted_elements": 12044,
  "next_label": 499275,
  "ef_construction": 200,
  "m": 16,
  "last_persisted": "2026-02-07T14:30:00Z"
}
```

**`vector_map`** SQLite table (in main index.db):
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

### 2.6 Loading on Startup

```
1. Check vectors.meta exists and is valid JSON
2. Check vectors.hnsw exists and file size > 0
3. Load vectors.meta, verify model_version matches current model
4. If model mismatch: trigger re-embedding (see embedding-pipeline.md Section 8)
5. index.loadIndex(path, space, meta.total_elements + growth_headroom)
6. Set efSearch = 50
7. Log: loaded N vectors, M deleted, index ready
```

If any step fails, set `vectorIndexAvailable = false`. Semantic search disabled, FTS5-only. Not fatal.

---

## 3. Semantic Search Flow

### 3.1 Query Processing

```
User types query: "database migration"
  → SearchController sends IPC to QueryService
  → QueryService:
      1. FTS5 search (existing M1 path)  →  lexical_results[]
      2. Embed query text               →  query_vector[384]
      3. KNN search                      →  semantic_results[]
      4. Merge results                   →  final_results[]
      5. Return final_results to UI
```

Steps 1 and 2 execute in parallel (independent). Step 3 requires step 2. Step 4 requires steps 1 and 3.

### 3.2 KNN Search

```
query_vector = EmbeddingManager.embedQuery("database migration")
  // Note: query text gets BGE prefix prepended automatically

knn_results = index.searchKnn(query_vector, K=50)
  // Returns: [(hnsw_label, distance), ...] sorted by distance ascending
  // For IP space, distance = 1 - cosine_similarity

For each (label, distance) in knn_results:
  cosine_similarity = 1.0 - distance
  if cosine_similarity < similarity_threshold (0.7):
    discard  // Below quality threshold
  item_id = SELECT item_id FROM vector_map WHERE hnsw_label = label
  semantic_results.append({item_id, cosine_similarity})
```

K=50 is intentionally generous. The similarity threshold (0.7) filters out low-quality matches. Typical result set after filtering: 5-20 items.

---

## 4. Semantic-Lexical Merge Algorithm

This is the core M2 algorithm. It combines two independently scored result sets into a single ranked list.

### 4.1 Score Normalization

FTS5 BM25 scores and cosine similarity scores are on different scales. Before merging, normalize both to [0, 1]:

**Lexical scores:**
```
For each lexical result:
  normalized_lexical = result.finalScore / max_lexical_score
  // max_lexical_score = highest score in the lexical result set
  // If only 1 result, normalized = 1.0
```

**Semantic scores:**
```
For each semantic result:
  normalized_semantic = (cosine_similarity - threshold) / (1.0 - threshold)
  // Maps [threshold, 1.0] to [0.0, 1.0]
  // threshold = 0.7 (configurable)
  // Similarity of 0.7 → 0.0, Similarity of 1.0 → 1.0
```

### 4.2 Merge Strategy

Three categories of results after both searches complete:

1. **Both** -- item appears in BOTH lexical and semantic results
2. **Lexical only** -- item appears only in FTS5 results
3. **Semantic only** -- item appears only in KNN results

Scoring per category:

```
Category: BOTH
  merged_score = (lexical_weight * normalized_lexical) + (semantic_weight * normalized_semantic)
  // lexical_weight = 0.6, semantic_weight = 0.4 (configurable)
  // Items in both sets get a natural advantage

Category: LEXICAL_ONLY
  merged_score = lexical_weight * normalized_lexical
  // No semantic component

Category: SEMANTIC_ONLY
  merged_score = semantic_weight * normalized_semantic
  // No lexical component. These are the "no keyword match but conceptually related" results.
```

### 4.3 Final Ranking

After merge scoring:
1. Combine all results into a single list
2. Add existing boosts (recency, frequency, context, pinned) to `merged_score`
3. Apply junk penalty
4. Sort by total score descending
5. Return top N results (default 20, configurable)

The existing M1 scoring formula still applies on top of the merged score. The `baseMatchScore` component from M1 is replaced by `merged_score` in M2. All other boosts/penalties remain unchanged.

### 4.4 Weights and Tuning

| Parameter | Default | Range | Stored In |
|-----------|---------|-------|-----------|
| `lexical_weight` | 0.6 | 0.0 - 1.0 | settings table |
| `semantic_weight` | 0.4 | 0.0 - 1.0 | settings table |
| `similarity_threshold` | 0.7 | 0.5 - 0.95 | settings table |
| `knn_k` | 50 | 10 - 200 | settings table |
| `ef_search` | 50 | 10 - 200 | settings table |

Constraint: `lexical_weight + semantic_weight` does not need to equal 1.0. They are independent multipliers, not a distribution. A sum > 1.0 means items appearing in both sets score higher than either alone (desirable).

### 4.5 Fallback Behavior

If semantic search is unavailable (model not loaded, index empty, inference error):
- Return lexical results only
- Do not apply semantic_weight (treat as 0.0)
- Log at DEBUG level (not ERROR, since this is expected during initial embedding)
- UI shows "Semantic search building..." in status if embeddings are in progress

---

## 5. Performance Targets

| Metric | Target | Notes |
|---|---|---|
| KNN search (500K vectors, K=50) | < 5ms | hnswlib is extremely fast at this scale |
| Query embedding | < 50ms | Single item, Apple Silicon |
| Merge algorithm | < 1ms | In-memory set operations |
| Total semantic+lexical search P50 | < 80ms | Including FTS5 + embed + KNN + merge |
| Total semantic+lexical search P95 | < 300ms | Upper bound |
| Vector index memory (500K vectors) | < 500MB | Can be reduced with memory-mapping |

### 5.1 Memory-Mapped Loading (Optional Optimization)

If memory budget is tight (user has many open applications), hnswlib supports memory-mapped loading. This reduces RSS by loading index pages on-demand from disk.

Implementation: `mmap()` the `vectors.hnsw` file instead of loading into heap. hnswlib's `loadIndex` supports this via a custom allocator. Defer to M3 if M2 memory targets are met without it.

---

## 6. Index Health Reporting

The vector index contributes to the Index Health dashboard:

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
    "embeddingProgress": 1.0  // 0.0-1.0 during initial embedding
  }
}
```

---

## 7. File Layout

```
src/core/vector/
  vector_index.h          // hnswlib wrapper: add, delete, search, persist, load
  vector_index.cpp
  vector_store.h          // vector_map table CRUD
  vector_store.cpp
  search_merger.h         // Semantic+lexical merge algorithm
  search_merger.cpp
```
