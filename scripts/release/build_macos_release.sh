#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BS_RELEASE_BUILD_DIR:-$ROOT_DIR/build-release}"
OUTPUT_DIR="${BS_RELEASE_OUTPUT_DIR:-$ROOT_DIR/dist/release}"
ENTITLEMENTS_PATH="${BS_ENTITLEMENTS_PATH:-$ROOT_DIR/packaging/macos/entitlements.plist}"
BUILD_TYPE="${BS_RELEASE_BUILD_TYPE:-Release}"
APP_NAME="${BS_RELEASE_APP_NAME:-BetterSpotlight}"

ENABLE_SPARKLE="${BS_ENABLE_SPARKLE:-0}"
RUN_MACDEPLOYQT="${BS_RUN_MACDEPLOYQT:-1}"
MACDEPLOYQT_INCLUDE_QMLDIR="${BS_MACDEPLOYQT_INCLUDE_QMLDIR:-1}"
CREATE_DMG="${BS_CREATE_DMG:-1}"
SIGN_ARTIFACTS="${BS_SIGN:-0}"
NOTARIZE="${BS_NOTARIZE:-0}"
SKIP_BUILD="${BS_SKIP_BUILD:-0}"
MACDEPLOYQT_TIMEOUT_SEC="${BS_MACDEPLOYQT_TIMEOUT_SEC:-600}"
ADHOC_SIGN_WHEN_UNSIGNED="${BS_ADHOC_SIGN_WHEN_UNSIGNED:-1}"
REQUIRE_ONLINE_RANKER_BOOTSTRAP="${BS_REQUIRE_ONLINE_RANKER_BOOTSTRAP:-0}"

APP_BUILD_PATH="$BUILD_DIR/src/app/betterspotlight.app"
APP_STAGE_PATH="$OUTPUT_DIR/${APP_NAME}.app"
ZIP_PATH="$OUTPUT_DIR/${APP_NAME}.app.zip"
DMG_PATH="$OUTPUT_DIR/${APP_NAME}-${BUILD_TYPE}.dmg"
APP_NOTARY_JSON="$OUTPUT_DIR/notary_app.json"
DMG_NOTARY_JSON="$OUTPUT_DIR/notary_dmg.json"
SUMMARY_JSON="$OUTPUT_DIR/release_summary.json"

function bool_is_true() {
    local v="${1:-0}"
    v="${v,,}"
    [[ "$v" == "1" || "$v" == "true" || "$v" == "yes" || "$v" == "on" ]]
}

function require_cmd() {
    local name="$1"
    if ! command -v "$name" >/dev/null 2>&1; then
        echo "Error: required command not found: $name" >&2
        exit 1
    fi
}

function require_env() {
    local key="$1"
    if [[ -z "${!key:-}" ]]; then
        echo "Error: environment variable $key is required" >&2
        exit 1
    fi
}

function run_command_with_timeout() {
    local timeout_sec="$1"
    shift
    local log_file="$1"
    shift

    "$@" >"$log_file" 2>&1 &
    local pid="$!"
    local elapsed=0
    while kill -0 "$pid" 2>/dev/null; do
        if (( elapsed >= timeout_sec )); then
            kill -TERM "$pid" 2>/dev/null || true
            sleep 1
            kill -KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            return 124
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    wait "$pid"
}

function find_macdeployqt() {
    if command -v macdeployqt >/dev/null 2>&1; then
        command -v macdeployqt
        return 0
    fi
    if command -v brew >/dev/null 2>&1; then
        local qt_prefix
        qt_prefix="$(brew --prefix qt@6 2>/dev/null || true)"
        if [[ -n "$qt_prefix" && -x "$qt_prefix/bin/macdeployqt" ]]; then
            echo "$qt_prefix/bin/macdeployqt"
            return 0
        fi
    fi
    return 1
}

