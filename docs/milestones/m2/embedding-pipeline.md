# M2: Embedding Pipeline Specification

**Scope:** ONNX Runtime integration, BGE-small-en-v1.5 inference, tokenization, batching, quantization, error handling.

**References:** This document extends [foundation/indexing-pipeline.md](../../foundation/indexing-pipeline.md) Stage 8 and [foundation/storage-schema.md](../../foundation/storage-schema.md) Section 3.12. It does NOT repeat parameters already specified there.

---

## 1. Model Specification

| Property | Value |
|----------|-------|
| Model | BGE-small-en-v1.5 (BAAI) |
| Architecture | BERT-based bi-encoder |
| Parameters | ~33.4M |
| Output dimensions | 384 (float32) |
| Max input tokens | 512 |
| License | MIT |
| ONNX file size | ~130MB (float32), ~35MB (int8 dynamic quantized) |
| Recommended ONNX variant | int8 dynamic quantized (35MB bundle) |

### 1.1 Why BGE-small-en-v1.5

Decision is recorded in the foundation docs. Summary: best quality-to-size ratio for English-language file search on consumer hardware. E5-small-v2 is the fallback candidate if BGE licensing changes. ADR to be filed if model changes.

### 1.2 Model Bundling

The ONNX model file ships inside the application bundle at `BetterSpotlight.app/Contents/Resources/models/bge-small-en-v1.5-int8.onnx`. No first-run download. This ensures offline-first operation and eliminates network failure modes.

Vocabulary file (WordPiece tokenizer): `BetterSpotlight.app/Contents/Resources/models/vocab.txt` (~230KB, from BERT-base-uncased).

---

## 2. ONNX Runtime Session Lifecycle

### 2.1 Session Creation

```
Startup sequence:
1. ExtractorService (or dedicated EmbeddingService) starts
2. Check model file exists at bundle path
3. Create OrtEnvironment (singleton, process lifetime)
4. Configure OrtSessionOptions:
   - intra_op_num_threads = 2 (limit CPU contention)
   - inter_op_num_threads = 1
   - execution_mode = ORT_SEQUENTIAL
   - graph_optimization_level = ORT_ENABLE_ALL
   - (Apple Silicon) AppendExecutionProvider_CoreML(COREML_FLAG_USE_CPU_AND_GPU)
5. Create OrtSession from model path
6. Validate input/output shapes (input: [batch, seq_len], output: [batch, 384])
7. Log: model loaded, session ready, provider (CPU/CoreML)
```

If step 2 fails (model file missing), log error and set `embeddingAvailable = false`. All embedding requests return empty vectors. Search falls back to FTS5-only. This is not a fatal error.

If step 5 fails (ONNX parse error, incompatible model), same fallback. Log at ERROR level.

### 2.2 Session Teardown

Session is destroyed when the embedding service process exits. No explicit cleanup needed beyond RAII (OrtSession destructor handles resource release). Do NOT recreate sessions during normal operation.

### 2.3 CoreML Acceleration

On Apple Silicon (M1/M2/M3/M4), ONNX Runtime's CoreML execution provider offloads compatible ops to the Neural Engine. This gives ~3-5x speedup over CPU-only inference for BERT-like models.

Configuration:
- Use `COREML_FLAG_USE_CPU_AND_GPU` (not `COREML_FLAG_USE_CPU_ONLY`)
- CoreML compilation happens on first inference (~5-10 seconds one-time cost)
- Cache compiled model: set `ORT_COREML_FLAG_CREATE_MLPROGRAM` to enable caching at `~/Library/Caches/betterspotlight/coreml/`
- Fallback: if CoreML provider fails to load, ONNX Runtime automatically falls back to CPU provider. Log a warning but do not treat as error.

---

## 3. Tokenization

### 3.1 WordPiece Tokenizer

BGE-small-en-v1.5 uses BERT's WordPiece tokenizer with `vocab.txt` (30,522 tokens). We implement a minimal C++ tokenizer rather than depending on HuggingFace Tokenizers (avoids Python/Rust dependency).

Implementation requirements:

1. **Load vocabulary** from `vocab.txt` into `std::unordered_map<std::string, int>` at startup
2. **Normalize** input: lowercase, strip accents (NFD decomposition, remove combining marks), collapse whitespace
3. **Tokenize**: split on whitespace, then apply WordPiece algorithm:
   - For each word, greedily find longest matching subword in vocab
   - If no match found, mark as `[UNK]` (token ID 100)
   - Prefix continuation tokens with `##`
