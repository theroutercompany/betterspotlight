# Semantic Stack Upgrade Audit (2026-02-11)

## Scope

This change set implements the principal engineering semantic-stack upgrade baseline:

- Runtime-configurable vector dimensions (no compile-time 384 lock)
- CoreML-first model session policy with explicit runtime disable override
- Float32-first online embedding path (quantizer removed from production path)
- Generation-aware vector metadata and dual-generation migration plumbing
- Passage-aware semantic aggregation in lexical/semantic merge
- Expanded query debug and health contracts for migration and memory visibility

## Delivered

1. Model/runtime authority

- `src/core/models/model_manifest.h/.cpp`
  - Added `modelId`, `generationId`, `fallbackRole`, provider policy fields.
- `src/core/models/model_session.h/.cpp`
  - CoreML preference policy + runtime env override `BETTERSPOTLIGHT_DISABLE_COREML`.
  - Provider diagnostics (`selectedProvider`, `coreMlRequested`, `coreMlAttached`).
- `src/core/models/model_registry.cpp`
  - Fallback role resolution when preferred model is unavailable.
- `data/models/manifest.json`
  - Primary `bi-encoder` now `v2` 1024d target with fallback to `bi-encoder-legacy` (`v1`, 384d).

2. Embedding pipeline

- `src/core/embedding/embedding_pipeline.h/.cpp`
  - Removed quantizer dependency from the online persistence path.
  - Stores float32 embeddings with generation/model/provider metadata.
- `src/core/embedding/embedding_manager.h/.cpp`
  - Exposes active dimensions/model/generation/provider and semantic aggregation mode.

3. Vector index/storage

- `src/core/vector/vector_index.h/.cpp`
  - Dynamic dimensions via runtime metadata.
  - Persisted index metadata now includes generation/model/provider.
- `src/core/vector/vector_store.h/.cpp`
  - New `vector_map` contract:
    - `generation_id`, `model_id`, `dimensions`, `provider`, `passage_ordinal`, `migration_state`
  - Added `vector_generation_state` table support.
  - Added active generation APIs and legacy schema migration path.
- `src/core/index/schema.h`
  - Canonical schema updated for generation-aware vector storage.
- `src/core/index/migration.cpp`
  - Added migration path `schema 2 -> 3` for generation-state settings/table.

4. Query service and migration orchestration

- `src/services/query/query_service.h/.cpp`
  - Active generation-aware index path resolution.
  - Generation-aware semantic label resolution.
  - Expanded debug contract:
    - `activeVectorGeneration`
    - `candidateCountsBySource`
    - `coremlProviderUsed`
    - `rerankDepthApplied`
    - `semanticAggregationMode`
  - Expanded health contract:
    - `memory.aggregateRssMb`
    - `memory.byService`
    - `vectorMigration.*`
    - `vectorGeneration.*`
  - Automatic startup migration trigger when active generation differs from target generation.
- `src/services/query/query_service_m2.cpp`
  - Rebuild now targets a named generation (`targetGeneration`) and performs atomic cutover.
  - Generation-specific mapping persistence with passage ordinals.
  - Migration state/progress surfaces updated during rebuild.

5. Retrieval quality improvements

- `src/core/vector/search_merger.h/.cpp`
  - Added passage-support-aware semantic aggregation (`aggregateSemanticScore`).
  - Multi-hit semantic evidence for a document now contributes via capped softmax support bonus.
- `src/services/query/query_service.cpp`
  - Merge config now tunes passage cap and semantic softmax behavior by query class.
  - Rerank depth now adapts against an interactive latency budget target.

6. Build/runtime defaults and guardrails

- `tools/fetch_embedding_models.sh`
  - Max-quality (`1024d`) model fetch is now default.
  - Added explicit `--no-max-quality` override for constrained/dev flows.
- `CMakeLists.txt`
  - Added `BETTERSPOTLIGHT_FETCH_MAX_QUALITY_MODEL` option (default ON).
  - `betterspotlight-fetch-models` target now tracks max-quality byproducts conditionally.
- `src/core/indexing/pipeline.h/.cpp`
  - Added memory-aware prep-worker throttling and scan backpressure controls:
    - `BETTERSPOTLIGHT_INDEXER_RSS_SOFT_MB`
    - `BETTERSPOTLIGHT_INDEXER_RSS_HARD_MB`
    - `BETTERSPOTLIGHT_INDEXER_PREP_WORKERS_PRESSURE`
- `src/services/indexer/indexer_service.cpp`
  - Queue-status payload now includes `memory` telemetry (`rssMb`, pressure band, soft/hard limits).

7. Documentation conformance updates

- Updated stale assumptions in:
  - `docs/milestones/m2/embedding-pipeline.md`
  - `docs/foundation/indexing-pipeline.md`
  - `docs/milestones/M3_ARCHITECTURAL_MAP.md`
  - `docs/operations/manual-inspection-m1-m2.md`

## Validation

- Compiled successfully:
  - `betterspotlight-core-embedding`
  - `betterspotlight-core-models`
  - `betterspotlight-core-vector`
- Syntax-checked updated sources against compile database:
  - `src/core/indexing/pipeline.cpp`
  - `src/services/indexer/indexer_service.cpp`
  - `src/services/query/query_service.cpp`
- Semantic shortlist harness report generated with max-quality artifact present:
  - `docs/audit/semantic-shortlist-benchmark-report.json`
  - `highQualityModelPresent: 1`, primary dims `1024`, generation `v2`
- Full build is currently blocked in this environment by missing Poppler headers in extraction (`pdf_extractor.cpp`).

## Follow-up Gap Closure (2026-02-11, continuation)

Additional deltas applied to close remaining plan gaps:

- `tools/fetch_embedding_models.sh` now defaults to max-quality model fetch (`1024d` primary), with explicit `--no-max-quality` opt-out.
- `CMakeLists.txt` adds `BETTERSPOTLIGHT_FETCH_MAX_QUALITY_MODEL` (default `ON`) and wires model fetch args/byproducts accordingly.
- `src/core/indexing/pipeline.cpp/.h` adds memory-aware prep throttling and scan backpressure guardrails:
  - soft/hard RSS thresholds with env overrides.
  - pressure-mode prep worker cap.
- `src/core/embedding/embedding_pipeline.cpp/.h` adds adaptive embedding batch size based on RSS pressure.
- `src/services/indexer/indexer_service.cpp` now exposes indexer memory telemetry in `getQueueStatus`.
- `src/services/query/query_service.cpp/.h` now auto-starts background vector migration on startup when active and target generations differ (configurable via `autoVectorMigration` setting).
- Required docs were aligned to current runtime posture (`1024d float32` primary, CoreML-first default, dual-index migration):
  - `/Users/rexliu/betterspotlight/docs/foundation/indexing-pipeline.md`
  - `/Users/rexliu/betterspotlight/docs/milestones/m2/embedding-pipeline.md`
  - `/Users/rexliu/betterspotlight/docs/milestones/M3_ARCHITECTURAL_MAP.md`
  - `/Users/rexliu/betterspotlight/docs/operations/manual-inspection-m1-m2.md`

## Deferred / Requires Dedicated Runner

- 24h memory drift gate
- 48h stress gate

Both remain required release gates and are pending runner availability.
