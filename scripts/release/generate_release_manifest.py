#!/usr/bin/env python3
"""Generate dist/release/release-manifest.json for BetterSpotlight releases."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import subprocess
import sys
from typing import Any


def run_git(args: list[str]) -> str:
    try:
        return (
            subprocess.check_output(["git", *args], text=True, stderr=subprocess.DEVNULL)
            .strip()
        )
    except Exception:
        return ""


def parse_bool(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "yes", "on"}


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def infer_artifact_class(path: pathlib.Path) -> str:
    suffix = path.suffix.lower()
    if suffix == ".zip":
        return "zip"
    if suffix == ".dmg":
        return "dmg"
    if suffix == ".json":
        return "metadata"
    if suffix == ".log":
        return "log"
    return "artifact"


def infer_source(repo_override: str, commit_override: str, ref_override: str) -> dict[str, Any]:
    remote_url = run_git(["config", "--get", "remote.origin.url"])
    clean = run_git(["status", "--porcelain"]) == ""
    return {
        "repository": repo_override or os.getenv("GITHUB_REPOSITORY", ""),
        "remoteUrl": remote_url,
        "commit": commit_override or os.getenv("GITHUB_SHA", run_git(["rev-parse", "HEAD"])),
        "ref": ref_override
        or os.getenv("GITHUB_REF", run_git(["symbolic-ref", "--short", "HEAD"])),
        "workflowRef": os.getenv("GITHUB_WORKFLOW_REF", ""),
        "treeState": "clean" if clean else "dirty",
    }


def infer_builder(provider_override: str, runner_class_override: str, runner_label_override: str) -> dict[str, Any]:
    provider = provider_override
    if not provider:
        provider = "github-actions" if os.getenv("GITHUB_ACTIONS") == "true" else "local"
    return {
        "provider": provider,
        "runnerClass": runner_class_override or os.getenv("RUNNER_OS", ""),
        "runnerLabel": runner_label_override or os.getenv("RUNNER_NAME", ""),
        "workflow": os.getenv("GITHUB_WORKFLOW", ""),
        "workflowRunId": os.getenv("GITHUB_RUN_ID", ""),
        "workflowRunAttempt": os.getenv("GITHUB_RUN_ATTEMPT", ""),
        "job": os.getenv("GITHUB_JOB", ""),
    }


def build_artifacts(paths: list[str], root_dir: pathlib.Path) -> list[dict[str, Any]]:
    artifacts: list[dict[str, Any]] = []
    for raw in paths:
        path = pathlib.Path(raw).resolve()
        if not path.exists() or not path.is_file():
            continue
        rel_path = os.path.relpath(path, root_dir)
        artifacts.append(
            {
                "name": path.name,
                "path": rel_path,
                "class": infer_artifact_class(path),
                "sha256": sha256_file(path),
                "size_bytes": path.stat().st_size,
                "attestation": {
                    "type": "github-artifact-attestation",
                    "verifyCommand": "gh attestation verify <artifact> --repo <owner/repo>",
                },
            }
        )
    return artifacts


def load_summary(path: str) -> dict[str, Any]:
    if not path:
        return {}
    summary_path = pathlib.Path(path)
    if not summary_path.exists():
        return {}
    try:
        return json.loads(summary_path.read_text(encoding="utf-8"))
    except Exception:
        return {}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--project", default="betterspotlight")
    parser.add_argument("--source-repository", default="")
    parser.add_argument("--source-commit", default="")
    parser.add_argument("--source-ref", default="")
    parser.add_argument("--builder-provider", default="")
    parser.add_argument("--builder-runner-class", default="")
    parser.add_argument("--builder-runner-label", default="")
    parser.add_argument("--apple-signed", default="false")
    parser.add_argument("--apple-notarized", default="false")
    parser.add_argument("--apple-stapled", default="false")
    parser.add_argument("--summary-json", default="")
    parser.add_argument("--artifact", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root_dir = pathlib.Path(__file__).resolve().parents[2]

    manifest = {
        "schemaVersion": "1.0",
        "project": args.project,
        "source": infer_source(args.source_repository, args.source_commit, args.source_ref),
        "builder": infer_builder(
            args.builder_provider,
            args.builder_runner_class,
            args.builder_runner_label,
        ),
        "artifacts": build_artifacts(args.artifact, root_dir),
        "appleTrust": {
            "signed": parse_bool(args.apple_signed),
            "notarized": parse_bool(args.apple_notarized),
            "stapled": parse_bool(args.apple_stapled),
        },
        "generatedAt": dt.datetime.now(tz=dt.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z"),
    }

    summary = load_summary(args.summary_json)
    if summary:
        manifest["releaseSummary"] = summary

    output_path = pathlib.Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
