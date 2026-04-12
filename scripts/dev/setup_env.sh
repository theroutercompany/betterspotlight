#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STATE_DIR="${ROOT_DIR}/.build"
NIX_GCROOT_DIR="${STATE_DIR}/nix-gcroots"
OUTPUT_PATH="${BS_DEV_ENV_FILE:-${STATE_DIR}/dev-env.sh}"
CHECK_ONLY=0
QUIET=0
ALLOW_DEGRADED_PDF="${BS_DEV_ALLOW_DEGRADED_PDF:-0}"

usage() {
    cat <<'EOF'
Usage: scripts/dev/setup_env.sh [--write-env-file PATH] [--check-only] [--quiet] [--allow-degraded-pdf]

Validates the local macOS toolchain for BetterSpotlight development and writes
a shell fragment with the required Qt/ONNX/runtime environment exports.
EOF
}

log() {
    if [[ "${QUIET}" -eq 0 ]]; then
        printf '[dev-setup] %s\n' "$*"
    fi
}

fail() {
    printf '[dev-setup] Error: %s\n' "$*" >&2
    exit 1
}

require_cmd() {
    local name="$1"
    command -v "${name}" >/dev/null 2>&1 || fail "required command not found: ${name}"
}

find_store_prefix() {
    local pattern="$1"
    local required_path="$2"
    local candidate

    while IFS= read -r candidate; do
        if [[ -e "${candidate}/${required_path}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done < <(compgen -G "${pattern}" || true)

    return 1
}

ensure_gcroot_symlink() {
    local root_name="$1"
    local target_path="$2"
    local link_path="${NIX_GCROOT_DIR}/${root_name}"

    [[ -e "${target_path}" ]] || fail "cannot create gcroot ${root_name}; target missing: ${target_path}"
    mkdir -p "${NIX_GCROOT_DIR}"
    ln -sfn "${target_path}" "${link_path}"
    printf '%s\n' "${link_path}"
}

nix_build_flake_output() {
    local attr="$1"
    local root_name="${2:-}"
    local out_link=""

    if [[ -z "${root_name}" ]]; then
        root_name="$(printf '%s' "${attr}" | tr -cs 'A-Za-z0-9' '-')"
        root_name="${root_name#-}"
        root_name="${root_name%-}"
    fi
    out_link="${NIX_GCROOT_DIR}/${root_name}"
    mkdir -p "${NIX_GCROOT_DIR}"

    if [[ -L "${out_link}" ]]; then
        if [[ -e "${out_link}" ]]; then
            printf '%s\n' "${out_link}"
            return 0
        fi
        rm -f "${out_link}"
    fi

    nix build --impure --out-link "${out_link}" --expr "
let
  flake = builtins.getFlake \"${ROOT_DIR}\";
  pkgs = import flake.inputs.nixpkgs {
    system = builtins.currentSystem;
    config.allowUnfree = true;
  };
in
  ${attr}
    " >/dev/null

    [[ -e "${out_link}" ]] || fail "failed to root nix output for ${attr} at ${out_link}"
    printf '%s\n' "${out_link}"
}

detect_poppler_backend() {
    local pkg_config_path="$1"

    if [[ -z "${PKG_CONFIG_BIN:-}" || ! -x "${PKG_CONFIG_BIN}" ]]; then
        return 0
    fi

    if PKG_CONFIG_PATH="${pkg_config_path}" "${PKG_CONFIG_BIN}" --exists poppler-qt6; then
        printf 'poppler-qt6\n'
        return 0
    fi

    if PKG_CONFIG_PATH="${pkg_config_path}" "${PKG_CONFIG_BIN}" --exists poppler-cpp; then
        printf 'poppler-cpp\n'
        return 0
    fi
}

resolve_host_qt() {
    local qt_version="$1"
    local qtbase_prefix="$2"
    local qtdeclarative_prefix=""
    local qttools_prefix=""

    if [[ -e "${qtbase_prefix}/lib/cmake/Qt6Qml/Qt6QmlConfig.cmake" ]]; then
        qtdeclarative_prefix="${qtbase_prefix}"
    else
        qtdeclarative_prefix="$(
            find_store_prefix "/nix/store/*-qtdeclarative-${qt_version}" \
                "lib/cmake/Qt6Qml/Qt6QmlConfig.cmake" \
            || find_store_prefix "/nix/store/*-qtdeclarative-*" \
                "lib/cmake/Qt6Qml/Qt6QmlConfig.cmake" \
            || true
        )"
    fi

    if [[ -e "${qtbase_prefix}/bin/macdeployqt" ]]; then
        qttools_prefix="${qtbase_prefix}"
    else
        qttools_prefix="$(
            find_store_prefix "/nix/store/*-qttools-${qt_version}" \
                "bin/macdeployqt" \
            || find_store_prefix "/nix/store/*-qttools-*" \
                "bin/macdeployqt" \
            || true
        )"
    fi

    if [[ -n "${qtdeclarative_prefix}" && -n "${qttools_prefix}" ]]; then
        printf '%s\n%s\n%s\n' "${qtbase_prefix}" "${qtdeclarative_prefix}" "${qttools_prefix}"
        return 0
    fi

    return 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --write-env-file)
            [[ $# -ge 2 ]] || fail "--write-env-file requires a path"
            OUTPUT_PATH="$2"
            shift 2
            ;;
        --check-only)
            CHECK_ONLY=1
            shift
            ;;
        --quiet)
            QUIET=1
            shift
            ;;
        --allow-degraded-pdf)
            ALLOW_DEGRADED_PDF=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown argument: $1"
            ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || fail "this script currently supports macOS only"

