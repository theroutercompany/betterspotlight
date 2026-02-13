#!/usr/bin/env python3
"""Generate an updatable Core ML bootstrap model for BetterSpotlight online ranker."""

from __future__ import annotations

import argparse
import datetime as dt
import inspect
import json
import pathlib
import shutil
import subprocess
import sys
from typing import Sequence

import numpy as np


def _require_coremltools():
    try:
        import coremltools as ct  # type: ignore
        from coremltools.models import datatypes  # type: ignore
        from coremltools.models.neural_network import (  # type: ignore
            NeuralNetworkBuilder,
            SgdParams,
        )
    except Exception as exc:  # pragma: no cover - import guard
        raise RuntimeError(
            "coremltools is required. Install with: "
            "python3 -m pip install coremltools"
        ) from exc
    return ct, datatypes, NeuralNetworkBuilder, SgdParams


def _call_with_supported_kwargs(fn, *args, **kwargs):
    sig = inspect.signature(fn)
    filtered = {k: v for k, v in kwargs.items() if k in sig.parameters}
    return fn(*args, **filtered)


def _run(cmd: Sequence[str]) -> None:
    subprocess.run(cmd, check=True)


def _compile_with_xcrun(mlmodel_path: pathlib.Path, compiled_path: pathlib.Path) -> None:
    compile_root = compiled_path.parent / ".coreml_compile_tmp"
    if compile_root.exists():
        shutil.rmtree(compile_root)
    compile_root.mkdir(parents=True, exist_ok=True)

    _run(
        [
            "xcrun",
            "coremlcompiler",
            "compile",
            str(mlmodel_path),
            str(compile_root),
        ]
    )

    generated = compile_root / f"{mlmodel_path.stem}.mlmodelc"
    if not generated.exists():
        compiled_candidates = list(compile_root.glob("*.mlmodelc"))
        if not compiled_candidates:
            raise RuntimeError("coremlcompiler succeeded but no .mlmodelc directory was produced")
        generated = compiled_candidates[0]

    if compiled_path.exists():
        shutil.rmtree(compiled_path)
    shutil.move(str(generated), str(compiled_path))
    shutil.rmtree(compile_root, ignore_errors=True)