4. **Add special tokens**: `[CLS]` (101) at start, `[SEP]` (102) at end
5. **Truncate** to 512 tokens (including special tokens, so 510 content tokens max)
6. **Pad** shorter sequences to batch max length with `[PAD]` (0)
7. **Generate attention mask**: 1 for real tokens, 0 for padding
8. **Generate token type IDs**: all zeros (single-segment input)

### 3.2 BGE-specific Query Prefix

BGE models expect a task-specific prefix for queries (not for documents):
- **Document embedding** (indexing time): no prefix, raw text
- **Query embedding** (search time): prepend `"Represent this sentence for searching relevant passages: "` to the query text

This prefix is critical for BGE retrieval quality. Without it, query-document similarity drops significantly.

### 3.3 Input Text Preparation

Before tokenization, prepare input text:

```
For indexing (document embedding):
  input = first_chunk_content  (already 500-2000 chars from Chunker)

For search (query embedding):
  input = "Represent this sentence for searching relevant passages: " + user_query
```

If the first chunk exceeds 510 WordPiece tokens after tokenization, it is truncated. No sliding window, no multi-chunk embedding for M2. First-chunk-only is the strategy (per foundation docs).

---

## 4. Inference Pipeline

### 4.1 Single-Item Flow

```
Text (string)
  → Tokenize (token_ids[], attention_mask[], token_type_ids[])
  → OrtSession::Run(inputs, outputs)
  → Raw embedding (float32[384])
  → L2-normalize (unit vector)
  → Quantize to int8 (scale + zero_point + int8[384])
  → Store in hnswlib index + vector_map table
```

### 4.2 Batch Inference

Batch size: 32 (configurable via `embedding_batch_size` in settings).

Batching strategy:
1. Accumulate embedding requests in a queue (from indexing pipeline)
2. When queue reaches batch size OR 500ms timeout expires, form a batch
3. Pad all sequences in batch to the length of the longest sequence
4. Run inference on batch (single `OrtSession::Run` call)
5. Extract per-item embeddings from output tensor
6. Normalize and quantize each embedding individually

Padding to uniform length within each batch is required because ONNX Runtime expects fixed-shape tensors. Dynamic shapes per batch are supported (batch can be any size, sequence length can vary between batches).

### 4.3 L2 Normalization

After inference, each embedding vector must be L2-normalized to a unit vector. This is required for cosine similarity to equal dot product (hnswlib uses inner product space).

```
for each dimension i:
  norm += v[i] * v[i]
norm = sqrt(norm)
for each dimension i:
  v[i] = v[i] / norm
```

### 4.4 Int8 Quantization

After normalization, quantize float32 embeddings to int8 for storage efficiency:

```
For each embedding vector v[384]:
  min_val = min(v)
  max_val = max(v)
  scale = (max_val - min_val) / 255.0
  zero_point = round(-min_val / scale)
  for each dimension i:
    q[i] = clamp(round(v[i] / scale) + zero_point, 0, 255) - 128

Store: scale (float32), zero_point (int8), q[384] (int8)
Total per vector: 4 + 1 + 384 = 389 bytes
```

For hnswlib search, dequantize back to float32 at query time. The index itself stores float32 for search accuracy; int8 is for disk persistence and memory-mapped loading.

---

## 5. Integration Points

### 5.1 Indexing Pipeline Integration

The embedding step runs AFTER FTS5 indexing (Stage 7 in indexing-pipeline.md), as a separate Stage 8:

```
Stage 7: FTS5 INSERT (chunks) → immediate, synchronous
Stage 8: Embedding (first chunk only) → async, lower priority
```

Stage 8 is non-blocking. If the embedding queue is full (backpressure), Stage 7 still completes. Files are searchable via FTS5 immediately; semantic search becomes available once embedding completes.

Priority: embedding runs at lower CPU/IO priority than FTS5 indexing. On macOS, use `setpriority(PRIO_PROCESS, 0, 10)` or QThread::LowPriority for the embedding thread.

### 5.2 IPC Interface

New methods on ExtractorService (or dedicated EmbeddingService):

