#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STATE_DIR="${ROOT_DIR}/.build"
OUTPUT_PATH="${BS_DEV_ENV_FILE:-${STATE_DIR}/dev-env.sh}"
CHECK_ONLY=0
QUIET=0

usage() {
    cat <<'EOF'
Usage: scripts/dev/setup_env.sh [--write-env-file PATH] [--check-only] [--quiet]

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

nix_build_flake_output() {
    local attr="$1"

    nix build --impure --no-link --print-out-paths --expr "
let
  flake = builtins.getFlake \"${ROOT_DIR}\";
  pkgs = import flake.inputs.nixpkgs {
    system = builtins.currentSystem;
    config.allowUnfree = true;
  };
in
  ${attr}
" | tail -n 1
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
    QTBASE_PREFIX="$(nix_build_flake_output "pkgs.qt6.qtbase")"
    QTDECLARATIVE_PREFIX="$(nix_build_flake_output "pkgs.qt6.qtdeclarative")"
    QTTOOLS_PREFIX="$(nix_build_flake_output "pkgs.qt6.qttools")"
    QT_VERSION="$(basename "${QTBASE_PREFIX}" | sed 's/.*-qtbase-//')"
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
    ONNX_DEV_PREFIX="$(nix_build_flake_output "pkgs.onnxruntime.dev")"
    ONNX_PREFIX="$(nix_build_flake_output "pkgs.onnxruntime")"
    ONNX_INCLUDE_DIR="${ONNX_DEV_PREFIX}/include"
    ONNX_LIBRARY="${ONNX_PREFIX}/lib/libonnxruntime.dylib"
fi

[[ -e "${ONNX_INCLUDE_DIR}/onnxruntime_cxx_api.h" ]] \
    || fail "onnxruntime_cxx_api.h not found under ${ONNX_INCLUDE_DIR}"
[[ -f "${ONNX_LIBRARY}" ]] || fail "libonnxruntime.dylib not found at ${ONNX_LIBRARY}"

MODELS_DIR="${ROOT_DIR}/data/models"
MANIFEST_PATH="${MODELS_DIR}/manifest.json"
[[ -f "${MANIFEST_PATH}" ]] || fail "runtime model manifest is missing: ${MANIFEST_PATH}"

BOOTSTRAP_READY=1
if [[ ! -s "${MODELS_DIR}/vocab.txt" || ! -s "${MODELS_DIR}/bge-small-en-v1.5-int8.onnx" ]]; then
    BOOTSTRAP_READY=0
fi

if [[ "${CHECK_ONLY}" -eq 0 ]]; then
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
export BS_DEV_QMLIMPORTSCANNER="${QMLIMPORTSCANNER_PATH}"
export BS_DEV_MACDEPLOYQT="${MACDEPLOYQT_PATH}"
export CMAKE_PREFIX_PATH="${QTBASE_PREFIX}:${QTDECLARATIVE_PREFIX}:${QTTOOLS_PREFIX}:\${CMAKE_PREFIX_PATH:-}"
export QT_PLUGIN_PATH="${QT_PLUGIN_PATH}:\${QT_PLUGIN_PATH:-}"
export QML_IMPORT_PATH="${QML_IMPORT_PATH}:\${QML_IMPORT_PATH:-}"
export QML2_IMPORT_PATH="${QML_IMPORT_PATH}:\${QML2_IMPORT_PATH:-}"
export PATH="$(dirname "${APPLE_STRIP}"):${QTBASE_PREFIX}/bin:${QTDECLARATIVE_PREFIX}/libexec:\${PATH}"
EOF
    chmod +x "${OUTPUT_PATH}"
fi

log "Validated BetterSpotlight development environment."
log "Toolchain source: ${TOOLCHAIN_SOURCE} (Qt ${QT_VERSION})"
if [[ "${CHECK_ONLY}" -eq 0 ]]; then
    log "Env file: ${OUTPUT_PATH}"
fi
if [[ "${BOOTSTRAP_READY}" -eq 0 ]]; then
    log "Bootstrap models are not fully present under ${MODELS_DIR}; build+launch can fetch them."
fi
