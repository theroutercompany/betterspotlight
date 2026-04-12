cmake_minimum_required(VERSION 3.25)
project(BetterSpotlight VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BETTERSPOTLIGHT_ENABLE_COVERAGE "Enable llvm coverage instrumentation" OFF)

# macOS deployment target
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")

# Module path for custom Find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---------- Compiler Flags ----------
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -flto)
    add_link_options(-flto)
endif()

if(BETTERSPOTLIGHT_ENABLE_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

# ---------- Qt 6 ----------
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Qml Quick QuickControls2)
qt_standard_project_setup()

# ---------- Vendored SQLite (static, with FTS5 + JSON1) ----------
add_library(sqlite3 STATIC src/vendor/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC src/vendor/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5=1
    SQLITE_ENABLE_JSON1=1
    SQLITE_ENABLE_COLUMN_METADATA=1
    SQLITE_THREADSAFE=2
    SQLITE_DQS=0
)
# SQLite is C; suppress C++ warnings for vendored code
target_compile_options(sqlite3 PRIVATE -w)

# ---------- Optional Dependencies ----------
option(BETTERSPOTLIGHT_WITH_ONNX "Enable ONNX Runtime for semantic search" ON)
if(APPLE)
    option(BETTERSPOTLIGHT_REQUIRE_POPPLER
           "Require Poppler PDF extraction backend in supported macOS/dev builds"
           ON)
else()
    option(BETTERSPOTLIGHT_REQUIRE_POPPLER
           "Require Poppler PDF extraction backend in supported macOS/dev builds"
           OFF)
endif()
option(BETTERSPOTLIGHT_ENABLE_SPARKLE "Enable Sparkle update framework integration" OFF)
option(BETTERSPOTLIGHT_FETCH_MODELS
       "Fetch embedding model assets into data/models when missing"
       ON)
option(BETTERSPOTLIGHT_FETCH_MAX_QUALITY_MODEL
       "Fetch the 1024d max-quality embedding model as part of model asset bootstrap"
       ON)
option(BETTERSPOTLIGHT_GENERATE_ONLINE_RANKER_BOOTSTRAP
       "Generate CoreML online-ranker bootstrap artifacts (requires coremltools + xcrun)"
       OFF)
if(APPLE)
    option(BETTERSPOTLIGHT_PREFER_COREML
           "Prefer CoreML execution provider on Apple Silicon when available"
           ON)
else()
    option(BETTERSPOTLIGHT_PREFER_COREML
           "Prefer CoreML execution provider on Apple Silicon when available"
           OFF)
endif()

set(BETTERSPOTLIGHT_MODELS_DIR "${CMAKE_SOURCE_DIR}/data/models")
set(BETTERSPOTLIGHT_MODEL_ONNX "${BETTERSPOTLIGHT_MODELS_DIR}/bge-small-en-v1.5-int8.onnx")
set(BETTERSPOTLIGHT_MODEL_ONNX_MAX_QUALITY "${BETTERSPOTLIGHT_MODELS_DIR}/bge-large-en-v1.5-f32.onnx")
set(BETTERSPOTLIGHT_MODEL_VOCAB "${BETTERSPOTLIGHT_MODELS_DIR}/vocab.txt")

if(BETTERSPOTLIGHT_FETCH_MODELS)
    set(_betterspotlight_model_fetch_args)
    set(_betterspotlight_model_fetch_byproducts
        ${BETTERSPOTLIGHT_MODEL_ONNX}
        ${BETTERSPOTLIGHT_MODEL_VOCAB}
    )
    if(NOT BETTERSPOTLIGHT_FETCH_MAX_QUALITY_MODEL)
        list(APPEND _betterspotlight_model_fetch_args --no-max-quality)
    else()
        list(APPEND _betterspotlight_model_fetch_byproducts
            ${BETTERSPOTLIGHT_MODEL_ONNX_MAX_QUALITY}
        )
    endif()

    add_custom_target(betterspotlight-fetch-models
        COMMAND ${CMAKE_SOURCE_DIR}/tools/fetch_embedding_models.sh ${_betterspotlight_model_fetch_args}
        BYPRODUCTS ${_betterspotlight_model_fetch_byproducts}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Ensuring embedding model assets are available in data/models"
        VERBATIM
    )
