#!/usr/bin/env python3
"""Capability and binary link preflight checks for scripts/dev/verify_pipeline.sh."""

from __future__ import annotations

import argparse
import hashlib
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
HELPER_NAMES = ("indexer", "extractor", "query", "inference")


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


def is_path_lexically_rooted(path_value: str, root_prefix: Path) -> bool:
    if not path_value:
        return False
    path = Path(path_value)
    if not path.is_absolute():
        path = (Path.cwd() / path).resolve(strict=False)
    try:
        path.relative_to(root_prefix)
        return True
    except ValueError:
        return False


def path_env_entries(value: str) -> list[Path]:
    entries: list[Path] = []
    for raw in value.split(os.pathsep):
        item = raw.strip()
        if not item:
            continue
        path = Path(item)
        if not path.is_absolute():
            path = (Path.cwd() / path).resolve(strict=False)
        entries.append(path)
    return entries


def rooted_native_backend_issues(
    root_dir: Path,
    *,
    require_semantic: bool,
    require_pdf: bool,
) -> list[str]:
    issues: list[str] = []
    rooted_prefix = root_dir / ".build" / "nix-gcroots"

    if require_semantic:
        onnx_include = os.environ.get("ONNXRuntime_INCLUDE_DIR", "")
        onnx_library = os.environ.get("ONNXRuntime_LIBRARY", "")
        if not onnx_include:
            issues.append("ONNXRuntime_INCLUDE_DIR is not set in environment")
        else:
            header = Path(onnx_include) / "onnxruntime_cxx_api.h"
            if not header.exists():
                issues.append(f"ONNX header missing at {header}")
            if not is_path_lexically_rooted(onnx_include, rooted_prefix):
                issues.append(
                    f"ONNX include path is not rooted under {rooted_prefix}: {onnx_include}"
                )
        if not onnx_library:
            issues.append("ONNXRuntime_LIBRARY is not set in environment")
        else:
            if not Path(onnx_library).exists():
                issues.append(f"ONNX runtime library missing at {onnx_library}")
            if not is_path_lexically_rooted(onnx_library, rooted_prefix):
                issues.append(
                    f"ONNX library path is not rooted under {rooted_prefix}: {onnx_library}"
                )

    if require_pdf:
        capability = os.environ.get("BS_DEV_PDF_CAPABILITY", "")
        if capability and capability != "ready":
            issues.append(f"BS_DEV_PDF_CAPABILITY={capability} (expected ready)")
        backend = os.environ.get("BS_DEV_POPPLER_BACKEND", "")
        if backend and backend in {"none", "degraded"}:
            issues.append(f"BS_DEV_POPPLER_BACKEND={backend} (expected poppler-qt6 or poppler-cpp)")
        poppler_prefix = os.environ.get("BS_DEV_POPPLER_PREFIX", "")
        if not poppler_prefix:
            issues.append("BS_DEV_POPPLER_PREFIX is not set")
        else:
            if not Path(poppler_prefix).exists():
                issues.append(f"BS_DEV_POPPLER_PREFIX is stale/missing: {poppler_prefix}")
            if not is_path_lexically_rooted(poppler_prefix, rooted_prefix):
                issues.append(
                    f"BS_DEV_POPPLER_PREFIX is not rooted under {rooted_prefix}: {poppler_prefix}"
                )
        pkg_config_bin = os.environ.get("PKG_CONFIG_BIN", "")
        if not pkg_config_bin:
            issues.append("PKG_CONFIG_BIN is not set")
        else:
            if not Path(pkg_config_bin).exists():
                issues.append(f"PKG_CONFIG_BIN is stale/missing: {pkg_config_bin}")
            if not is_path_lexically_rooted(pkg_config_bin, rooted_prefix):
                issues.append(
                    f"PKG_CONFIG_BIN is not rooted under {rooted_prefix}: {pkg_config_bin}"
                )

        pkg_config_path = os.environ.get("PKG_CONFIG_PATH", "")
        if not pkg_config_path:
            issues.append("PKG_CONFIG_PATH is empty; rooted Poppler contract cannot be verified")
        else:
            entries = path_env_entries(pkg_config_path)
            rooted_entries = [path for path in entries if is_path_lexically_rooted(str(path), rooted_prefix)]
            if not rooted_entries:
                issues.append(
                    f"PKG_CONFIG_PATH has no rooted entries under {rooted_prefix}; Poppler backend may drift"
                )
            else:
                existing_rooted = [path for path in rooted_entries if path.exists()]
                if not existing_rooted:
                    issues.append(
                        "PKG_CONFIG_PATH rooted entries are stale/missing; expected rooted Poppler pkg-config dirs"
                    )
                else:
                    poppler_pc_found = False
                    for entry in existing_rooted:
                        if (entry / "poppler-qt6.pc").exists() or (entry / "poppler-cpp.pc").exists():
                            poppler_pc_found = True
                            break
                    if not poppler_pc_found:
                        issues.append(
                            "no rooted poppler-qt6.pc/poppler-cpp.pc found in rooted PKG_CONFIG_PATH entries"
                        )

    return issues