mkdir -p "${STATE_DIR}"
mkdir -p "$(dirname "${OUTPUT_PATH}")"

require_cmd git
require_cmd cmake
require_cmd curl
require_cmd python3
require_cmd xcodebuild

HAS_QMAKE=0
HAS_NIX=0
if command -v qmake >/dev/null 2>&1; then
    HAS_QMAKE=1
fi
if command -v nix >/dev/null 2>&1; then
    HAS_NIX=1
fi

if [[ "${HAS_QMAKE}" -eq 0 && "${HAS_NIX}" -eq 0 ]]; then
    fail "need either qmake/Qt already installed or nix available to hydrate the toolchain"
fi

PKG_CONFIG_BIN="$(command -v pkg-config 2>/dev/null || true)"
if [[ -z "${PKG_CONFIG_BIN}" && "${HAS_NIX}" -eq 1 ]]; then
    PKG_CONFIG_PREFIX="$(nix_build_flake_output "pkgs.pkg-config" "pkg-config")"
    if [[ -x "${PKG_CONFIG_PREFIX}/bin/pkg-config" ]]; then
        PKG_CONFIG_BIN="${PKG_CONFIG_PREFIX}/bin/pkg-config"
    fi
fi

xcodebuild -version >/dev/null 2>&1 || fail "xcodebuild is installed but not usable"
DEVELOPER_DIR="$(xcode-select -p 2>/dev/null || true)"
[[ -n "${DEVELOPER_DIR}" ]] || fail "xcode-select did not return a developer directory"
APPLE_STRIP="$(xcrun -find strip 2>/dev/null || true)"
[[ -x "${APPLE_STRIP}" ]] || fail "could not find Apple strip via xcrun"

QTBASE_PREFIX=""
QTDECLARATIVE_PREFIX=""
QTTOOLS_PREFIX=""
QT_VERSION=""
TOOLCHAIN_SOURCE=""

if [[ "${HAS_QMAKE}" -eq 1 ]]; then
    HOST_QTBASE_PREFIX="$(qmake -query QT_INSTALL_PREFIX 2>/dev/null || true)"
    HOST_QT_VERSION="$(qmake -query QT_VERSION 2>/dev/null || true)"
    if [[ -n "${HOST_QTBASE_PREFIX}" && -n "${HOST_QT_VERSION}" ]]; then
        if mapfile -t HOST_QT_PATHS < <(resolve_host_qt "${HOST_QT_VERSION}" "${HOST_QTBASE_PREFIX}" || true); then
            if [[ "${#HOST_QT_PATHS[@]}" -eq 3 ]]; then
                QTBASE_PREFIX="${HOST_QT_PATHS[0]}"
                QTDECLARATIVE_PREFIX="${HOST_QT_PATHS[1]}"
                QTTOOLS_PREFIX="${HOST_QT_PATHS[2]}"
                QT_VERSION="${HOST_QT_VERSION}"
                TOOLCHAIN_SOURCE="host-qmake"
            fi
        fi
    fi