def _extract_label_feature_name(spec, input_feature_name: str, fallback: str) -> str:
    for training_input in spec.description.trainingInput:
        if training_input.name != input_feature_name:
            return training_input.name
    return fallback


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Generate BetterSpotlight online ranker bootstrap model "
            "(updatable Core ML neuralnetwork classifier)."
        )
    )
    parser.add_argument(
        "--output-root",
        default="data/models/online-ranker-v1/bootstrap",
        help="Bootstrap output directory (contains .mlmodelc + metadata.json)",
    )
    parser.add_argument("--feature-dim", type=int, default=13, help="Input dense feature dimension")
    parser.add_argument("--input-name", default="features", help="Input feature name")
    parser.add_argument(
        "--probability-output-name",
        default="probabilities",
        help="Softmax probability output blob name",
    )
    parser.add_argument(
        "--predicted-label-name",
        default="classLabel",
        help="Predicted class output feature name",
    )
    parser.add_argument(
        "--learning-rate",
        type=float,
        default=0.01,
        help="Default on-device SGD learning rate embedded in update params",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=32,
        help="Default on-device SGD mini-batch size embedded in update params",
    )
    parser.add_argument(
        "--epochs",
        type=int,
        default=1,
        help="Default on-device SGD epochs embedded in update params",
    )
    parser.add_argument(
        "--version",
        default="",
        help="Optional metadata version string (default: bootstrap_utc_timestamp)",
    )
    parser.add_argument(
        "--keep-mlmodel",
        action="store_true",
        help="Keep intermediate .mlmodel next to compiled model",
    )
    args = parser.parse_args()

    if args.feature_dim <= 0:
        raise ValueError("--feature-dim must be > 0")
    if args.learning_rate <= 0.0:
        raise ValueError("--learning-rate must be > 0")
    if args.batch_size <= 0:
        raise ValueError("--batch-size must be > 0")
    if args.epochs <= 0:
        raise ValueError("--epochs must be > 0")

    ct, datatypes, NeuralNetworkBuilder, SgdParams = _require_coremltools()

    output_root = pathlib.Path(args.output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    mlmodel_path = output_root / "online_ranker_v1.mlmodel"
    compiled_path = output_root / "online_ranker_v1.mlmodelc"
    metadata_path = output_root / "metadata.json"

    input_features = [(args.input_name, datatypes.Array(args.feature_dim))]
    output_features = [(args.probability_output_name, datatypes.Array(2))]
    builder = NeuralNetworkBuilder(
        input_features=input_features,
        output_features=output_features,
        mode="classifier",
        disable_rank5_shape_mapping=True,
    )

    dense_layer_name = "online_head_dense"
    logits_blob = "logits"
    dense_weights = np.zeros((2, args.feature_dim), dtype=np.float32)
    dense_bias = np.zeros((2,), dtype=np.float32)
    builder.add_inner_product(
        name=dense_layer_name,
        W=dense_weights,
        b=dense_bias,
        input_channels=args.feature_dim,
        output_channels=2,
        has_bias=True,
        input_name=args.input_name,
        output_name=logits_blob,
    )
    builder.add_softmax(
        name="online_head_softmax",
        input_name=logits_blob,
        output_name=args.probability_output_name,
    )

    class_labels = ["0", "1"]
    _call_with_supported_kwargs(
        builder.set_class_labels,
        class_labels,
        predicted_feature_name=args.predicted_label_name,
        prediction_blob=args.probability_output_name,
    )

    try:
        builder.make_updatable([dense_layer_name])
    except TypeError:
        builder.make_updatable()

    try:
        builder.set_categorical_cross_entropy_loss(
            name="cross_entropy_loss", input=args.probability_output_name
        )
    except TypeError:
        builder.set_categorical_cross_entropy_loss("cross_entropy_loss", args.probability_output_name)

    try:
        sgd_params = SgdParams(lr=args.learning_rate, batch=args.batch_size)
    except TypeError:
        sgd_params = SgdParams(learning_rate=args.learning_rate, batch=args.batch_size)
    builder.set_sgd_optimizer(sgd_params)
    builder.set_epochs(args.epochs)

    spec = builder.spec
    spec.description.input[0].shortDescription = "Dense ranking/context features"
    spec.description.output[0].shortDescription = "Binary relevance probability distribution"
    spec.description.metadata.shortDescription = (
        "BetterSpotlight online ranker v1 bootstrap (updatable neuralnetwork classifier)"
    )
    spec.description.metadata.author = "BetterSpotlight"
    spec.description.metadata.versionString = "1.0"

    model = ct.models.MLModel(spec, skip_model_load=True)
    model.save(str(mlmodel_path))
    _compile_with_xcrun(mlmodel_path, compiled_path)

    label_feature_name = _extract_label_feature_name(
        spec, args.input_name, f"{args.probability_output_name}_true"
    )
    version = args.version.strip() or dt.datetime.now(dt.timezone.utc).strftime(
        "bootstrap_%Y%m%d%H%M%S"
    )
    metadata = {
        "version": version,
        "backend": "coreml",
        "featureDim": args.feature_dim,
        "inputFeatureName": args.input_name,
        "labelFeatureName": label_feature_name,
        "probabilityOutputName": args.probability_output_name,
        "predictedLabelName": args.predicted_label_name,
        "classLabels": class_labels,
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if not args.keep_mlmodel:
        mlmodel_path.unlink(missing_ok=True)

    print(f"Wrote compiled model: {compiled_path}")
    print(f"Wrote metadata: {metadata_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # pragma: no cover - CLI guard
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)

