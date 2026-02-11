# M3 Semantic Stack Upgrade (1024d + CoreML-First + Dual-Generation)

## Runtime Policy

- Primary semantic generation: `v2`
- Primary dimensionality: `1024`
- Provider policy: CoreML preferred on Apple Silicon, CPU fallback enabled
- Runtime CoreML disable override: `BETTERSPOTLIGHT_DISABLE_COREML=1`

## Generation Model

- Legacy generation: `v1` (384d fallback)
- Target generation: `v2` (1024d primary)
- Query service resolves vectors against active generation metadata.
- Rebuild worker builds target generation in background and performs atomic active-generation cutover.

## Health and Debug Contracts

### Index Health Additions

- `memory.aggregateRssMb`
- `memory.byService`
- `vectorMigration.state`
- `vectorMigration.progressPct`
- `vectorGeneration.active`

### Search Debug Additions

- `activeVectorGeneration`
- `candidateCountsBySource`
- `coremlProviderUsed`
- `rerankDepthApplied`
- `semanticAggregationMode`

## Retrieval Behavior

- Stage A candidate mix: lexical + passage-level ANN signals
- Stage B rerank: cross-encoder depth adapts to latency budget
- Semantic score aggregation supports multi-passage evidence per document

## Rollout Notes

1. Keep `v1` readable during `v2` build.
2. Cut over active generation only after target generation persistence succeeds.
3. On migration failure, retain existing active generation.
4. Keep long-run reliability gates required (24h drift, 48h stress) when runner capacity exists.