fi

if [[ -z "${QTBASE_PREFIX}" ]]; then
    [[ "${HAS_NIX}" -eq 1 ]] || fail "unable to resolve Qt from qmake and nix is unavailable"
    log "Hydrating Qt toolchain from the repo flake..."
    QTBASE_PREFIX="$(nix_build_flake_output "pkgs.qt6.qtbase" "qt6-qtbase")"
    QTDECLARATIVE_PREFIX="$(nix_build_flake_output "pkgs.qt6.qtdeclarative" "qt6-qtdeclarative")"
    QTTOOLS_PREFIX="$(nix_build_flake_output "pkgs.qt6.qttools" "qt6-qttools")"
    QT_VERSION="$(python3 - "${QTBASE_PREFIX}" <<'PY'
import os
import sys
target = os.path.realpath(sys.argv[1])
print(os.path.basename(target).split("-qtbase-")[-1])
PY
)"
    TOOLCHAIN_SOURCE="nix-flake"
fi

[[ -e "${QTBASE_PREFIX}/lib/cmake/Qt6/Qt6Config.cmake" ]] \
    || fail "Qt6Config.cmake not found under ${QTBASE_PREFIX}"
[[ -e "${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6Qml/Qt6QmlConfig.cmake" ]] \
    || fail "Qt6QmlConfig.cmake not found under ${QTDECLARATIVE_PREFIX}"
[[ -e "${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6Quick/Qt6QuickConfig.cmake" ]] \
    || fail "Qt6QuickConfig.cmake not found under ${QTDECLARATIVE_PREFIX}"
[[ -e "${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6QuickControls2/Qt6QuickControls2Config.cmake" ]] \
    || fail "Qt6QuickControls2Config.cmake not found under ${QTDECLARATIVE_PREFIX}"
[[ -e "${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6QuickTemplates2/Qt6QuickTemplates2Config.cmake" ]] \
    || fail "Qt6QuickTemplates2Config.cmake not found under ${QTDECLARATIVE_PREFIX}"
[[ -e "${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6QmlTools/Qt6QmlToolsConfig.cmake" ]] \
    || fail "Qt6QmlToolsConfig.cmake not found under ${QTDECLARATIVE_PREFIX}"

QT_PLUGIN_PATH="${QTBASE_PREFIX}/lib/qt-6/plugins"
QML_IMPORT_PATH="${QTDECLARATIVE_PREFIX}/lib/qt-6/qml"
QMLIMPORTSCANNER_PATH="${QTDECLARATIVE_PREFIX}/libexec/qmlimportscanner"
MACDEPLOYQT_PATH="${QTTOOLS_PREFIX}/bin/macdeployqt"

[[ -d "${QT_PLUGIN_PATH}" ]] || fail "Qt plugin directory not found: ${QT_PLUGIN_PATH}"
[[ -d "${QML_IMPORT_PATH}" ]] || fail "Qt QML import directory not found: ${QML_IMPORT_PATH}"
[[ -x "${QMLIMPORTSCANNER_PATH}" ]] || fail "qmlimportscanner not found: ${QMLIMPORTSCANNER_PATH}"
[[ -x "${MACDEPLOYQT_PATH}" ]] || fail "macdeployqt not found: ${MACDEPLOYQT_PATH}"

ONNX_INCLUDE_DIR="${ONNXRuntime_INCLUDE_DIR:-}"
ONNX_LIBRARY="${ONNXRuntime_LIBRARY:-}"

if [[ -z "${ONNX_INCLUDE_DIR}" || ! -e "${ONNX_INCLUDE_DIR}/onnxruntime_cxx_api.h" ]]; then
    ONNX_DEV_PREFIX="$(
        find_store_prefix "/nix/store/*-onnxruntime-*-dev" "include/onnxruntime_cxx_api.h" \
        || true
    )"
    if [[ -n "${ONNX_DEV_PREFIX}" ]]; then
        ONNX_INCLUDE_DIR="${ONNX_DEV_PREFIX}/include"
    fi
