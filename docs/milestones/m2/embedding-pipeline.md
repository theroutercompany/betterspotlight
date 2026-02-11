# M2/M3: Embedding Pipeline Specification (Current Runtime)

**Status:** Active
**Last Updated:** 2026-02-11
**Scope:** Runtime embedding inference, provider policy, vector generation, migration behavior, and guardrails.

---

## 1. Runtime Defaults

- Primary embedding role: `bi-encoder` (`generation=v2`)
- Primary model posture: `1024d float32`
- Primary provider policy: `CoreML preferred` on Apple Silicon
- Fallback policy: CPU provider allowed when CoreML attach fails
- Legacy posture: `generation=v1` (`384d int8`) remains readable for migration safety only

Manifest source of truth:
- `/Users/rexliu/betterspotlight/data/models/manifest.json`

---

## 2. Provider Policy

`ModelSession` resolves execution provider per manifest policy:

1. Attempt CoreML when `preferCoreMl=true` and platform supports it.
2. Fall back to CPU when CoreML is unavailable or attach fails and `allowCpuFallback=true`.
3. Expose selected provider in runtime diagnostics.

Runtime override:
- `BETTERSPOTLIGHT_DISABLE_COREML=1` forces CPU path without rebuild.

Build-time default toggle:
- `BETTERSPOTLIGHT_PREFER_COREML` (ON by default on Apple).

---

## 3. Embedding Data Path

Current online embedding path is float32-first:

1. Tokenize text/query with WordPiece tokenizer.
2. Run ONNX Runtime session.
3. Extract embedding tensor and L2-normalize.
4. Insert float vectors into HNSW index.
5. Persist mapping metadata in `vector_map` with generation/model/provider fields.

Important runtime behavior:
- Online path no longer quantizes embeddings for primary generation.
- Legacy quantizer utilities remain for compatibility tooling only.

---

## 4. Vector Generation and Metadata

Vector metadata is generation-aware and stored in two places:

1. HNSW metadata file (`vectors-<generation>.meta`)
2. SQLite tables (`vector_map`, `vector_generation_state`)

Required metadata fields:
- `generation_id`
- `model_id`
- `dimensions`
- `provider`
- `migration_state`

This enables concurrent `v1`/`v2` artifacts and atomic active-generation cutover.

---

## 5. Dual-Index Migration Contract

Migration keeps search available throughout:

1. Active generation continues serving requests.
2. Target generation is built in background (`state=building`).
3. On health/completeness success, active pointer switches atomically.
4. Previous generation remains available for rollback.

Query service health and debug payloads expose migration state and active generation.

---

## 6. Retrieval Behavior (Semantic Contribution)

Semantic retrieval is not cosine-only ranking:

- Passage-level ANN hits are collected.
- Evidence is aggregated to document score via capped max/softmax pooling.
- Lexical + semantic candidate union feeds rerank stage.
- Cross-encoder rerank depth is latency-budget aware.

Debug contract includes:
- `activeVectorGeneration`
- `candidateCountsBySource`
- `coremlProviderUsed`
- `rerankDepthApplied`
- `semanticAggregationMode`

---

## 7. Memory and Latency Guardrails

Current runtime guardrails:

- Query path enforces rerank-depth adaptation when latency budget is consumed.
- Health payload reports per-service RSS and aggregate RSS.
- Indexer pipeline applies memory-aware prep-worker throttling and scan backpressure:
  - `BETTERSPOTLIGHT_INDEXER_RSS_SOFT_MB` (default `900`)
  - `BETTERSPOTLIGHT_INDEXER_RSS_HARD_MB` (default `1200`)
  - `BETTERSPOTLIGHT_INDEXER_PREP_WORKERS_PRESSURE` (default `1`)

Long-run gates remain required when runner capacity is available:
- 24h memory drift
- 48h stress

---

## 8. Operational Commands

Fetch model assets (now max-quality by default):

```bash
./tools/fetch_embedding_models.sh
```

Optional legacy-only fetch mode:

```bash
./tools/fetch_embedding_models.sh --no-max-quality
```

Build:

```bash
cmake -S . -B build
cmake --build build -j8
```

Run semantic shortlist harness:

```bash
./tools/benchmark_semantic_shortlist.sh /tmp/semantic-shortlist-report.json
```

---

## 9. Deprecated Historical Notes

Any prior references in older docs to fixed `384d int8` as the primary runtime should be treated as historical design context only. Current runtime authority is manifest-driven generation `v2` (`1024d float32`) with CoreML-first policy.
