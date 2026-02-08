cmake_minimum_required(VERSION 3.25)
project(BetterSpotlight VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

find_package(Tesseract QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(POPPLER QUIET poppler-qt6)
endif()

if(BETTERSPOTLIGHT_WITH_ONNX)
    find_package(ONNXRuntime QUIET)
endif()

# ---------- Subdirectories ----------
add_subdirectory(src/core)
add_subdirectory(src/services/indexer)
add_subdirectory(src/services/extractor)
add_subdirectory(src/services/query)
add_subdirectory(src/app)

enable_testing()
add_subdirectory(tests)