fi

if [[ -z "${ONNX_LIBRARY}" || ! -f "${ONNX_LIBRARY}" ]]; then
    ONNX_PREFIX="$(
        find_store_prefix "/nix/store/*-onnxruntime-*" "lib/libonnxruntime.dylib" \
        || true
    )"
    if [[ -n "${ONNX_PREFIX}" ]]; then
        ONNX_LIBRARY="${ONNX_PREFIX}/lib/libonnxruntime.dylib"
    fi
fi

if [[ ( -z "${ONNX_INCLUDE_DIR}" || ! -e "${ONNX_INCLUDE_DIR}/onnxruntime_cxx_api.h" ) \
   || ( -z "${ONNX_LIBRARY}" || ! -f "${ONNX_LIBRARY}" ) ]]; then
    [[ "${HAS_NIX}" -eq 1 ]] || fail "ONNX Runtime not found locally and nix is unavailable"
    log "Hydrating ONNX Runtime from the repo flake..."
    ONNX_DEV_PREFIX="$(nix_build_flake_output "pkgs.onnxruntime.dev" "onnxruntime-dev")"
    ONNX_PREFIX="$(nix_build_flake_output "pkgs.onnxruntime" "onnxruntime")"
    ONNX_INCLUDE_DIR="${ONNX_DEV_PREFIX}/include"
    ONNX_LIBRARY="${ONNX_PREFIX}/lib/libonnxruntime.dylib"
fi

[[ -e "${ONNX_INCLUDE_DIR}/onnxruntime_cxx_api.h" ]] \
    || fail "onnxruntime_cxx_api.h not found under ${ONNX_INCLUDE_DIR}"
[[ -f "${ONNX_LIBRARY}" ]] || fail "libonnxruntime.dylib not found at ${ONNX_LIBRARY}"

if [[ "${ONNX_INCLUDE_DIR}" == /nix/store/*/include ]]; then
    ONNX_DEV_PREFIX="${ONNX_INCLUDE_DIR%/include}"
    ONNX_DEV_PREFIX="$(ensure_gcroot_symlink "onnxruntime-dev" "${ONNX_DEV_PREFIX}")"
    ONNX_INCLUDE_DIR="${ONNX_DEV_PREFIX}/include"
fi

if [[ "${ONNX_LIBRARY}" == /nix/store/*/lib/libonnxruntime.dylib ]]; then
    ONNX_PREFIX="${ONNX_LIBRARY%/lib/libonnxruntime.dylib}"
    ONNX_PREFIX="$(ensure_gcroot_symlink "onnxruntime" "${ONNX_PREFIX}")"
    ONNX_LIBRARY="${ONNX_PREFIX}/lib/libonnxruntime.dylib"
fi

RESOLVED_PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}"
POPPLER_BACKEND="$(detect_poppler_backend "${RESOLVED_PKG_CONFIG_PATH}")"
REQUIRE_POPPLER=1
if [[ "${ALLOW_DEGRADED_PDF}" -eq 1 ]]; then
    REQUIRE_POPPLER=0
fi
BS_REQUIRE_POPPLER_VALUE="OFF"
if [[ "${REQUIRE_POPPLER}" -eq 1 ]]; then
    BS_REQUIRE_POPPLER_VALUE="ON"
fi

if [[ -z "${POPPLER_BACKEND}" && "${HAS_NIX}" -eq 1 && "${REQUIRE_POPPLER}" -eq 1 ]]; then
    POPPLER_PREFIX="$(nix_build_flake_output "pkgs.poppler" "poppler")"
    POPPLER_PKGCONFIG_DIR="${POPPLER_PREFIX}/lib/pkgconfig"
    if [[ -d "${POPPLER_PKGCONFIG_DIR}" ]]; then
        if [[ -n "${RESOLVED_PKG_CONFIG_PATH}" ]]; then
            RESOLVED_PKG_CONFIG_PATH="${POPPLER_PKGCONFIG_DIR}:${RESOLVED_PKG_CONFIG_PATH}"
        else
            RESOLVED_PKG_CONFIG_PATH="${POPPLER_PKGCONFIG_DIR}"
        fi
        POPPLER_BACKEND="$(detect_poppler_backend "${RESOLVED_PKG_CONFIG_PATH}")"
    fi