function configure_and_build() {
    local sparkle_flag
    if bool_is_true "$ENABLE_SPARKLE"; then
        sparkle_flag="ON"
    else
        sparkle_flag="OFF"
    fi

    local cmake_args=(
        -S "$ROOT_DIR"
        -B "$BUILD_DIR"
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        -DBETTERSPOTLIGHT_ENABLE_SPARKLE="$sparkle_flag"
        -DBETTERSPOTLIGHT_FETCH_MODELS=OFF
        -DBETTERSPOTLIGHT_FETCH_MAX_QUALITY_MODEL=OFF
    )

    if command -v brew >/dev/null 2>&1; then
        local qt_prefix
        qt_prefix="$(brew --prefix qt@6 2>/dev/null || true)"
        if [[ -n "$qt_prefix" ]]; then
            cmake_args+=("-DCMAKE_PREFIX_PATH=$qt_prefix")
        fi
    fi

    cmake "${cmake_args[@]}"
    cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"
}

function ensure_helpers_in_bundle() {
    local helpers_dir="$APP_STAGE_PATH/Contents/Helpers"
    mkdir -p "$helpers_dir"

    local helper_names=(indexer extractor query inference)
    local helper
    for helper in "${helper_names[@]}"; do
        local src="$BUILD_DIR/src/services/$helper/betterspotlight-$helper"
        local dst="$helpers_dir/betterspotlight-$helper"
        if [[ ! -x "$src" ]]; then
            echo "Error: helper binary missing: $src" >&2
            exit 1
        fi
        cp -f "$src" "$dst"
        chmod +x "$dst"
    done
}

function verify_bundle_contents() {
    local models_dir="$APP_STAGE_PATH/Contents/Resources/models"
    local required_models=(
        "$models_dir/manifest.json"
        "$models_dir/vocab.txt"
        "$models_dir/bge-small-en-v1.5-int8.onnx"
    )
    local file
    for file in "${required_models[@]}"; do
        if [[ ! -s "$file" ]]; then
            echo "Error: required model bootstrap artifact missing: $file" >&2
            exit 1
        fi
    done

    local ranker_model_dir="$models_dir/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc"
    local ranker_metadata="$models_dir/online-ranker-v1/bootstrap/metadata.json"
    if bool_is_true "$REQUIRE_ONLINE_RANKER_BOOTSTRAP"; then
        if [[ ! -d "$ranker_model_dir" ]]; then
            echo "Error: required online-ranker CoreML bootstrap missing: $ranker_model_dir" >&2
            exit 1
        fi
        if [[ ! -s "$ranker_metadata" ]]; then
            echo "Error: required online-ranker metadata missing: $ranker_metadata" >&2
            exit 1
        fi
    elif [[ ! -d "$ranker_model_dir" || ! -s "$ranker_metadata" ]]; then
        echo "Warning: online-ranker CoreML bootstrap not bundled; fallback SGD ranker will be used." >&2
    fi
}

function verify_qt_platform_plugins() {
    local cocoa_plugin="$APP_STAGE_PATH/Contents/PlugIns/platforms/libqcocoa.dylib"
    if [[ ! -f "$cocoa_plugin" ]]; then
        echo "Error: required Qt platform plugin missing: $cocoa_plugin" >&2
        echo "Hint: ensure macdeployqt runs successfully and Qt plugins are available." >&2
        exit 1
    fi
}