def detect_selected_poppler_backend(cache: dict[str, str]) -> tuple[str, str, str]:
    qt6_found = cache_truthy(cache.get("POPPLER_QT6_FOUND", ""))
    cpp_found = cache_truthy(cache.get("POPPLER_CPP_FOUND", ""))
    if qt6_found:
        return ("poppler-qt6", cache.get("POPPLER_QT6_INCLUDEDIR", ""), cache.get("POPPLER_QT6_LIBDIR", ""))
    if cpp_found:
        return ("poppler-cpp", cache.get("POPPLER_CPP_INCLUDEDIR", ""), cache.get("POPPLER_CPP_LIBDIR", ""))
    return ("none", "", "")


def run_build_contract(args: argparse.Namespace) -> None:
    build_dir = Path(args.build_dir).resolve()
    root_dir = Path(args.root_dir).resolve()
    rooted_prefix = root_dir / ".build" / "nix-gcroots"
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
        elif not is_path_lexically_rooted(onnx_include_dir, rooted_prefix):
            failures.append(
                "configured ONNXRuntime_INCLUDE_DIR is not rooted under "
                f"{rooted_prefix}: {onnx_include_dir}"
            )
        if not onnx_library:
            failures.append("configured build is missing ONNXRuntime_LIBRARY")
        elif not Path(onnx_library).exists():
            failures.append(
                f"configured build points to a stale ONNX library ({onnx_library})"
            )
        elif not is_path_lexically_rooted(onnx_library, rooted_prefix):
            failures.append(
                "configured ONNXRuntime_LIBRARY is not rooted under "
                f"{rooted_prefix}: {onnx_library}"
            )
        env_onnx_include = os.environ.get("ONNXRuntime_INCLUDE_DIR", "")
        env_onnx_library = os.environ.get("ONNXRuntime_LIBRARY", "")
        if env_onnx_include and onnx_include_dir and env_onnx_include != onnx_include_dir:
            failures.append(
                "configured ONNXRuntime_INCLUDE_DIR drifted from preflight env "
                f"(env={env_onnx_include}, cache={onnx_include_dir})"
            )
        if env_onnx_library and onnx_library and env_onnx_library != onnx_library:
            failures.append(
                "configured ONNXRuntime_LIBRARY drifted from preflight env "
                f"(env={env_onnx_library}, cache={onnx_library})"
            )
        if "ONNXRUNTIME_FOUND" not in compile_commands:
            failures.append(
                "configured build did not compile ONNX-enabled targets (missing ONNXRUNTIME_FOUND in compile commands)"
            )

    if "pdf" in REQUIRED_CAPABILITIES[profile]:
        require_poppler = cache_truthy(cache.get("BETTERSPOTLIGHT_REQUIRE_POPPLER", ""))
        if not require_poppler:
            failures.append(
                "BETTERSPOTLIGHT_REQUIRE_POPPLER is OFF in a profile that requires pdf capability"
            )
        pkg_config_executable = cache.get("PKG_CONFIG_EXECUTABLE", "")
        env_pkg_config = os.environ.get("PKG_CONFIG_BIN", "")
        if not pkg_config_executable:
            failures.append("configured build is missing PKG_CONFIG_EXECUTABLE")
        elif not Path(pkg_config_executable).exists():
            failures.append(
                f"configured PKG_CONFIG_EXECUTABLE is missing or stale: {pkg_config_executable}"
            )
        elif not is_path_lexically_rooted(pkg_config_executable, rooted_prefix):
            failures.append(
                "configured PKG_CONFIG_EXECUTABLE is not rooted under "
                f"{rooted_prefix}: {pkg_config_executable}"
            )
        if env_pkg_config and pkg_config_executable and env_pkg_config != pkg_config_executable:
            failures.append(
                "configured PKG_CONFIG_EXECUTABLE drifted from preflight env "
                f"(env={env_pkg_config}, cache={pkg_config_executable})"
            )
        poppler_qt6 = cache.get("POPPLER_QT6_FOUND", "")
        poppler_cpp = cache.get("POPPLER_CPP_FOUND", "")
        if not (cache_truthy(poppler_qt6) or cache_truthy(poppler_cpp)):
            failures.append(
                "configured build has no Poppler backend (POPPLER_QT6_FOUND/POPPLER_CPP_FOUND not truthy)"
            )
        selected_backend, include_dir, lib_dir = detect_selected_poppler_backend(cache)
        if selected_backend != "none":
            if not include_dir or not Path(include_dir).exists():
                failures.append(
                    f"configured {selected_backend} include dir is missing or stale: {include_dir or '<empty>'}"
                )
            elif not is_path_lexically_rooted(include_dir, rooted_prefix):
                failures.append(
                    f"configured {selected_backend} include dir is not rooted under "
                    f"{rooted_prefix}: {include_dir}"
                )
            if not lib_dir or not Path(lib_dir).exists():
                failures.append(
                    f"configured {selected_backend} library dir is missing or stale: {lib_dir or '<empty>'}"
                )
            elif not is_path_lexically_rooted(lib_dir, rooted_prefix):
                failures.append(
                    f"configured {selected_backend} library dir is not rooted under "
                    f"{rooted_prefix}: {lib_dir}"
                )
        env_backend = os.environ.get("BS_DEV_POPPLER_BACKEND", "")
        if env_backend and env_backend not in {"none", selected_backend}:
            failures.append(
                "configured Poppler backend drifted from preflight env "
                f"(env={env_backend}, cache={selected_backend})"
            )

    failures.extend(
        rooted_native_backend_issues(
            root_dir,
            require_semantic=("semantic" in REQUIRED_CAPABILITIES[profile]),
            require_pdf=("pdf" in REQUIRED_CAPABILITIES[profile]),
        )
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


def run(
    cmd: list[str],
    *,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, capture_output=True, check=False, env=env)


def detect_poppler() -> tuple[bool, str]:
    pkg_config = os.environ.get("PKG_CONFIG_BIN", "") or shutil.which("pkg-config")
    if not pkg_config:
        return False, "pkg-config not found"
    env = os.environ.copy()
    for pkg_name in ("poppler-qt6", "poppler-cpp"):
        result = run([pkg_config, "--exists", pkg_name], env=env)
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
    failures.extend(
        rooted_native_backend_issues(
            root_dir,
            require_semantic=("semantic" in REQUIRED_CAPABILITIES[args.profile]),
            require_pdf=("pdf" in REQUIRED_CAPABILITIES[args.profile]),
        )
    )

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


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_runtime_parity(args: argparse.Namespace) -> None:
    build_dir = Path(args.build_dir).resolve()
    app_bundle = build_dir / "src" / "app" / "betterspotlight.app"
    helpers_dir = app_bundle / "Contents" / "Helpers"
    service_root = build_dir / "src" / "services"

    failures: list[str] = []
    checked = 0

    if not helpers_dir.exists():
        failures.append(f"app bundle helpers directory missing: {helpers_dir}")

    for helper in HELPER_NAMES:
        target_binary = service_root / helper / f"betterspotlight-{helper}"
        bundle_binary = helpers_dir / f"betterspotlight-{helper}"
        if not target_binary.exists():
            failures.append(f"helper target missing: {target_binary}")
            continue
        if not bundle_binary.exists():
            failures.append(f"bundled helper missing: {bundle_binary}")
            continue
        checked += 1
        target_digest = file_digest(target_binary)
        bundle_digest = file_digest(bundle_binary)
        if target_digest != bundle_digest:
            failures.append(
                "bundled helper drifted from helper target: "
                f"{helper} (target={target_digest[:12]} bundle={bundle_digest[:12]})"
            )

    log(f"Runtime parity scanned {checked} helper binaries.")
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


def run_fixture_integrity(args: argparse.Namespace) -> None:
    root_dir = Path(args.root_dir).resolve()
    fixture_root = root_dir / "Tests" / "Fixtures" / "standard_home_v1"
    corpus_path = root_dir / "Tests" / "relevance" / "test_corpus.json"
    generator_script = root_dir / "Tests" / "Fixtures" / "generate_standard_home.sh"
    this_script = Path(__file__).resolve()

    failures: list[str] = []

    if not fixture_root.is_dir():
        failures.append(f"fixture root missing: {fixture_root}")
    if not corpus_path.is_file():
        failures.append(f"fixture corpus missing: {corpus_path}")
    if not generator_script.is_file():
        failures.append(f"fixture generator script missing: {generator_script}")
    elif not os.access(generator_script, os.X_OK):
        failures.append(f"fixture generator script is not executable: {generator_script}")

    drift_hits: list[str] = []
    drift_tokens = ("tests/fixtures", "tests/Fixtures", "tests/relevance")
    for scan_root in (root_dir / "docs", root_dir / "scripts", root_dir / "Tests"):
        if not scan_root.exists():
            continue
        for path in scan_root.rglob("*"):
            if not path.is_file():
                continue
            if path.resolve() == this_script:
                continue
            if "__pycache__" in path.parts or path.suffix in {".pyc", ".pyo"}:
                continue
            # Fixture payload files can contain arbitrary text; path-casing drift checks
            # should focus on docs/scripts/tests code, not corpus content.
            if "Fixtures" in path.parts:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for line_no, line in enumerate(text.splitlines(), start=1):
                if any(token in line for token in drift_tokens):
                    drift_hits.append(f"{path}:{line_no}: {line.strip()}")
                    if len(drift_hits) >= 20:
                        break
            if len(drift_hits) >= 20:
                break
        if len(drift_hits) >= 20:
            break
    if drift_hits:
        failures.append("path-casing drift detected (expected canonical Tests/... paths):")
        failures.extend(drift_hits)

    if corpus_path.is_file() and fixture_root.is_dir():
        try:
            corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            failures.append(f"fixture corpus is not valid JSON: {exc}")
            corpus = {}
        queries = corpus.get("queries", [])
        if not isinstance(queries, list):
            failures.append("fixture corpus field 'queries' must be a list")
            queries = []

        for idx, entry in enumerate(queries):
            if not isinstance(entry, dict):
                failures.append(f"query entry at index {idx} is not an object")
                continue
            query_id = str(entry.get("id", f"index-{idx}"))
            expected = entry.get("expected_files", [])
            if not isinstance(expected, list):
                failures.append(f"{query_id}: expected_files must be a list")
                continue
            for rel in expected:
                if not isinstance(rel, str) or not rel.strip():
                    failures.append(f"{query_id}: invalid expected file entry: {rel!r}")
                    continue
                target = fixture_root / rel
                if not target.exists():
                    failures.append(f"{query_id}: expected fixture file missing: {target}")

    if fixture_root.is_dir():
        pdf_files = sorted(fixture_root.rglob("*.pdf"))
        if not pdf_files:
            failures.append(f"no PDF fixtures found under {fixture_root}")
        for pdf_path in pdf_files:
            try:
                blob = pdf_path.read_bytes()
            except OSError as exc:
                failures.append(f"unable to read PDF fixture {pdf_path}: {exc}")
                continue
            rel = pdf_path.relative_to(fixture_root)
            if not blob.startswith(b"%PDF-"):
                failures.append(f"{rel}: missing %PDF- header")
                continue
            if b"%%EOF" not in blob[-4096:]:
                failures.append(f"{rel}: missing %%EOF trailer near end of file")

    if failures:
        for item in failures:
            log(f"Error: {item}")
        raise SystemExit(1)

    log(f"Fixture integrity passed for {fixture_root}.")


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
    p_contract.add_argument("--root-dir", default=str(Path(__file__).resolve().parents[2]))
    p_contract.add_argument("--profile", choices=PROFILES, default="core-hermetic")
    p_contract.add_argument("--allow-degraded", action="store_true")
    p_contract.set_defaults(func=run_build_contract)

    p_runtime = subparsers.add_parser(
        "runtime-parity",
        help="verify bundled helper binaries match helper build targets",
    )
    p_runtime.add_argument("--build-dir", required=True)
    p_runtime.set_defaults(func=run_runtime_parity)

    p_docs = subparsers.add_parser("docs-parity", help="verify docs reference supported script flow")
    p_docs.add_argument("--root-dir", required=True)
    p_docs.set_defaults(func=run_docs_parity)

    p_fixture = subparsers.add_parser(
        "fixture-integrity",
        help="verify canonical Tests/... fixture paths and deterministic fixture integrity",
    )
    p_fixture.add_argument("--root-dir", required=True)
    p_fixture.set_defaults(func=run_fixture_integrity)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