```json
// Request
{
  "method": "embed_text",
  "params": {
    "item_id": 12345,
    "text": "first chunk content...",
    "is_query": false
  }
}

// Response
{
  "result": {
    "item_id": 12345,
    "embedding": [0.023, -0.041, ...],  // 384 floats (only for queries)
    "stored": true                       // for documents, embedding stored directly
  }
}

// Batch request
{
  "method": "embed_batch",
  "params": {
    "items": [
      {"item_id": 12345, "text": "..."},
      {"item_id": 12346, "text": "..."}
    ]
  }
}
```

For document embedding during indexing, the embedding is stored directly to the vector index. The response only confirms storage. For query embedding during search, the float32 vector is returned in the response for KNN search.

### 5.3 Delete Handling

When a file is deleted from the index:
1. Remove from `vector_map` table (SQL DELETE)
2. Mark the hnswlib label as deleted (hnswlib's `markDelete()`)
3. Do NOT rebuild the index immediately

Orphaned hnswlib entries are cleaned up during periodic index maintenance (see vector-search.md).

---

## 6. Error Handling

| Failure Mode | Behavior | Recovery |
|---|---|---|
| Model file missing | `embeddingAvailable = false`, log ERROR | FTS5-only search. User sees "Semantic search unavailable" in Index Health. |
| ONNX session creation fails | Same as above | Same |
| CoreML provider unavailable | Fall back to CPU provider, log WARNING | Slower inference (~3-5x), but functional |
| Single inference fails (OOM, timeout) | Skip this item's embedding, log WARNING | Item searchable via FTS5 only. Retry on next full reindex. |
| Batch inference fails | Retry items individually, then skip remaining failures | Partial batch success is acceptable |
| Tokenizer vocab missing | `embeddingAvailable = false`, log ERROR | Same as model missing |

All failures are non-fatal. The system must remain fully functional for FTS5 search regardless of embedding status.

---

## 7. Performance Targets

| Metric | Target | Measurement |
|---|---|---|
| Single embedding latency (Apple Silicon, CoreML) | < 15ms | Wall clock, inference only |
| Single embedding latency (Apple Silicon, CPU) | < 50ms | Wall clock, inference only |
| Single embedding latency (Intel, CPU) | < 100ms | Wall clock, inference only |
| Batch of 32 (Apple Silicon, CoreML) | < 200ms | Wall clock |
| Batch of 32 (Apple Silicon, CPU) | < 1s | Wall clock |
| Tokenization latency (512 tokens) | < 1ms | Wall clock |
| Memory (ONNX session + model) | < 150MB RSS | Peak during batch inference |
| Memory (idle, session loaded) | < 80MB RSS | Steady state |

### 7.1 Throttling

During user-active periods (user has typed in search panel in last 30 seconds):
- Pause background embedding
- Only run query embedding (single item, not batch)
- Resume background embedding when user goes idle

During initial index build:
- Batch embedding runs at full speed (user expects it to take time)
- CPU usage capped at 50% of available cores (`intra_op_num_threads = max(1, cores/2)`)

---

## 8. Model Upgrade Path

When a new model version is adopted (e.g., BGE-small-en-v1.5 -> BGE-small-en-v2):

1. Bundle new model in app update
2. On first launch after update, detect model version change via `model_version` column in `vector_map`
3. Set `reembedding_required = true` in settings
4. Background re-embedding starts: iterate all items, re-embed first chunks, update `vector_map.model_version`
5. During re-embedding, semantic search uses OLD embeddings (stale but functional)
6. When re-embedding completes, rebuild hnswlib index from scratch
7. Clear `reembedding_required`

Re-embedding 500K files at 32/batch with CoreML: ~(500000/32) * 0.2s = ~52 minutes. Acceptable as background task.

---

## 9. File Layout

New files for M2 embedding:

```
src/core/embedding/
  embedding_manager.h      // Public API: embed(), embedBatch(), embedQuery()
  embedding_manager.cpp    // ONNX session, batching, error handling
  tokenizer.h              // WordPiece tokenizer
  tokenizer.cpp            // Vocab loading, tokenization, special tokens
  quantizer.h              // Float32 -> int8 quantization
  quantizer.cpp            // Quantize, dequantize utilities

Resources/models/
  bge-small-en-v1.5-int8.onnx   // ONNX model file (~35MB)
  vocab.txt                       // WordPiece vocabulary (~230KB)
```