function maybe_run_macdeployqt() {
    if ! bool_is_true "$RUN_MACDEPLOYQT"; then
        return 0
    fi

    local macdeployqt_bin
    if ! macdeployqt_bin="$(find_macdeployqt)"; then
        echo "Warning: macdeployqt not found; skipping framework deployment." >&2
        return 0
    fi

    local deploy_args=("$APP_STAGE_PATH" -always-overwrite -no-strip)
    local qml_dir="$ROOT_DIR/src/app/qml"
    if bool_is_true "$MACDEPLOYQT_INCLUDE_QMLDIR" && [[ -d "$qml_dir" ]]; then
        deploy_args+=("-qmldir=$qml_dir")
    fi

    local helper_exec
    for helper_exec in \
        "$APP_STAGE_PATH/Contents/Helpers/betterspotlight-indexer" \
        "$APP_STAGE_PATH/Contents/Helpers/betterspotlight-extractor" \
        "$APP_STAGE_PATH/Contents/Helpers/betterspotlight-query" \
        "$APP_STAGE_PATH/Contents/Helpers/betterspotlight-inference"; do
        if [[ -x "$helper_exec" ]]; then
            deploy_args+=("-executable=$helper_exec")
        fi
    done
    local deploy_log
    deploy_log="$(mktemp)"
    set +e
    run_command_with_timeout "$MACDEPLOYQT_TIMEOUT_SEC" "$deploy_log" \
        "$macdeployqt_bin" "${deploy_args[@]}"
    local status="$?"
    set -e
    if [[ "$status" -ne 0 ]]; then
        if [[ "$status" -eq 124 ]]; then
            echo "Error: macdeployqt timed out after ${MACDEPLOYQT_TIMEOUT_SEC}s." >&2
        fi
        cat "$deploy_log" >&2
        rm -f "$deploy_log" 2>/dev/null || true
        return 1
    fi

    # Keep warnings visible (macdeployqt may emit non-fatal resolution warnings).
    if [[ -s "$deploy_log" ]]; then
        cat "$deploy_log" >&2
    fi
    rm -f "$deploy_log" 2>/dev/null || true
}

