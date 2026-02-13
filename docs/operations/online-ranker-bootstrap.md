# Online Ranker CoreML Bootstrap

This project supports a CoreML-first online ranker for continual personalization.
The runtime looks for a per-user bootstrap artifact at:

- `~/Library/Application Support/betterspotlight/models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc`

If missing, it attempts to seed from bundled/source bootstrap artifacts.

## Generate Bootstrap Artifacts

### Prerequisites

- macOS with Xcode command line tools (`xcrun`)
- Python 3
- `coremltools`:

```bash
python3 -m pip install coremltools
```

### Generate

```bash
./tools/generate_online_ranker_coreml.py \
  --output-root data/models/online-ranker-v1/bootstrap \
  --feature-dim 13 \
  --input-name features
```

This writes:

- `data/models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc`
- `data/models/online-ranker-v1/bootstrap/metadata.json`

## Build Integration

- CMake target: `betterspotlight-generate-online-ranker-bootstrap`
- Optional auto-generation flag:
  `-DBETTERSPOTLIGHT_GENERATE_ONLINE_RANKER_BOOTSTRAP=ON`

Bundle sync (`cmake/sync_embedding_models.cmake`) copies
`online-ranker-v1/bootstrap` into app resources when present.

## Release Validation

Release pipeline supports optional strict validation:

- `BS_REQUIRE_ONLINE_RANKER_BOOTSTRAP=1` enforces presence of
  bundled `online_ranker_v1.mlmodelc` and `metadata.json`.

When not required, missing bootstrap artifacts only emit a warning and
runtime falls back to native SGD ranker behavior.