fi

if [[ "${REQUIRE_POPPLER}" -eq 1 && -z "${POPPLER_BACKEND}" ]]; then
    fail "Poppler backend not detected. Supported macOS/dev flows require PDF extraction capability. Install Poppler so pkg-config can resolve poppler-qt6 or poppler-cpp, or run with --allow-degraded-pdf (or BS_DEV_ALLOW_DEGRADED_PDF=1) only for unsupported local forensics."
fi

PDF_CAPABILITY="available"
if [[ -z "${POPPLER_BACKEND}" ]]; then
    PDF_CAPABILITY="degraded"
fi

MODELS_DIR="${ROOT_DIR}/data/models"
MANIFEST_PATH="${MODELS_DIR}/manifest.json"
[[ -f "${MANIFEST_PATH}" ]] || fail "runtime model manifest is missing: ${MANIFEST_PATH}"
ONLINE_RANKER_BOOTSTRAP_DIR="${MODELS_DIR}/online-ranker-v1/bootstrap"
ONLINE_RANKER_BOOTSTRAP_MODEL_DIR="${ONLINE_RANKER_BOOTSTRAP_DIR}/online_ranker_v1.mlmodelc"
ONLINE_RANKER_BOOTSTRAP_METADATA="${ONLINE_RANKER_BOOTSTRAP_DIR}/metadata.json"

BOOTSTRAP_READY=1
if [[ ! -s "${MODELS_DIR}/vocab.txt" \
   || ! -s "${MODELS_DIR}/bge-small-en-v1.5-int8.onnx" \
   || ! -s "${MODELS_DIR}/bge-large-en-v1.5-f32.onnx" ]]; then
    BOOTSTRAP_READY=0
fi

SEMANTIC_CAPABILITY="ready"
if [[ "${BOOTSTRAP_READY}" -eq 0 ]]; then
    SEMANTIC_CAPABILITY="missing_assets"
fi

OCR_CAPABILITY="missing"
if command -v pkg-config >/dev/null 2>&1 \
   && PKG_CONFIG_PATH="${RESOLVED_PKG_CONFIG_PATH}" pkg-config --exists tesseract; then
    OCR_CAPABILITY="ready"
elif command -v tesseract >/dev/null 2>&1; then
    # CLI-only presence is not sufficient for build/link capability.
    OCR_CAPABILITY="cli_only"
fi

ONLINE_RANKER_BOOTSTRAP_READY=1
if [[ ! -d "${ONLINE_RANKER_BOOTSTRAP_MODEL_DIR}" || ! -s "${ONLINE_RANKER_BOOTSTRAP_METADATA}" ]]; then
    ONLINE_RANKER_BOOTSTRAP_READY=0
fi

ONLINE_RANKER_CAPABILITY="ready"
if [[ "${ONLINE_RANKER_BOOTSTRAP_READY}" -eq 0 ]]; then
    ONLINE_RANKER_CAPABILITY="missing_bootstrap"
fi

if [[ "${CHECK_ONLY}" -eq 0 ]]; then
    PKG_CONFIG_ENV_LINE='export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}"'
    if [[ -n "${RESOLVED_PKG_CONFIG_PATH}" ]]; then
        PKG_CONFIG_ENV_LINE="export PKG_CONFIG_PATH=\"${RESOLVED_PKG_CONFIG_PATH}:\${PKG_CONFIG_PATH:-}\""
    fi

    cat > "${OUTPUT_PATH}" <<EOF