function sanitize_bundle_binary_references() {
    python3 - "$APP_STAGE_PATH" <<'PY'
import os
import pathlib
import re
import subprocess
import sys

app = pathlib.Path(sys.argv[1]).resolve()
frameworks_dir = app / "Contents" / "Frameworks"
if not frameworks_dir.exists():
    print("Frameworks directory missing; cannot sanitize bundle.", file=sys.stderr)
    sys.exit(1)

def is_macho(path: pathlib.Path) -> bool:
    try:
        out = subprocess.check_output(["file", str(path)], text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return False
    return "Mach-O" in out

def parse_deps(path: pathlib.Path):
    out = subprocess.check_output(["otool", "-L", str(path)], text=True)
    deps = []
    for line in out.splitlines()[1:]:
        line = line.strip()
        if not line:
            continue
        m = re.match(r"(.+?)\s+\(", line)
        if m:
            deps.append(m.group(1))
    return deps

def parse_rpaths(path: pathlib.Path):
    out = subprocess.check_output(["otool", "-l", str(path)], text=True)
    lines = out.splitlines()
    rpaths = []
    i = 0
    while i < len(lines):
        if lines[i].strip() == "cmd LC_RPATH":
            j = i + 1
            while j < len(lines) and "path " not in lines[j]:
                j += 1
            if j < len(lines):
                part = lines[j].strip()
                if part.startswith("path "):
                    value = part.split("path ", 1)[1].split(" (offset", 1)[0]
                    rpaths.append(value)
        i += 1
    return rpaths

def parse_install_id(path: pathlib.Path):
    try:
        out = subprocess.check_output(["otool", "-D", str(path)], text=True)
    except subprocess.CalledProcessError:
        return None
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    if len(lines) < 2:
        return None
    return lines[1]

def is_system_dep(dep: str) -> bool:
    return dep.startswith("/System/") or dep.startswith("/usr/lib/")

def find_bundled_target(dep: str) -> pathlib.Path | None:
    if dep.startswith("@rpath/"):
        suffix = dep[len("@rpath/"):]
        suffix_path = pathlib.Path(suffix)
        direct = frameworks_dir / suffix_path
        if direct.exists():
            return direct
        # Fallback for flattened dylib names.
        by_name = frameworks_dir / suffix_path.name
        if by_name.exists():
            return by_name
        marker = ".framework/"
        suffix_posix = suffix_path.as_posix()
        if marker in suffix_posix:
            fw_root = suffix_posix.split(marker, 1)[0] + ".framework"
            fw_name = pathlib.Path(fw_root).name
            bin_name = suffix_path.name
            candidates = [
                frameworks_dir / fw_name / "Versions" / "A" / bin_name,
                frameworks_dir / fw_name / bin_name,
            ]
            for candidate in candidates:
                if candidate.exists():
                    return candidate
        return None

    dep_path = pathlib.Path(dep)
    marker = ".framework/"
    dep_posix = dep_path.as_posix()
    if marker in dep_posix:
        fw_root = dep_posix.split(marker, 1)[0] + ".framework"
        fw_name = pathlib.Path(fw_root).name
        bin_name = dep_path.name
        candidates = [
            frameworks_dir / fw_name / "Versions" / "A" / bin_name,
            frameworks_dir / fw_name / bin_name,
        ]
        for candidate in candidates:
            if candidate.exists():
                return candidate
        return None
    candidate = frameworks_dir / dep_path.name
    if candidate.exists():
        return candidate
    return None

def is_framework_rpath(dep: str) -> bool:
    return dep.startswith("@rpath/") and ".framework/" in dep

def desired_install_id(path: pathlib.Path) -> str | None:
    try:
        rel = path.relative_to(frameworks_dir)
    except ValueError:
        return None
    parts = rel.parts
    if ".framework" in rel.as_posix():
        fw_part = next((p for p in parts if p.endswith(".framework")), None)
        if fw_part is None:
            return None
        bin_name = path.name
        return f"@rpath/{fw_part}/Versions/A/{bin_name}"
    return f"@rpath/{path.name}"

# Framework bundles often contain top-level symlinked binaries (e.g. Foo.framework/Foo
# -> Versions/Current/Foo). Rewriting install names via the symlink path can produce
# incorrect @loader_path relative links. Only operate on real Mach-O files.
machos = [p for p in app.rglob("*") if p.is_file() and not p.is_symlink() and is_macho(p)]
unresolved_abs = []
unresolved_rpath = []

for macho in machos:
    install_id = parse_install_id(macho)
    if install_id and install_id.startswith("/") and not is_system_dep(install_id):
        new_id = desired_install_id(macho)
        if new_id:
            subprocess.check_call(["install_name_tool", "-id", new_id, str(macho)])

    try:
        deps = parse_deps(macho)
    except subprocess.CalledProcessError:
        continue
    for dep in deps:
        if install_id and dep == install_id:
            continue
        if not dep.startswith("/"):
            if dep.startswith("@rpath/"):
                # Keep framework-style @rpath links intact (Qt frameworks rely on them).
                if is_framework_rpath(dep):
                    continue
                target = find_bundled_target(dep)
                if target is None:
                    unresolved_rpath.append((str(macho), dep))
                    continue
                rel = os.path.relpath(str(target), str(macho.parent))
                new_dep = f"@loader_path/{rel}"
                if dep != new_dep:
                    subprocess.check_call(["install_name_tool", "-change", dep, new_dep, str(macho)])
            continue
        if is_system_dep(dep):
            continue
        target = find_bundled_target(dep)
        if target is None:
            unresolved_abs.append((str(macho), dep))
            continue
        rel = os.path.relpath(str(target), str(macho.parent))
        new_dep = f"@loader_path/{rel}"
        if dep != new_dep:
            subprocess.check_call(["install_name_tool", "-change", dep, new_dep, str(macho)])

    try:
        rpaths = parse_rpaths(macho)
    except subprocess.CalledProcessError:
        rpaths = []
    for rpath in rpaths:
        if rpath in ("@loader_path/../lib", "@executable_path/../lib"):
            subprocess.call(["install_name_tool", "-delete_rpath", rpath, str(macho)])
            continue
        if not rpath.startswith("/"):
            continue
        if rpath.startswith("/System/") or rpath.startswith("/usr/lib/"):
            continue
        subprocess.call(["install_name_tool", "-delete_rpath", rpath, str(macho)])

if unresolved_rpath:
    print("Unresolved non-framework @rpath dependencies remain after sanitization:", file=sys.stderr)
    for macho, dep in unresolved_rpath:
        print(f"- {macho} -> {dep}", file=sys.stderr)
    sys.exit(1)

if unresolved_abs:
    print("Unresolved non-system dependencies remain after sanitization:", file=sys.stderr)
    for macho, dep in unresolved_abs:
        print(f"- {macho} -> {dep}", file=sys.stderr)
    sys.exit(1)

print("Bundle binary references sanitized.")
PY
}

function verify_portable_bundle() {
    python3 - "$APP_STAGE_PATH" <<'PY'
import pathlib
import re
import subprocess
import sys

app = pathlib.Path(sys.argv[1]).resolve()
frameworks_dir = app / "Contents" / "Frameworks"
main_executable_dir = app / "Contents" / "MacOS"

def is_macho(path: pathlib.Path) -> bool:
    try:
        out = subprocess.check_output(["file", str(path)], text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return False
    return "Mach-O" in out

def bad_dep(dep: str) -> bool:
    dep = dep.strip()
    if not dep:
        return False
    if dep.startswith(("@rpath/", "@loader_path/", "@executable_path/")):
        return False
    if dep.startswith("/System/") or dep.startswith("/usr/lib/"):
        return False
    # Any other absolute path is not portable for distribution.
    return dep.startswith("/")

def resolve_bundle_rpath_target(dep: str) -> pathlib.Path | None:
    if not dep.startswith("@rpath/"):
        return None
    suffix = dep[len("@rpath/"):]
    suffix_path = pathlib.Path(suffix)
    direct = frameworks_dir / suffix_path
    if direct.exists():
        return direct

    flattened = frameworks_dir / suffix_path.name
    if flattened.exists():
        return flattened

    marker = ".framework/"
    suffix_posix = suffix_path.as_posix()
    if marker in suffix_posix:
        fw_root = suffix_posix.split(marker, 1)[0] + ".framework"
        fw_name = pathlib.Path(fw_root).name
        bin_name = suffix_path.name
        candidates = [
            frameworks_dir / fw_name / "Versions" / "A" / bin_name,
            frameworks_dir / fw_name / bin_name,
        ]
        for candidate in candidates:
            if candidate.exists():
                return candidate
    return None

def resolve_loader_path_target(dep: str, macho_path: pathlib.Path) -> pathlib.Path:
    suffix = dep[len("@loader_path/"):]
    return (macho_path.resolve().parent / suffix).resolve()

def resolve_executable_path_targets(dep: str, macho_path: pathlib.Path):
    suffix = dep[len("@executable_path/"):]
    # Primary process executable path.
    primary = (main_executable_dir / suffix).resolve()
    # Helper/service process executable path (for helper-local loads).
    secondary = (macho_path.resolve().parent / suffix).resolve()
    return [primary, secondary]

def parse_deps(path: pathlib.Path):
    out = subprocess.check_output(["otool", "-L", str(path)], text=True)
    deps = []
    for line in out.splitlines()[1:]:
        line = line.strip()
        if not line:
            continue
        m = re.match(r"(.+?)\s+\(", line)
        if m:
            deps.append(m.group(1))
    return deps

def parse_rpaths(path: pathlib.Path):
    out = subprocess.check_output(["otool", "-l", str(path)], text=True)
    lines = out.splitlines()
    rpaths = []
    i = 0
    while i < len(lines):
        if lines[i].strip() == "cmd LC_RPATH":
            j = i + 1
            while j < len(lines) and "path " not in lines[j]:
                j += 1
            if j < len(lines):
                part = lines[j].strip()
                # format: path <value> (offset N)
                if part.startswith("path "):
                    value = part.split("path ", 1)[1].split(" (offset", 1)[0]
                    rpaths.append(value)
        i += 1
    return rpaths

def parse_install_id(path: pathlib.Path):
    try:
        out = subprocess.check_output(["otool", "-D", str(path)], text=True)
    except subprocess.CalledProcessError:
        return None
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    if len(lines) < 2:
        return None
    return lines[1]

bad = []
for p in app.rglob("*"):
    if not p.is_file():
        continue
    if not is_macho(p):
        continue
    try:
        deps = parse_deps(p)
    except subprocess.CalledProcessError:
        continue
    install_id = parse_install_id(p)
    try:
        rel_parts = p.relative_to(app).parts
    except ValueError:
        rel_parts = ()
    strict_missing_checks = (
        rel_parts[:2] == ("Contents", "MacOS")
        or rel_parts[:2] == ("Contents", "Helpers")
        or rel_parts[:2] == ("Contents", "Frameworks")
        or rel_parts[:3] == ("Contents", "PlugIns", "platforms")
    )
    for dep in deps:
        if install_id and dep == install_id:
            continue
        if dep.startswith("@loader_path/"):
            if strict_missing_checks and not resolve_loader_path_target(dep, p).exists():
                bad.append((str(p), "missing_loader_path_dep", dep))
            continue
        if dep.startswith("@executable_path/"):
            if strict_missing_checks and not any(target.exists() for target in resolve_executable_path_targets(dep, p)):
                bad.append((str(p), "missing_executable_path_dep", dep))
            continue
        if dep.startswith("@rpath/"):
            if strict_missing_checks and resolve_bundle_rpath_target(dep) is None:
                bad.append((str(p), "missing_rpath_dep", dep))
            continue
        if bad_dep(dep):
            bad.append((str(p), "dep", dep))
    try:
        rpaths = parse_rpaths(p)
    except subprocess.CalledProcessError:
        rpaths = []
    for r in rpaths:
        if r.startswith("/System/") or r.startswith("/usr/lib/"):
            continue
        if r.startswith(("@loader_path", "@executable_path", "@rpath")):
            continue
        if r.startswith("/"):
            bad.append((str(p), "rpath", r))

if bad:
    print("Portable bundle verification failed. Non-portable references found:")
    for p, kind, ref in bad:
        print(f"- {p} [{kind}] -> {ref}")
    sys.exit(1)

print("Portable bundle verification passed.")
PY
}

function prepare_staged_app() {
    rm -rf "$APP_STAGE_PATH"
    cp -R "$APP_BUILD_PATH" "$APP_STAGE_PATH"
    ensure_helpers_in_bundle
    verify_bundle_contents
    maybe_run_macdeployqt
    verify_qt_platform_plugins
    sanitize_bundle_binary_references
    verify_portable_bundle
    xattr -cr "$APP_STAGE_PATH" || true
}

function codesign_args_base() {
    local identity="$1"
    local args=(--force --sign "$identity")
    if [[ "$identity" != "-" ]]; then
        args+=(--options runtime --timestamp)
    fi
    printf '%s\n' "${args[@]}"
}

function sign_path() {
    local target="$1"
    local identity="$2"
    shift
    shift

    local args=()
    while IFS= read -r line; do
        args+=("$line")
    done < <(codesign_args_base "$identity")

    if [[ "$#" -gt 0 ]]; then
        args+=("$@")
    fi

    codesign "${args[@]}" "$target"
}

function sign_staged_app() {
    require_cmd codesign
    local identity="-"
    local use_entitlements="0"
    if bool_is_true "$SIGN_ARTIFACTS" || bool_is_true "$NOTARIZE"; then
        require_env DEVELOPER_ID
        identity="$DEVELOPER_ID"
        use_entitlements="1"
        if [[ ! -f "$ENTITLEMENTS_PATH" ]]; then
            echo "Error: entitlements file not found: $ENTITLEMENTS_PATH" >&2
            exit 1
        fi
    elif ! bool_is_true "$ADHOC_SIGN_WHEN_UNSIGNED"; then
        return 0
    fi

    local frameworks_dir="$APP_STAGE_PATH/Contents/Frameworks"
    if [[ -d "$frameworks_dir" ]]; then
        while IFS= read -r path; do
            [[ -z "$path" ]] && continue
            sign_path "$path" "$identity"
        done < <(find "$frameworks_dir" -type f \( -name "*.dylib" -o -perm -u+x \) | LC_ALL=C sort)
        while IFS= read -r framework; do
            [[ -z "$framework" ]] && continue
            sign_path "$framework" "$identity"
        done < <(find "$frameworks_dir" -type d -name "*.framework" | LC_ALL=C sort)
    fi

    local plugins_dir="$APP_STAGE_PATH/Contents/PlugIns"
    if [[ -d "$plugins_dir" ]]; then
        while IFS= read -r path; do
            [[ -z "$path" ]] && continue
            sign_path "$path" "$identity"
        done < <(find "$plugins_dir" -type f \( -name "*.dylib" -o -perm -u+x \) | LC_ALL=C sort)
    fi

    local helpers_dir="$APP_STAGE_PATH/Contents/Helpers"
    if [[ -d "$helpers_dir" ]]; then
        while IFS= read -r helper; do
            [[ -z "$helper" ]] && continue
            if [[ "$use_entitlements" == "1" ]]; then
                sign_path "$helper" "$identity" --entitlements "$ENTITLEMENTS_PATH"
            else
                sign_path "$helper" "$identity"
            fi
        done < <(find "$helpers_dir" -type f -perm -u+x | LC_ALL=C sort)
    fi

    if [[ "$use_entitlements" == "1" ]]; then
        sign_path "$APP_STAGE_PATH/Contents/MacOS/betterspotlight" "$identity" --entitlements "$ENTITLEMENTS_PATH"
        sign_path "$APP_STAGE_PATH" "$identity" --entitlements "$ENTITLEMENTS_PATH"
    else
        sign_path "$APP_STAGE_PATH/Contents/MacOS/betterspotlight" "$identity"
        sign_path "$APP_STAGE_PATH" "$identity"
    fi

    codesign --verify --deep --strict --verbose=2 "$APP_STAGE_PATH"
}

function run_notary_submit() {
    local artifact_path="$1"
    local output_json="$2"

    xcrun notarytool submit "$artifact_path" \
        --apple-id "$APPLE_ID" \
        --password "$APPLE_APP_SPECIFIC_PASSWORD" \
        --team-id "$TEAM_ID" \
        --wait \
        --output-format json > "$output_json"

    local status
    status="$(python3 - "$output_json" <<'PY'
import json
import sys

with open(sys.argv[1], 'r', encoding='utf-8') as fh:
    payload = json.load(fh)
print(payload.get('status', 'unknown'))
PY
)"

    if [[ "$status" != "Accepted" ]]; then
        echo "Error: notarization rejected for $artifact_path (status=$status)" >&2
        echo "See notary response: $output_json" >&2
        exit 1
    fi
}

function maybe_notarize_app() {
    if ! bool_is_true "$NOTARIZE"; then
        return 0
    fi

    require_cmd xcrun
    require_cmd ditto
    require_env APPLE_ID
    require_env APPLE_APP_SPECIFIC_PASSWORD
    require_env TEAM_ID

    rm -f "$ZIP_PATH"
    ditto -c -k --sequesterRsrc --keepParent "$APP_STAGE_PATH" "$ZIP_PATH"
    run_notary_submit "$ZIP_PATH" "$APP_NOTARY_JSON"
    xcrun stapler staple "$APP_STAGE_PATH"
    xcrun stapler validate "$APP_STAGE_PATH"
}

function maybe_create_and_notarize_dmg() {
    if ! bool_is_true "$CREATE_DMG"; then
        return 0
    fi

    require_cmd hdiutil
    require_cmd ditto
    local stage_dir
    stage_dir="$(mktemp -d)"
    local dmg_tmp_dir
    dmg_tmp_dir="$(mktemp -d)"
    local mount_dir
    mount_dir="$(mktemp -d)"
    trap "hdiutil detach '$mount_dir' -force >/dev/null 2>&1 || true; rm -rf '$stage_dir' '$dmg_tmp_dir' '$mount_dir'" EXIT

    cp -R "$APP_STAGE_PATH" "$stage_dir/${APP_NAME}.app"
    ln -s /Applications "$stage_dir/Applications"
    xattr -cr "$stage_dir" || true

    rm -f "$DMG_PATH"
    local dmg_rw="$dmg_tmp_dir/${APP_NAME}-${BUILD_TYPE}-rw.dmg"
    local dmg_tmp="$dmg_tmp_dir/${APP_NAME}-${BUILD_TYPE}.dmg"
    rm -f "$dmg_rw" "$dmg_tmp"

    local size_kb
    size_kb="$(du -sk "$stage_dir" | awk '{print $1}')"
    size_kb=$((size_kb + 512000))

    hdiutil create \
        -size "${size_kb}k" \
        -fs APFS \
        -volname "$APP_NAME" \
        "$dmg_rw"

    hdiutil attach "$dmg_rw" -mountpoint "$mount_dir" -nobrowse -noautoopen
    ditto "$stage_dir" "$mount_dir"
    sync

    local detached=0
    local attempt
    set +e
    for attempt in 1 2 3 4 5; do
        hdiutil detach "$mount_dir" >/dev/null 2>&1
        if [[ "$?" -eq 0 ]]; then
            detached=1
            break
        fi
        hdiutil detach -force "$mount_dir" >/dev/null 2>&1
        if [[ "$?" -eq 0 ]]; then
            detached=1
            break
        fi
        sleep 2
    done
    set -e
    if [[ "$detached" -ne 1 ]]; then
        echo "Error: failed to detach temporary DMG mount at $mount_dir" >&2
        exit 1
    fi

    hdiutil convert "$dmg_rw" -format UDZO -imagekey zlib-level=9 -o "$dmg_tmp"
    mv -f "$dmg_tmp" "$DMG_PATH"

    if bool_is_true "$NOTARIZE"; then
        run_notary_submit "$DMG_PATH" "$DMG_NOTARY_JSON"
        xcrun stapler staple "$DMG_PATH"
        xcrun stapler validate "$DMG_PATH"
    fi

    rm -rf "$stage_dir" "$dmg_tmp_dir" "$mount_dir"
    trap - EXIT
}

function write_summary() {
    python3 - "$SUMMARY_JSON" "$APP_STAGE_PATH" "$ZIP_PATH" "$DMG_PATH" \
        "$BUILD_TYPE" "$ENABLE_SPARKLE" "$SIGN_ARTIFACTS" "$NOTARIZE" "$CREATE_DMG" <<'PY'
import json
import os
import sys

summary_path, app_path, zip_path, dmg_path, build_type, sparkle, sign, notarize, create_dmg = sys.argv[1:]

def size(path):
    if not path:
        return 0
    if os.path.isdir(path):
        total = 0
        for root, _, files in os.walk(path):
            for name in files:
                fp = os.path.join(root, name)
                try:
                    total += os.path.getsize(fp)
                except OSError:
                    pass
        return total
    if os.path.isfile(path):
        return os.path.getsize(path)
    return 0

summary = {
    "buildType": build_type,
    "appPath": app_path,
    "appExists": os.path.isdir(app_path),
    "appSizeBytes": size(app_path),
    "zipPath": zip_path,
    "zipExists": os.path.isfile(zip_path),
    "zipSizeBytes": size(zip_path),
    "dmgPath": dmg_path,
    "dmgExists": os.path.isfile(dmg_path),
    "dmgSizeBytes": size(dmg_path),
    "sparkleEnabled": sparkle.lower() in {"1", "true", "yes", "on"},
    "signEnabled": sign.lower() in {"1", "true", "yes", "on"},
    "notarizeEnabled": notarize.lower() in {"1", "true", "yes", "on"},
    "createDmgEnabled": create_dmg.lower() in {"1", "true", "yes", "on"},
}

with open(summary_path, "w", encoding="utf-8") as fh:
    json.dump(summary, fh, indent=2)

print(json.dumps(summary, indent=2))
PY
}

require_cmd cmake
require_cmd cp
require_cmd python3
mkdir -p "$OUTPUT_DIR"

if ! bool_is_true "$SKIP_BUILD"; then
    configure_and_build
fi

if [[ ! -d "$APP_BUILD_PATH" || ! -x "$APP_BUILD_PATH/Contents/MacOS/betterspotlight" ]]; then
    echo "Error: built app bundle not found at $APP_BUILD_PATH" >&2
    exit 1
fi

prepare_staged_app
sign_staged_app
maybe_notarize_app
maybe_create_and_notarize_dmg
write_summary

echo "Release pipeline complete. Artifacts in: $OUTPUT_DIR"
