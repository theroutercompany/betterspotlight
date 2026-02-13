# Continual Learning Soak Gate (48h)

This gate validates the v1 continual-learning loop in a service-driven run:

- behavior event ingest (`record_behavior_event`)
- positive attribution onto indexed items
- manual learning cycle triggers (`trigger_learning_cycle`)
- periodic learning health snapshots (`get_learning_health`)
- periodic DB integrity checks

## Harness

- Script: `/Users/rexliu/betterspotlight/Tests/benchmarks/learning_soak_48h.sh`
- Default duration: `48h` (`172800s`)

## Quick smoke

```bash
BS_STRESS_DURATION_SECONDS=300 \
BS_STRESS_SAMPLE_INTERVAL=2 \
BS_LEARNING_CYCLE_INTERVAL=10 \
/Users/rexliu/betterspotlight/Tests/benchmarks/learning_soak_48h.sh
```

## Full 48h run

```bash
/Users/rexliu/betterspotlight/Tests/benchmarks/learning_soak_48h.sh
```

## Key env knobs

- `BS_LEARNING_ROLLOUT_MODE`: `instrumentation_only|shadow_training|blended_ranking` (default `shadow_training`)
- `BS_LEARNING_PAUSE_ON_USER_INPUT`: `0|1` (default `0` for deterministic harness cycles)
- `BS_LEARNING_CYCLE_INTERVAL`: seconds between manual trigger attempts (default `60`)
- `BS_LEARNING_MIN_CYCLES`: minimum required cycle delta (default `1`)
- `BS_LEARNING_ASSERT_PROGRESS`: fail if cycle delta is below min (default `1`)
- `BS_STRESS_ARTIFACT_DIR`: explicit artifact output directory
- `BS_STRESS_QUERY_BIN|BS_STRESS_INDEXER_BIN|BS_STRESS_EXTRACTOR_BIN`: binary overrides

## Artifacts

The harness writes:

- `summary.json`: top-level pass/fail and aggregate counters
- `metrics.json`: periodic general service health samples
- `learning_health_samples.json`: periodic snapshots from `get_learning_health`
- `learning_cycle_events.json`: per-manual-trigger outcomes
- `initial_learning_health.json` and `final_learning_health.json`
- `query.log`, `indexer.log`, `extractor.log`

## Pass/fail criteria

The harness exits non-zero on:

- any collected RPC/processing errors
- any SQLite integrity check failures
- (default) no measurable learning cycle progress vs `BS_LEARNING_MIN_CYCLES`