#!/usr/bin/env bash
# Generated by scripts/dev/setup_env.sh for BetterSpotlight local development.
export BETTERSPOTLIGHT_ROOT="${ROOT_DIR}"
export BS_DEV_TOOLCHAIN_SOURCE="${TOOLCHAIN_SOURCE}"
export BS_QT_PREFIX_PATH="${QTBASE_PREFIX};${QTDECLARATIVE_PREFIX};${QTTOOLS_PREFIX}"
export Qt6_DIR="${QTBASE_PREFIX}/lib/cmake/Qt6"
export Qt6Qml_DIR="${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6Qml"
export Qt6Quick_DIR="${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6Quick"
export Qt6QuickControls2_DIR="${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6QuickControls2"
export Qt6QuickTemplates2_DIR="${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6QuickTemplates2"
export Qt6QmlTools_DIR="${QTDECLARATIVE_PREFIX}/lib/cmake/Qt6QmlTools"
export ONNXRuntime_INCLUDE_DIR="${ONNX_INCLUDE_DIR}"
export ONNXRuntime_LIBRARY="${ONNX_LIBRARY}"
export BETTERSPOTLIGHT_MODELS_DIR="${MODELS_DIR}"
export BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR="${ONLINE_RANKER_BOOTSTRAP_DIR}"
export BS_DEV_ONLINE_RANKER_BOOTSTRAP_READY="${ONLINE_RANKER_BOOTSTRAP_READY}"
export BS_DEV_SEMANTIC_CAPABILITY="${SEMANTIC_CAPABILITY}"
export BS_DEV_PDF_CAPABILITY="${PDF_CAPABILITY}"
export BS_DEV_OCR_CAPABILITY="${OCR_CAPABILITY}"
export BS_DEV_ONLINE_RANKER_CAPABILITY="${ONLINE_RANKER_CAPABILITY}"
export BS_DEV_POPPLER_BACKEND="${POPPLER_BACKEND:-none}"
export BS_REQUIRE_POPPLER="${BS_REQUIRE_POPPLER_VALUE}"
export BS_DEV_QMLIMPORTSCANNER="${QMLIMPORTSCANNER_PATH}"
export BS_DEV_MACDEPLOYQT="${MACDEPLOYQT_PATH}"
export CMAKE_PREFIX_PATH="${QTBASE_PREFIX}:${QTDECLARATIVE_PREFIX}:${QTTOOLS_PREFIX}:\${CMAKE_PREFIX_PATH:-}"
${PKG_CONFIG_ENV_LINE}
export QT_PLUGIN_PATH="${QT_PLUGIN_PATH}:\${QT_PLUGIN_PATH:-}"
export QML_IMPORT_PATH="${QML_IMPORT_PATH}:\${QML_IMPORT_PATH:-}"
export QML2_IMPORT_PATH="${QML_IMPORT_PATH}:\${QML2_IMPORT_PATH:-}"
    export PATH="$(dirname "${APPLE_STRIP}"):${QTBASE_PREFIX}/bin:${QTDECLARATIVE_PREFIX}/libexec${PKG_CONFIG_BIN:+:$(dirname "${PKG_CONFIG_BIN}")}:\${PATH}"
EOF
    chmod +x "${OUTPUT_PATH}"
fi

log "Validated BetterSpotlight development environment."
log "Toolchain source: ${TOOLCHAIN_SOURCE} (Qt ${QT_VERSION})"
log "Capability matrix: semantic=${SEMANTIC_CAPABILITY} pdf=${PDF_CAPABILITY} ocr=${OCR_CAPABILITY} online_ranker=${ONLINE_RANKER_CAPABILITY}"
if [[ -n "${POPPLER_BACKEND}" ]]; then
    log "PDF extraction backend: ${POPPLER_BACKEND}"
else
    log "PDF extraction backend: unavailable (degraded profile)"
fi
if [[ "${ONLINE_RANKER_BOOTSTRAP_READY}" -eq 1 ]]; then
    log "CoreML online-ranker bootstrap: ready"
else
    log "CoreML online-ranker bootstrap: missing (${ONLINE_RANKER_BOOTSTRAP_DIR})"
fi
if [[ "${CHECK_ONLY}" -eq 0 ]]; then
    log "Env file: ${OUTPUT_PATH}"
fi
if [[ "${BOOTSTRAP_READY}" -eq 0 ]]; then
    log "Bootstrap models are not fully present under ${MODELS_DIR}; build+launch can fetch them."
fi