endif()

add_custom_target(betterspotlight-generate-online-ranker-bootstrap
    COMMAND ${CMAKE_SOURCE_DIR}/tools/generate_online_ranker_coreml.py
            --output-root ${BETTERSPOTLIGHT_MODELS_DIR}/online-ranker-v1/bootstrap
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating CoreML online ranker bootstrap artifacts"
    VERBATIM
)

if(BETTERSPOTLIGHT_GENERATE_ONLINE_RANKER_BOOTSTRAP)
    if(TARGET betterspotlight-fetch-models)
        add_dependencies(betterspotlight-fetch-models
            betterspotlight-generate-online-ranker-bootstrap)
    endif()
endif()

add_custom_target(betterspotlight-benchmark-semantic-shortlist
    COMMAND ${CMAKE_SOURCE_DIR}/tools/benchmark_semantic_shortlist.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Run semantic shortlist benchmark harness"
    VERBATIM
)

find_package(Tesseract QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(POPPLER_QT6 QUIET poppler-qt6)
    if(NOT POPPLER_QT6_FOUND)
        pkg_check_modules(POPPLER_CPP QUIET poppler-cpp)
    endif()
endif()

set(BETTERSPOTLIGHT_POPPLER_BACKEND "none")
if(POPPLER_QT6_FOUND)
    set(BETTERSPOTLIGHT_POPPLER_BACKEND "poppler-qt6")
elseif(POPPLER_CPP_FOUND)
    set(BETTERSPOTLIGHT_POPPLER_BACKEND "poppler-cpp")
endif()

if(BETTERSPOTLIGHT_REQUIRE_POPPLER AND BETTERSPOTLIGHT_POPPLER_BACKEND STREQUAL "none")
    message(FATAL_ERROR
        "Poppler was not detected, but BETTERSPOTLIGHT_REQUIRE_POPPLER=ON.\n"
        "Supported macOS/dev flows require PDF extraction capability.\n"
        "Install Poppler (for example via Homebrew) and ensure pkg-config can find it,\n"
        "or use -DBETTERSPOTLIGHT_REQUIRE_POPPLER=OFF only for unsupported local forensics."
    )
endif()

message(STATUS "BetterSpotlight PDF backend: ${BETTERSPOTLIGHT_POPPLER_BACKEND}")

if(BETTERSPOTLIGHT_WITH_ONNX)
    find_package(ONNXRuntime QUIET)
    if(NOT ONNXRuntime_FOUND)
        message(FATAL_ERROR
            "BETTERSPOTLIGHT_WITH_ONNX=ON but ONNX Runtime was not found.\n"
            "Provide a valid ONNXRuntime toolchain, or configure with "
            "-DBETTERSPOTLIGHT_WITH_ONNX=OFF only for unsupported degraded flows."
        )
    endif()

    if(NOT EXISTS "${ONNXRuntime_INCLUDE_DIR}/onnxruntime_cxx_api.h")
        message(FATAL_ERROR
            "ONNX Runtime include dir is stale or invalid: ${ONNXRuntime_INCLUDE_DIR}\n"
            "Expected header: ${ONNXRuntime_INCLUDE_DIR}/onnxruntime_cxx_api.h"
        )
    endif()

    if(NOT EXISTS "${ONNXRuntime_LIBRARY}")
        message(FATAL_ERROR
            "ONNX Runtime library path is stale or invalid: ${ONNXRuntime_LIBRARY}"
        )
    endif()
endif()

# ---------- Subdirectories ----------
add_subdirectory(src/core)
add_subdirectory(src/services/indexer)
add_subdirectory(src/services/extractor)
add_subdirectory(src/services/query)
add_subdirectory(src/services/inference)
add_subdirectory(src/app)

add_custom_target(betterspotlight-dev-runtime
    DEPENDS
        betterspotlight-sync-runtime-bundle
)

enable_testing()
add_subdirectory(Tests)

if(TARGET betterspotlight-default-verification-tests)
    add_custom_target(betterspotlight-default-verification
        DEPENDS
            betterspotlight-dev-runtime
            betterspotlight-default-verification-tests
    )
endif()
