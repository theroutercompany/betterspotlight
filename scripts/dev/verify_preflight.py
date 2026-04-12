#!/usr/bin/env python3
"""Capability and binary link preflight checks for scripts/dev/verify_pipeline.sh."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


PROFILES = ("core-hermetic", "extended-capabilities", "stress")
REQUIRED_CAPABILITIES = {
    "core-hermetic": {"semantic", "pdf"},
    "extended-capabilities": {"semantic", "pdf", "ocr", "online_ranker"},
    "stress": {"semantic", "pdf"},
}


def cache_truthy(cache_value: str) -> bool:
    normalized = cache_value.strip().upper()
    if not normalized:
        return False
    return normalized not in {"0", "FALSE", "OFF", "NO", "N", "NOTFOUND", "IGNORE", "UNKNOWN"}


def parse_cmake_cache(cache_path: Path) -> dict[str, str]:
    if not cache_path.exists():
        fail(f"CMake cache not found: {cache_path}")
    values: dict[str, str] = {}
    for raw in cache_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        if ":" not in line or "=" not in line:
            continue
        name_type, value = line.split("=", 1)
        name = name_type.split(":", 1)[0]
        values[name] = value
    return values


def run_build_contract(args: argparse.Namespace) -> None:
    build_dir = Path(args.build_dir).resolve()
    profile = args.profile
    cache = parse_cmake_cache(build_dir / "CMakeCache.txt")
    compile_commands = (build_dir / "compile_commands.json").read_text(
        encoding="utf-8", errors="ignore"
    ) if (build_dir / "compile_commands.json").exists() else ""

    failures: list[str] = []

    if "semantic" in REQUIRED_CAPABILITIES[profile]:
        with_onnx = cache.get("BETTERSPOTLIGHT_WITH_ONNX", "")
        onnx_include_dir = cache.get("ONNXRuntime_INCLUDE_DIR", "")
        onnx_library = cache.get("ONNXRuntime_LIBRARY", "")
        if not cache_truthy(with_onnx):
            failures.append(
                "BETTERSPOTLIGHT_WITH_ONNX is not enabled in configured build (semantic profile requires it)"
            )
        if not onnx_include_dir:
            failures.append("configured build is missing ONNXRuntime_INCLUDE_DIR")
        elif not (Path(onnx_include_dir) / "onnxruntime_cxx_api.h").exists():
            failures.append(
                "configured build points to a stale ONNX include dir "
                f"({onnx_include_dir}/onnxruntime_cxx_api.h missing)"
            )
        if not onnx_library:
            failures.append("configured build is missing ONNXRuntime_LIBRARY")
        elif not Path(onnx_library).exists():
            failures.append(
                f"configured build points to a stale ONNX library ({onnx_library})"
            )
        if "ONNXRUNTIME_FOUND" not in compile_commands:
            failures.append(
                "configured build did not compile ONNX-enabled targets (missing ONNXRUNTIME_FOUND in compile commands)"
            )

    if "pdf" in REQUIRED_CAPABILITIES[profile]:
        poppler_qt6 = cache.get("POPPLER_QT6_FOUND", "")
        poppler_cpp = cache.get("POPPLER_CPP_FOUND", "")
        if not (cache_truthy(poppler_qt6) or cache_truthy(poppler_cpp)):
            failures.append(
                "configured build has no Poppler backend (POPPLER_QT6_FOUND/POPPLER_CPP_FOUND not truthy)"
            )

    if "ocr" in REQUIRED_CAPABILITIES[profile]:
        tesseract_found = cache.get("TESSERACT_PC_FOUND", "")
        if not cache_truthy(tesseract_found):
            failures.append("configured build has no Tesseract backend (TESSERACT_PC_FOUND not truthy)")

    if failures and not args.allow_degraded:
        for item in failures:
            log(f"Error: {item}")
        raise SystemExit(1)

    if failures and args.allow_degraded:
        for item in failures:
            log(f"Warning: {item}")
        log("Continuing build-contract check because --allow-degraded is enabled.")

    log(f"Build contract passed for profile={profile}.")


def log(message: str) -> None:
    print(f"[verify-preflight] {message}")


def fail(message: str) -> None:
    log(f"Error: {message}")
    raise SystemExit(1)


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, capture_output=True, check=False)


def detect_poppler() -> tuple[bool, str]:
    pkg_config = shutil.which("pkg-config")
    if not pkg_config:
        return False, "pkg-config not found"
    for pkg_name in ("poppler-qt6", "poppler-cpp"):
        result = run([pkg_config, "--exists", pkg_name])
        if result.returncode == 0:
            return True, f"{pkg_name} via pkg-config"
    return False, "pkg-config missing poppler-qt6/poppler-cpp"


def detect_ocr() -> tuple[bool, str]:
    tesseract = shutil.which("tesseract")
    if not tesseract:
        return False, "tesseract command not found"
    result = run([tesseract, "--version"])
    if result.returncode != 0:
        return False, "tesseract --version failed"
    first_line = result.stdout.splitlines()[0] if result.stdout else "tesseract present"
    return True, first_line


def detect_semantic(root_dir: Path) -> tuple[bool, str]:
    models_dir = Path(
        os.environ.get("BETTERSPOTLIGHT_MODELS_DIR", str(root_dir / "data" / "models"))
    )
    onnx_include = Path(os.environ.get("ONNXRuntime_INCLUDE_DIR", ""))
    onnx_library = Path(os.environ.get("ONNXRuntime_LIBRARY", ""))

    missing: list[str] = []
    required_paths = [
        models_dir / "manifest.json",
        models_dir / "vocab.txt",
        models_dir / "bge-small-en-v1.5-int8.onnx",
        models_dir / "bge-large-en-v1.5-f32.onnx",
        onnx_include / "onnxruntime_cxx_api.h",
        onnx_library,
    ]
    for path in required_paths:
        if not path.exists():
            missing.append(str(path))

    if missing:
        return False, f"missing assets: {', '.join(missing)}"
    return True, f"models={models_dir} onnx={onnx_library}"


def detect_online_ranker(root_dir: Path) -> tuple[bool, str]:
    bootstrap_dir = Path(
        os.environ.get(
            "BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR",
            str(root_dir / "data" / "models" / "online-ranker-v1" / "bootstrap"),
        )
    )
    ready_env = os.environ.get("BS_DEV_ONLINE_RANKER_BOOTSTRAP_READY", "0") == "1"
    model_dir = bootstrap_dir / "online_ranker_v1.mlmodelc"
    metadata = bootstrap_dir / "metadata.json"
    ready = ready_env and model_dir.is_dir() and metadata.is_file()
    detail = (
        f"ready via {bootstrap_dir}"
        if ready
        else f"missing bootstrap artifacts under {bootstrap_dir}"
    )
    return ready, detail


def collect_mode_issues() -> list[str]:
    issues: list[str] = []
    allow_unsupported = os.environ.get("BETTERSPOTLIGHT_ALLOW_UNSUPPORTED_RUNTIME_MODES") == "1"
    if allow_unsupported:
        issues.append("BETTERSPOTLIGHT_ALLOW_UNSUPPORTED_RUNTIME_MODES=1")

    expected_values = {
        "BETTERSPOTLIGHT_PIPELINE_ACTOR_MODE": "actor_primary",
        "BETTERSPOTLIGHT_CONTROL_PLANE_MODE": "actor_primary",
        "BETTERSPOTLIGHT_HEALTH_SOURCE_MODE": "aggregator_primary",
        "BETTERSPOTLIGHT_INFERENCE_SUPERVISOR_MODE": "actor_primary",
    }
    for env_name, expected in expected_values.items():
        value = os.environ.get(env_name)
        if value and value != expected:
            issues.append(f"{env_name}={value} (expected {expected})")
    return issues


def run_capabilities(args: argparse.Namespace) -> None:
    root_dir = Path(args.root_dir).resolve()
    capabilities: dict[str, tuple[bool, str]] = {
        "semantic": detect_semantic(root_dir),
        "pdf": detect_poppler(),
        "ocr": detect_ocr(),
        "online_ranker": detect_online_ranker(root_dir),
    }

    log(f"profile={args.profile}")
    log("Capability matrix:")
    for name in ("semantic", "pdf", "ocr", "online_ranker"):
        ready, detail = capabilities[name]
        status = "ready" if ready else "missing"
        log(f"  {name:14s} {status:7s} {detail}")

    failures: list[str] = []
    for capability in sorted(REQUIRED_CAPABILITIES[args.profile]):
        if not capabilities[capability][0]:
            failures.append(f"required capability missing: {capability} ({capabilities[capability][1]})")

    mode_issues = collect_mode_issues()
    for issue in mode_issues:
        failures.append(f"incompatible runtime mode/capability profile: {issue}")

    if failures and not args.allow_degraded:
        for item in failures:
            log(f"Error: {item}")
        raise SystemExit(1)

    if failures and args.allow_degraded:
        for item in failures:
            log(f"Warning: {item}")
        log("Continuing because --allow-degraded is enabled.")


def parse_ctest_json(build_dir: Path) -> dict:
    result = run(["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"])
    if result.returncode != 0:
        fail(f"ctest --show-only failed for {build_dir}: {result.stderr.strip()}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        fail(f"failed to parse ctest json output: {exc}")


def gather_binaries(build_dir: Path) -> list[Path]:
    report = parse_ctest_json(build_dir)
    binaries: set[Path] = set()
    for test in report.get("tests", []):
        command = test.get("command") or []
        if not command:
            continue
        executable = Path(command[0])
        if executable.is_absolute():
            binaries.add(executable)

    app_bundle = build_dir / "src" / "app" / "betterspotlight.app"
    app_binary = app_bundle / "Contents" / "MacOS" / "betterspotlight"
    helpers_dir = app_bundle / "Contents" / "Helpers"
    binaries.add(app_binary)
    for helper_name in ("indexer", "extractor", "query", "inference"):
        binaries.add(helpers_dir / f"betterspotlight-{helper_name}")
    return sorted(binaries)


def binary_is_macho(binary: Path) -> bool:
    file_cmd = shutil.which("file")
    if not file_cmd:
        return False
    result = run([file_cmd, "-b", str(binary)])
    return result.returncode == 0 and "Mach-O" in result.stdout


def missing_deps_for_binary(binary: Path) -> list[str]:
    if not binary_is_macho(binary):
        return []
    otool = shutil.which("otool")
    if not otool:
        fail("otool is required for link-health checks on macOS")
    result = run([otool, "-L", str(binary)])
    if result.returncode != 0:
        return [f"unable to inspect dependencies: {result.stderr.strip()}"]

    missing: list[str] = []
    for line in result.stdout.splitlines()[1:]:
        dep = line.strip().split(" ", 1)[0]
        if not dep.startswith("/"):
            continue
        if dep.startswith("/System/Library/") or dep.startswith("/usr/lib/"):
            continue
        if not Path(dep).exists():
            missing.append(dep)
    return missing


def run_link_health(args: argparse.Namespace) -> None:
    build_dir = Path(args.build_dir).resolve()
    binaries = gather_binaries(build_dir)
    failures: list[str] = []

    env_onnx = os.environ.get("ONNXRuntime_LIBRARY")
    if env_onnx and not Path(env_onnx).exists():
        failures.append(f"ONNXRuntime_LIBRARY points to missing file: {env_onnx}")

    checked = 0
    for binary in binaries:
        if not binary.exists():
            failures.append(f"missing binary: {binary}")
            continue
        checked += 1
        missing = missing_deps_for_binary(binary)
        for dep in missing:
            failures.append(f"{binary} -> missing dependency: {dep}")

    log(f"Link-health scanned {checked} binaries.")
    if failures:
        for item in failures:
            log(f"Error: {item}")
        raise SystemExit(1)


def run_docs_parity(args: argparse.Namespace) -> None:
    root_dir = Path(args.root_dir).resolve()
    manual_doc = root_dir / "docs" / "operations" / "manual-inspection-m1-m2.md"
    if not manual_doc.exists():
        fail(f"manual ops doc not found: {manual_doc}")

    text = manual_doc.read_text(encoding="utf-8")
    required_snippets = [
        "./scripts/dev/setup_env.sh --check-only",
        "./scripts/dev/verify_pipeline.sh --build-dir build-stabilize --skip-launch",
        "build-stabilize/src/app/betterspotlight.app/Contents/MacOS/betterspotlight",
    ]
    disallowed_snippets = [
        "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release",
        "ctest --test-dir build --output-on-failure",
    ]

    failures: list[str] = []
    for snippet in required_snippets:
        if snippet not in text:
            failures.append(f"missing required docs snippet: {snippet}")
    for snippet in disallowed_snippets:
        if snippet in text:
            failures.append(f"legacy docs snippet should be removed: {snippet}")

    if failures:
        for item in failures:
            log(f"Error: {item}")
        raise SystemExit(1)

    log(f"Docs parity passed for {manual_doc}.")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    p_caps = subparsers.add_parser("capabilities", help="check capability matrix and profile gates")
    p_caps.add_argument("--root-dir", required=True)
    p_caps.add_argument("--profile", choices=PROFILES, default="core-hermetic")
    p_caps.add_argument("--allow-degraded", action="store_true")
    p_caps.set_defaults(func=run_capabilities)

    p_links = subparsers.add_parser("link-health", help="check built binaries for missing dylib deps")
    p_links.add_argument("--build-dir", required=True)
    p_links.set_defaults(func=run_link_health)

    p_contract = subparsers.add_parser(
        "build-contract",
        help="verify configured build capabilities match verification profile",
    )
    p_contract.add_argument("--build-dir", required=True)
    p_contract.add_argument("--profile", choices=PROFILES, default="core-hermetic")
    p_contract.add_argument("--allow-degraded", action="store_true")
    p_contract.set_defaults(func=run_build_contract)

    p_docs = subparsers.add_parser("docs-parity", help="verify docs reference supported script flow")
    p_docs.add_argument("--root-dir", required=True)
    p_docs.set_defaults(func=run_docs_parity)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
