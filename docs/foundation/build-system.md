# Build System & Packaging Specification

**Document Status:** Active | **Last Updated:** 2026-02-06 | **Target:** macOS 14+ with Qt 6.10+

---

## Table of Contents

1. [Project Structure](#project-structure)
2. [CMake Configuration](#cmake-configuration)
3. [Build Configurations](#build-configurations)
4. [macOS-Specific Build Steps](#macos-specific-build-steps)
5. [Dependency Management Strategy](#dependency-management-strategy)
6. [Packaging & Distribution](#packaging--distribution)
7. [CI/CD Considerations](#cicd-considerations)
8. [Developer Setup](#developer-setup)
9. [Troubleshooting](#troubleshooting)

---

## Project Structure

The BetterSpotlight codebase is organized to support modular development, clear separation of concerns, and independent service binaries. This structure enables parallel development and testability.

```
betterspotlight/
├── CMakeLists.txt                      # Top-level orchestration
├── cmake/
│   ├── FindTesseract.cmake             # Custom Find module for Tesseract
│   ├── FindONNXRuntime.cmake           # Custom Find module for ONNX Runtime
│   └── Toolchain.cmake                 # macOS universal binary toolchain
├── src/
│   ├── app/                            # Qt UI application
│   │   ├── CMakeLists.txt
│   │   ├── main.cpp
│   │   ├── qml/
│   │   │   ├── Main.qml
│   │   │   ├── SearchView.qml
│   │   │   ├── ResultsList.qml
│   │   │   └── SettingsPanel.qml
│   │   └── resources/
│   │       ├── images/
│   │       ├── icons/
│   │       └── qml.qrc
│   ├── core/                           # Shared C++ library
│   │   ├── CMakeLists.txt
│   │   ├── extraction/
│   │   │   ├── CMakeLists.txt
│   │   │   ├── file_extractor.h
│   │   │   ├── text_extractor.cpp
│   │   │   ├── pdf_extractor.cpp
│   │   │   └── image_extractor.cpp
│   │   ├── fs/
│   │   │   ├── CMakeLists.txt
│   │   │   ├── file_monitor.h
│   │   │   ├── file_monitor_macos.cpp
│   │   │   └── file_scanner.cpp
│   │   ├── index/
│   │   │   ├── CMakeLists.txt
│   │   │   ├── index_manager.h
│   │   │   ├── index_manager.cpp
│   │   │   └── schema.sql
│   │   ├── ranking/
│   │   │   ├── CMakeLists.txt
│   │   │   ├── ranker.h
│   │   │   ├── ml_ranker.cpp
│   │   │   └── bm25_ranker.cpp
│   │   └── shared/
│   │       ├── CMakeLists.txt
│   │       ├── models/
│   │       │   ├── file_metadata.h
│   │       │   ├── search_result.h
│   │       │   └── index_status.h
│   │       ├── ipc/
│   │       │   ├── query_protocol.h
│   │       │   └── indexer_protocol.h
│   │       └── utils/
│   │           ├── logging.h
│   │           ├── path_utils.h
│   │           └── config_manager.h
│   ├── services/
│   │   ├── indexer/                    # Background indexing service
│   │   │   ├── CMakeLists.txt
│   │   │   └── main.cpp
│   │   ├── extractor/                  # Text extraction service
│   │   │   ├── CMakeLists.txt
│   │   │   └── main.cpp
│   │   └── query/                      # Query execution service
│   │       ├── CMakeLists.txt
│   │       └── main.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/
│   │   ├── test_extractor.cpp
│   │   ├── test_indexer.cpp
│   │   ├── test_file_monitor.cpp
│   │   └── test_ranker.cpp
│   ├── integration/
│   │   └── test_full_search_flow.cpp
│   └── fixtures/
│       └── sample_documents/
├── data/
│   ├── default-bsignore.txt             # Default patterns, copied to ~/.bsignore on first run
│   └── tessdata/
│       └── eng.traineddata             # English OCR model (~30 MB)
├── packaging/
│   ├── macos/
│   │   ├── Info.plist
│   │   ├── entitlements.plist
│   │   ├── dmg-background.png
│   │   └── build-dmg.sh
│   └── resources/
│       └── AppIcon.icns
├── .github/
│   └── workflows/
│       ├── build.yml
│       └── release.yml
├── README.md
├── CONTRIBUTING.md
└── docs/

```

### Directory Purposes

- **cmake/**: Custom CMake Find modules and toolchain files for macOS-specific configuration.
- **src/app/**: Qt 6 QML UI + C++ backend. Contains `main.cpp` (Qt entry point), QML views, and resource files.
- **src/core/**: Core C++ library (static) shared by all services. Handles file extraction, indexing, ranking, filesystem monitoring.
- **src/services/**: Standalone service binaries (indexer, extractor, query) that may run independently or in-process.
- **tests/**: Unit and integration tests. Use Qt Test framework for C++ tests.
- **data/**: Data files shipped with the app (exclusion patterns, OCR models, etc.).
- **packaging/macos/**: macOS-specific files for code signing, notarization, DMG creation.

---

## CMake Configuration

### Top-Level CMakeLists.txt

The top-level CMakeLists.txt orchestrates the build, defines global settings, and includes subdirectories.

```cmake
cmake_minimum_required(VERSION 3.21)
project(BetterSpotlight VERSION 1.0.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Qt Configuration
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    Sql
    Network
    Concurrent
)

# Compiler Flags
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")  # Universal binary
endif()

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Build Configuration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add subdirectories
add_subdirectory(src/core)
add_subdirectory(src/app)
add_subdirectory(src/services/indexer)
add_subdirectory(src/services/extractor)
add_subdirectory(src/services/query)
add_subdirectory(tests)

# Summary
message(STATUS "BetterSpotlight ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
if(APPLE)
    message(STATUS "macOS Architectures: ${CMAKE_OSX_ARCHITECTURES}")
endif()
```

### Core Library CMakeLists.txt

The core library is a static library containing shared functionality.

```cmake
# src/core/CMakeLists.txt
add_library(betterspotlight-core STATIC)

# Source files from subdirectories
add_subdirectory(extraction)
add_subdirectory(fs)
add_subdirectory(index)
add_subdirectory(ranking)
add_subdirectory(shared)

# Link against Qt and system dependencies
target_link_libraries(betterspotlight-core PUBLIC
    Qt6::Core
    Qt6::Sql
    Qt6::Concurrent
)

# macOS frameworks
if(APPLE)
    target_link_libraries(betterspotlight-core PRIVATE
        "-framework CoreServices"
        "-framework Security"
    )
endif()

# Include directories
target_include_directories(betterspotlight-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Compiler definitions
target_compile_definitions(betterspotlight-core PRIVATE
    BETTERSPOTLIGHT_CORE_LIBRARY
)
```

### Dependency Configuration Examples

#### SQLite (Bundled/Vendored)

SQLite should be vendored in the source tree and compiled with FTS5 enabled. The system SQLite may not have FTS5 support.

```cmake
# src/core/index/CMakeLists.txt (simplified)
add_library(sqlite3 STATIC)

target_sources(sqlite3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/sqlite/sqlite3.c
)

target_include_directories(sqlite3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/sqlite
)

# Enable FTS5 support
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5=1
    SQLITE_ENABLE_RTREE=1
    SQLITE_ENABLE_EXPLAIN_COMMENTS=1
    SQLITE_ENABLE_UNKNOWN_SQL_FUNCTION=1
)

# Link into core library
target_link_libraries(betterspotlight-core PRIVATE sqlite3)
```

#### Tesseract

Find via pkg-config or custom Find module.

```cmake
# Top-level CMakeLists.txt
find_package(Tesseract REQUIRED)
find_package(Leptonica REQUIRED)

# In src/core/extraction/CMakeLists.txt
target_link_libraries(betterspotlight-core PRIVATE
    Tesseract::Tesseract
    Leptonica::Leptonica
)
```

#### Poppler

Find via pkg-config.

```cmake
# Top-level CMakeLists.txt
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER REQUIRED poppler-cpp)

# In src/core/extraction/CMakeLists.txt
target_include_directories(betterspotlight-core PRIVATE ${POPPLER_INCLUDE_DIRS})
target_link_libraries(betterspotlight-core PRIVATE ${POPPLER_LIBRARIES})
```

#### ONNX Runtime (M2 Only, Optional)

```cmake
# Top-level CMakeLists.txt
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    find_package(ONNXRuntime)
    if(ONNXRuntime_FOUND)
        add_compile_definitions(BETTERSPOTLIGHT_WITH_ONNX=1)
        target_link_libraries(betterspotlight-core PRIVATE
            ONNXRuntime::ONNXRuntime
        )
    endif()
endif()
```

### Main App CMakeLists.txt

```cmake
# src/app/CMakeLists.txt
add_executable(betterspotlight
    main.cpp
    resources/qml.qrc
)

qt_add_qml_module(betterspotlight
    URI BetterSpotlight
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/SearchView.qml
        qml/ResultsList.qml
        qml/SettingsPanel.qml
    RESOURCES
        resources/images/
)

target_link_libraries(betterspotlight PRIVATE
    betterspotlight-core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
)

# macOS app bundle properties
if(APPLE)
    set_target_properties(betterspotlight PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.betterspotlight.app"
        MACOSX_BUNDLE_BUNDLE_NAME "BetterSpotlight"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    )
endif()
```

### Service Binaries

Service binaries are small executables that link against the core library.

```cmake
# src/services/indexer/CMakeLists.txt
add_executable(betterspotlight-indexer main.cpp)
target_link_libraries(betterspotlight-indexer PRIVATE
    betterspotlight-core
    Qt6::Core
    Qt6::Sql
)

# Similar for extractor and query services
```

---

## Build Configurations

BetterSpotlight supports three primary build configurations, each tailored to different development and deployment scenarios.

### Debug

**Use for:** Local development, debugging, testing.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build
```

**Characteristics:**
- Compiler optimization: `-O0` (no optimization)
- Debug symbols: `-g` (included)
- Address Sanitizer (ASAN): enabled to catch memory errors
- Undefined Behavior Sanitizer (UBSAN): enabled to detect undefined behavior
- Assertions: enabled (assert() macros work)
- Binary size: large (~500+ MB)

**CMake configuration:**

```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT MSVC)
        add_compile_options(-g -O0 -fno-omit-frame-pointer)
        add_compile_options(-fsanitize=address -fsanitize=undefined)
        add_link_options(-fsanitize=address -fsanitize=undefined)
    else()
        add_compile_options(/Zi /O0)
    endif()
    add_compile_definitions(BETTERSPOTLIGHT_DEBUG=1)
endif()
```

### Release

**Use for:** Production distribution, DMG packages, end-user binaries.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build
```

**Characteristics:**
- Compiler optimization: `-O2` (fast)
- Link-Time Optimization (LTO): enabled for smaller, faster binary
- Debug symbols: stripped
- Assertions: disabled
- Binary size: small (~20-30 MB for main app)

**CMake configuration:**

```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(NOT MSVC)
        add_compile_options(-O2)
        add_compile_options(-flto)
        add_link_options(-flto)
    else()
        add_compile_options(/O2)
    endif()
    # Strip symbols during install
    install(TARGETS betterspotlight DESTINATION . STRIP)
endif()
```

### RelWithDebInfo

**Use for:** Profiling production builds, debugging release issues.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build
```

**Characteristics:**
- Compiler optimization: `-O2`
- Debug symbols: included (not stripped)
- Size: medium (~100-150 MB)
- Useful for `lldb`, `Instruments.app`, flame graphs

---

## macOS-Specific Build Steps

BetterSpotlight targets macOS 14+ (Sonoma and later) with support for both Apple Silicon (arm64) and Intel (x86_64) processors via universal binaries.

### Universal Binary Configuration

Create a universal binary containing both ARM64 and x86_64 architectures.

```cmake
# Top-level CMakeLists.txt
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
```

**Build commands:**

```bash
# Universal build (arm64 + x86_64)
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)

cmake --build build

# Verify universal binary
lipo -info build/src/app/betterspotlight
# Output: Mach-O universal binary with 2 architectures: [arm64:Mach-O 64-bit executable arm64] [x86_64:Mach-O 64-bit executable x86_64]
```

### macOS Frameworks

Link system frameworks directly in CMakeLists.txt:

```cmake
# src/core/fs/CMakeLists.txt
if(APPLE)
    target_link_libraries(betterspotlight-core PRIVATE
        "-framework CoreServices"      # FSEvents for file monitoring
        "-framework Security"           # For keychain access (optional, future)
    )
endif()
```

**Frameworks used:**

| Framework | Purpose | Required? |
|-----------|---------|-----------|
| CoreServices | FSEvents API for efficient file system monitoring | Yes |
| Security | Keychain integration for storing saved credentials | Optional (Phase 2) |
| Cocoa | Window management, native UI elements | Optional (may use via Qt) |

### App Bundle Structure

After building, CMake automatically creates an `.app` bundle. Verify its structure:

```bash
ls -la build/src/app/betterspotlight.app/Contents/
# MacOS/          (executable)
# Resources/      (QML, icons, data)
# Frameworks/     (Qt, system libraries)
# Helpers/        (service binaries)
# Info.plist
# PkgInfo
```

Embed service binaries in the Helpers directory during install:

```cmake
# src/app/CMakeLists.txt (after add_executable)
if(APPLE)
    # Copy service binaries into Helpers/
    add_custom_command(TARGET betterspotlight POST_BUILD
        COMMAND mkdir -p
            "$<TARGET_BUNDLE_CONTENT_DIR:betterspotlight>/Helpers"
        COMMAND cp
            "${CMAKE_BINARY_DIR}/src/services/indexer/betterspotlight-indexer"
            "$<TARGET_BUNDLE_CONTENT_DIR:betterspotlight>/Helpers/"
        COMMAND cp
            "${CMAKE_BINARY_DIR}/src/services/extractor/betterspotlight-extractor"
            "$<TARGET_BUNDLE_CONTENT_DIR:betterspotlight>/Helpers/"
        COMMAND cp
            "${CMAKE_BINARY_DIR}/src/services/query/betterspotlight-query"
            "$<TARGET_BUNDLE_CONTENT_DIR:betterspotlight>/Helpers/"
    )
endif()
```

### Code Signing

Sign the app bundle for distribution outside the Mac App Store.

```bash
# Code sign the app
codesign --deep --force --verify --verbose \
    --options=runtime \
    --sign "Developer ID Application: Your Team" \
    build/src/app/betterspotlight.app

# Verify signature
codesign --verify --verbose build/src/app/betterspotlight.app
```

**Entitlements file** (packaging/macos/entitlements.plist):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.app-sandbox</key>
    <false/>
    <key>com.apple.security.files.user-selected.read-write</key>
    <true/>
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <true/>
</dict>
</plist>
```

### Notarization

Notarize the app with Apple for distribution outside the Mac App Store (required for Gatekeeper).

```bash
# Create a zip of the signed app
ditto -c -k --sequesterRsrc \
    build/src/app/betterspotlight.app \
    betterspotlight.app.zip

# Submit for notarization
xcrun notarytool submit betterspotlight.app.zip \
    --apple-id "your-apple-id@example.com" \
    --password "your-app-specific-password" \
    --team-id "TEAM123456"

# Wait for approval (typically 5-10 minutes), then staple
xcrun stapler staple build/src/app/betterspotlight.app
```

**Verify notarization:**

```bash
codesign -dv build/src/app/betterspotlight.app
```

---

## Dependency Management Strategy

BetterSpotlight has several external dependencies with different management strategies based on stability and distribution model.

### Qt 6.10+

**Strategy:** Install via official Qt installer or Homebrew.

**Installation:**

```bash
# Via Homebrew (easiest for development)
brew install qt@6

# Verify installation
brew --prefix qt@6
# Output: /usr/local/opt/qt@6
```

**CMake configuration:**

```bash
cmake -B build \
    -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
```

**Release deployment:** Qt libraries are linked into the app bundle via CMake's `qt_deploy_runtime_dependencies`.

```cmake
# In src/app/CMakeLists.txt
qt_generate_deploy_app_cmake_script(
    TARGET betterspotlight
    OUTPUT_SCRIPT deploy_script
    NO_COMPILER_RUNTIME
)
install(SCRIPT "${deploy_script}")
```

### SQLite (Vendored)

**Strategy:** Vendored in source tree (`src/vendor/sqlite/sqlite3.c`). Compiled with FTS5 enabled.

**Rationale:**
- System SQLite may lack FTS5 (Full Text Search 5) support.
- Vendoring ensures consistent FTS5 behavior across all builds.
- Single-file amalgamation is easy to maintain and version.

**Installation:**

```bash
# Download SQLite amalgamation
curl -O https://www.sqlite.org/2024/sqlite-amalgamation-3450000.zip
unzip sqlite-amalgamation-3450000.zip
cp sqlite-amalgamation-3450000/sqlite3.{c,h} src/vendor/sqlite/
```

**Compilation:**

CMake automatically builds SQLite with FTS5 enabled (see [CMake Configuration](#sqlite-bundledvendored) section above).

### Tesseract + Leptonica

**Strategy:** Homebrew for development, static vendoring for release builds (TBD).

**Development installation:**

```bash
brew install tesseract leptonica
pkg-config --cflags --libs tesseract leptonica
```

**CMake configuration:**

```cmake
# Top-level CMakeLists.txt
find_package(Tesseract REQUIRED)
find_package(Leptonica REQUIRED)

# src/core/extraction/CMakeLists.txt
target_link_libraries(betterspotlight-core PRIVATE
    Tesseract::Tesseract
    Leptonica::Leptonica
)
```

**Tessdata (trained OCR model):**

The English model (`eng.traineddata`, ~30 MB) is shipped in the app bundle:

```bash
# Download and place in data/tessdata/
mkdir -p data/tessdata
wget https://github.com/UB-Mannheim/tesseract/raw/master/tessdata_best/eng.traineddata
mv eng.traineddata data/tessdata/
```

**Deployment:**

```cmake
# In src/app/CMakeLists.txt
file(COPY data/tessdata
    DESTINATION "${CMAKE_BINARY_DIR}/src/app/betterspotlight.app/Contents/Resources/")
```

**Release note:** Static linking of Tesseract for release builds is under investigation (see ADR-006).

### Poppler / MuPDF

**Strategy:** Homebrew for development. Release strategy TBD (see ADR-006).

**Development installation:**

```bash
brew install poppler
pkg-config --cflags --libs poppler-cpp
```

**CMake configuration:**

```cmake
# Top-level CMakeLists.txt
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER REQUIRED poppler-cpp)

# src/core/extraction/CMakeLists.txt
target_include_directories(betterspotlight-core PRIVATE ${POPPLER_INCLUDE_DIRS})
target_link_libraries(betterspotlight-core PRIVATE ${POPPLER_LIBRARIES})
```

### ONNX Runtime (M2 Only)

**Strategy:** Pre-built framework downloaded from GitHub. Optional, Intel build skips.

**Conditional compilation:**

```cmake
# Top-level CMakeLists.txt
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    find_package(ONNXRuntime)
    if(ONNXRuntime_FOUND)
        add_compile_definitions(BETTERSPOTLIGHT_WITH_ONNX=1)
        target_link_libraries(betterspotlight-core PRIVATE
            ONNXRuntime::ONNXRuntime
        )
    endif()
endif()
```

**Manual download (if find_package fails):**

```bash
# Download pre-built ONNX Runtime for macOS ARM64
wget https://github.com/microsoft/onnxruntime/releases/download/v1.17.0/onnxruntime-osx-arm64-1.17.0.zip
unzip onnxruntime-osx-arm64-1.17.0.zip -d src/vendor/onnxruntime
```

---

## Packaging & Distribution

### DMG Creation

The final deliverable is a macOS Disk Image (.dmg) containing the signed, notarized app and a convenient symlink to `/Applications`.

#### Automated DMG Build

```bash
# packaging/macos/build-dmg.sh
#!/bin/bash

VOLUME_NAME="BetterSpotlight"
DMG_FILE="BetterSpotlight-1.0.0.dmg"
APP_PATH="build/src/app/betterspotlight.app"
BACKGROUND="packaging/macos/dmg-background.png"

# Create temporary directory
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

mkdir -p "$TEMP_DIR/Applications"
cp -r "$APP_PATH" "$TEMP_DIR/"
ln -s /Applications "$TEMP_DIR/Applications"

# Create DMG
hdiutil create -volname "$VOLUME_NAME" \
    -srcfolder "$TEMP_DIR" \
    -ov -format UDZO \
    -imagekey zlib-level=9 \
    "$DMG_FILE"

echo "Created $DMG_FILE"
```

**Run:**

```bash
chmod +x packaging/macos/build-dmg.sh
./packaging/macos/build-dmg.sh
```

#### Using create-dmg Tool (Alternative)

```bash
brew install create-dmg

create-dmg \
    --volname "BetterSpotlight" \
    --volicon packaging/macos/AppIcon.icns \
    --background packaging/macos/dmg-background.png \
    --window-pos 100 100 \
    --window-size 600 400 \
    --icon-size 120 \
    --icon "betterspotlight.app" 100 180 \
    --hide-extension "betterspotlight.app" \
    --app-drop-link 400 180 \
    "BetterSpotlight-1.0.0.dmg" \
    "build/src/app/"
```

### DMG Contents

The DMG contains:

```
BetterSpotlight/
├── betterspotlight.app/      (signed, notarized)
└── Applications -> /Applications (symlink for drag-install)
```

**User installation:** Drag `betterspotlight.app` to Applications folder.

### Code Signing & Notarization

Both are **required** for distribution outside the Mac App Store.

**Complete signing + notarization workflow:**

```bash
#!/bin/bash
# Assumes app is built in build/src/app/betterspotlight.app

DEV_ID="Developer ID Application: Your Name (TEAMID)"

# 1. Code sign the app
echo "Signing application..."
codesign --deep --force --options=runtime \
    --sign "$DEV_ID" \
    --entitlements packaging/macos/entitlements.plist \
    build/src/app/betterspotlight.app

# 2. Create zip for notarization
echo "Creating archive for notarization..."
ditto -c -k --sequesterRsrc \
    build/src/app/betterspotlight.app \
    betterspotlight.app.zip

# 3. Submit for notarization
echo "Submitting to Apple for notarization..."
NOTARY_ID=$(xcrun notarytool submit betterspotlight.app.zip \
    --apple-id "your-id@example.com" \
    --password "app-specific-password" \
    --team-id "TEAMID" \
    --output-format json | jq -r '.id')

echo "Notarization ID: $NOTARY_ID"

# 4. Wait for approval
echo "Waiting for notarization approval (this may take 5-10 minutes)..."
while true; do
    STATUS=$(xcrun notarytool info "$NOTARY_ID" \
        --apple-id "your-id@example.com" \
        --password "app-specific-password" \
        --team-id "TEAMID" \
        --output-format json | jq -r '.status')

    echo "Status: $STATUS"

    if [ "$STATUS" = "Accepted" ]; then
        echo "Notarization approved! Stapling..."
        xcrun stapler staple build/src/app/betterspotlight.app
        break
    elif [ "$STATUS" = "Rejected" ]; then
        echo "Notarization rejected. Check logs."
        xcrun notarytool log "$NOTARY_ID" \
            --apple-id "your-id@example.com" \
            --password "app-specific-password" \
            --team-id "TEAMID"
        exit 1
    fi

    sleep 10
done

# 5. Create final DMG
echo "Creating distribution DMG..."
./packaging/macos/build-dmg.sh

echo "Done! DMG ready at BetterSpotlight-*.dmg"
```

---

## CI/CD Considerations

This section outlines a CI/CD pipeline for automated builds, testing, and distribution. The examples use GitHub Actions, but the concepts apply to other CI systems.

### GitHub Actions Workflow Structure

Create two workflows: one for continuous builds (on every push), one for release builds (on tags).

#### Build & Test Workflow (.github/workflows/build.yml)

```yaml
name: Build & Test

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main ]

jobs:
  build:
    runs-on: macos-latest-xlarge  # M-series runner for arm64 support
    strategy:
      matrix:
        arch: [arm64, x86_64, universal]

    steps:
    - uses: actions/checkout@v4
      with:
        fetch-depth: 0  # Full history for versioning

    - name: Install dependencies
      run: |
        brew install cmake qt@6 tesseract leptonica poppler

    - name: Configure CMake
      run: |
        cmake -B build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)

    - name: Build
      run: cmake --build build

    - name: Run unit tests
      run: ctest --test-dir build --output-on-failure

    - name: Verify universal binary
      if: matrix.arch == 'universal'
      run: lipo -info build/src/app/betterspotlight

    - name: Upload build artifacts
      if: always()
      uses: actions/upload-artifact@v3
      with:
        name: betterspotlight-${{ matrix.arch }}-${{ github.sha }}
        path: build/src/app/betterspotlight.app
        retention-days: 7
```

#### Release Workflow (.github/workflows/release.yml)

```yaml
name: Release

on:
  push:
    tags:
      - 'v*'

jobs:
  release:
    runs-on: macos-latest-xlarge

    steps:
    - uses: actions/checkout@v4

    - name: Install dependencies
      run: brew install cmake qt@6 tesseract leptonica poppler create-dmg

    - name: Build Release
      run: |
        cmake -B build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6) \
          -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
        cmake --build build

    - name: Run tests
      run: ctest --test-dir build

    - name: Code sign
      env:
        DEVELOPER_ID: ${{ secrets.DEVELOPER_ID }}
        KEYCHAIN_PASSWORD: ${{ secrets.KEYCHAIN_PASSWORD }}
      run: |
        # Unlock keychain and sign
        security unlock-keychain -p "$KEYCHAIN_PASSWORD" ~/Library/Keychains/login.keychain-db
        codesign --deep --force --options=runtime \
          --sign "$DEVELOPER_ID" \
          --entitlements packaging/macos/entitlements.plist \
          build/src/app/betterspotlight.app

    - name: Notarize
      env:
        APPLE_ID: ${{ secrets.APPLE_ID }}
        APPLE_PASSWORD: ${{ secrets.APPLE_PASSWORD }}
        TEAM_ID: ${{ secrets.TEAM_ID }}
      run: |
        # Create zip
        ditto -c -k --sequesterRsrc \
          build/src/app/betterspotlight.app \
          betterspotlight.app.zip

        # Submit for notarization
        NOTARY_ID=$(xcrun notarytool submit betterspotlight.app.zip \
          --apple-id "$APPLE_ID" \
          --password "$APPLE_PASSWORD" \
          --team-id "$TEAM_ID" \
          --output-format json | jq -r '.id')

        # Poll until approved
        while true; do
          STATUS=$(xcrun notarytool info "$NOTARY_ID" \
            --apple-id "$APPLE_ID" \
            --password "$APPLE_PASSWORD" \
            --team-id "$TEAM_ID" \
            --output-format json | jq -r '.status')

          if [ "$STATUS" = "Accepted" ]; then
            xcrun stapler staple build/src/app/betterspotlight.app
            break
          elif [ "$STATUS" = "Rejected" ]; then
            echo "Notarization rejected"
            exit 1
          fi
          sleep 10
        done

    - name: Create DMG
      run: ./packaging/macos/build-dmg.sh

    - name: Create Release
      uses: softprops/action-gh-release@v1
      with:
        files: |
          BetterSpotlight-*.dmg
          build/src/app/betterspotlight.app/
      env:
        GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
```

### Local Build Commands

For developers building locally:

```bash
# Build for local testing
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Build release for distribution
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build-release
./packaging/macos/build-dmg.sh
```

---

## Developer Setup

### Quick Start (Under 30 Minutes)

A new team member can go from zero to building in these steps:

#### 1. Install Xcode Command Line Tools

```bash
xcode-select --install
# Follow prompts to complete installation
```

#### 2. Install Homebrew (if not already installed)

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

#### 3. Install CMake & Qt

```bash
brew install cmake qt@6
```

#### 4. Install Optional Dependencies

```bash
brew install tesseract leptonica poppler
```

**Note:** These are optional for initial builds but required for full functionality.

#### 5. Clone Repository & Build

```bash
git clone https://github.com/yourusername/betterspotlight.git
cd betterspotlight

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)

# Build
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Run the app
open build/src/app/betterspotlight.app
```

#### 6. Verify Everything Works

```bash
# Check that the app opens without errors
build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight &

# Check that the core library links correctly
otool -L build/src/core/libbetterspotlight-core.a | grep -i qt
```

### Development Environment Setup (Detailed)

#### IDE Setup

**Xcode:**

```bash
# Generate Xcode project
cmake -G Xcode -B build-xcode -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
open build-xcode/BetterSpotlight.xcodeproj
```

**CLion:**

1. Open CLion
2. File → Open → Select the `betterspotlight` directory
3. CLion will auto-detect CMakeLists.txt and configure the project
4. Set CMake options: `-DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)`

**VS Code:**

Install extensions:
- C/C++ (IntelliSense, debugging)
- CMake Tools
- Qt Tools (optional, for QML syntax highlighting)

Configure `.vscode/settings.json`:

```json
{
    "cmake.sourceDirectory": "${workspaceFolder}",
    "cmake.buildDirectory": "${workspaceFolder}/build",
    "cmake.configureSettings": {
        "CMAKE_PREFIX_PATH": "/usr/local/opt/qt@6"
    },
    "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools"
}
```

#### Debugging with LLDB

```bash
# Build with debug symbols
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build

# Run under lldb
lldb build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight

# In lldb prompt:
(lldb) breakpoint set --file extraction/text_extractor.cpp --line 42
(lldb) run
(lldb) p variable_name  # Print variable
(lldb) continue
```

#### Code Style & Linting

Install clang-format and clang-tidy via Homebrew:

```bash
brew install clang-format clang-tools

# Format a file
clang-format -i src/core/extraction/text_extractor.cpp

# Run static analysis
clang-tidy src/core/extraction/text_extractor.cpp -- -I$(brew --prefix qt@6)/include
```

**Pre-commit hook** (optional, prevents committing badly formatted code):

```bash
# .git/hooks/pre-commit
#!/bin/bash
files=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.(cpp|h)$')
for file in $files; do
    clang-format -i "$file"
    git add "$file"
done
```

#### Running Tests Locally

```bash
# Build tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build

# Run all tests with verbose output
ctest --test-dir build --output-on-failure -V

# Run a specific test
ctest --test-dir build -R "test_extractor" --output-on-failure -V

# Run with ASAN enabled (memory safety)
ASAN_OPTIONS=verbosity=1 ctest --test-dir build --output-on-failure
```

#### Common Development Tasks

**Build and run the app:**

```bash
cmake --build build && open build/src/app/betterspotlight.app
```

**Rebuild the index database:**

```bash
# Reset index
rm -rf ~/.local/share/betterspotlight/index.db

# Re-run app to rebuild
open build/src/app/betterspotlight.app
```

**Check for memory leaks (Debug build only):**

```bash
# Run with AddressSanitizer
ASAN_OPTIONS=detect_leaks=1 build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight
```

**View app bundle structure:**

```bash
tree build/src/app/betterspotlight.app
# Or with file sizes:
du -h -d 2 build/src/app/betterspotlight.app
```

---

## Troubleshooting

### Common Build Issues

#### "Qt6 not found"

**Symptom:**

```
CMake Error at CMakeLists.txt:12 (find_package):
  By not providing "Qt6Config.cmake" in CMAKE_PREFIX_PATH...
```

**Solution:**

```bash
# Ensure Qt is installed
brew install qt@6

# Set CMAKE_PREFIX_PATH correctly
cmake -B build -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
```

#### "Tesseract/Leptonica not found"

**Symptom:**

```
CMake Error: Could not find FindTesseract.cmake
```

**Solution:**

```bash
# Install via Homebrew
brew install tesseract leptonica

# If pkg-config is missing:
brew install pkg-config

# Try reconfiguring
rm -rf build && cmake -B build -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
```

#### "SQLite FTS5 not available"

**Symptom:**

```
SQL Error: near "VIRTUAL": syntax error
```

**Cause:** System SQLite doesn't have FTS5 compiled in.

**Solution:** Ensure vendored SQLite is being used and compiled with `-DSQLITE_ENABLE_FTS5=1`:

```bash
# Check which SQLite is linked
otool -L build/src/core/libbetterspotlight-core.a | grep sqlite

# Should NOT show system sqlite. If it does, check CMakeLists.txt
# and ensure sqlite3 static library is linked before system one.
```

#### "Undefined reference to TestCase::..." (in tests)

**Symptom:**

```
ld: undefined symbol for architecture arm64: _ZN8TestCaseC1Ev
```

**Cause:** Qt Test framework not linked properly.

**Solution:**

```cmake
# In tests/CMakeLists.txt, ensure:
target_link_libraries(betterspotlight-tests PRIVATE Qt6::Test)
```

### Runtime Issues

#### App crashes on startup

**Debug steps:**

```bash
# Run in debugger
lldb build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight
(lldb) run
(lldb) bt  # Print backtrace if crash occurs

# Or check system logs
log stream --predicate 'eventMessage contains[c] "betterspotlight"' --level debug
```

#### Services don't start

**Check that service binaries exist:**

```bash
ls -la build/src/app/betterspotlight.app/Contents/Helpers/
```

**If missing, rebuild and check CMakeLists.txt for custom copy commands.**

#### "No such file or directory" when loading QML

**Symptom:**

```
file:///path/to/betterspotlight.app/.../Main.qml not found
```

**Solution:** Verify QML files are included in `qml.qrc`:

```xml
<!-- src/app/resources/qml.qrc -->
<RCC>
    <qresource prefix="/">
        <file>qml/Main.qml</file>
        <file>qml/SearchView.qml</file>
        <!-- etc -->
    </qresource>
</RCC>
```

### Performance Issues

#### Slow build times

**Workaround:**

- Use Ninja instead of Xcode:

  ```bash
  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
  cmake --build build
  ```

- Enable ccache for incremental builds:

  ```bash
  brew install ccache
  cmake -B build -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache ...
  ```

#### Large binary size

**Check for debug symbols in Release build:**

```bash
# Release build should be ~20-30 MB
du -h build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight

# Strip symbols if needed:
strip -x build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight
```

**Enable LTO (Link Time Optimization) in CMakeLists.txt:**

```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-flto)
    add_link_options(-flto)
endif()
```

### Signing & Notarization Issues

#### "Code signature invalid" after modification

**Cause:** Modifying files after signing invalidates the signature.

**Solution:** Re-sign after any changes:

```bash
codesign --deep --force --options=runtime \
    --sign "Developer ID Application: ..." \
    build/src/app/betterspotlight.app
```

#### Notarization rejected with cryptic error

**Debug:**

```bash
# Fetch detailed log
xcrun notarytool log $NOTARY_ID \
    --apple-id "your-id@example.com" \
    --password "app-password" \
    --team-id "TEAMID"
```

**Common causes:**
- Unsigned or improperly signed executable
- Hardened runtime not enabled (`--options=runtime` flag missing)
- Non-notarized third-party binaries (Tesseract, etc.)

**Fix:** Re-sign all binaries and re-submit.

---

## Appendix: Reference Commands

### Build Commands

```bash
# Debug build (development)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build

# Release build (distribution)
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build-release

# Ninja (faster)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)
cmake --build build
```

### Testing Commands

```bash
# Run all tests
ctest --test-dir build --output-on-failure

# Run specific test
ctest --test-dir build -R "test_name" --output-on-failure -V

# Run with memory sanitizer
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build --output-on-failure
```

### Code Quality

```bash
# Format code
clang-format -i src/core/**/*.cpp src/core/**/*.h

# Static analysis
clang-tidy src/core/extraction/text_extractor.cpp -- -I$(brew --prefix qt@6)/include

# Check for unused variables/functions
clang-tidy src/core/extraction/text_extractor.cpp -checks=readability-function-size,readability-identifier-naming
```

### Distribution Commands

```bash
# Sign app
codesign --deep --force --options=runtime \
    --sign "Developer ID Application: Name (TEAM)" \
    --entitlements packaging/macos/entitlements.plist \
    build/src/app/betterspotlight.app

# Create DMG
./packaging/macos/build-dmg.sh

# Notarize
xcrun notarytool submit BetterSpotlight-*.dmg \
    --apple-id "id@example.com" \
    --password "app-password" \
    --team-id "TEAMID"

# Staple notarization
xcrun stapler staple build/src/app/betterspotlight.app
```

### Verification Commands

```bash
# Verify universal binary
lipo -info build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight

# Verify code signature
codesign -dv build/src/app/betterspotlight.app

# Check linked libraries
otool -L build/src/app/betterspotlight.app/Contents/MacOS/betterspotlight

# Check app bundle structure
tree build/src/app/betterspotlight.app

# Verify notarization
spctl -a -t exec -vvv build/src/app/betterspotlight.app
```

---

## Related Documents

- [Architecture Overview](./architecture-overview.md)
- [Dependency Audit](../operations/dependency-audit.md)
- [Acceptance Criteria](../milestones/acceptance-criteria.md)
- ADR-006: Dependency Packaging for Tesseract & Poppler
- ADR-007: CI/CD Platform Selection

---

**Document Version:** 1.0 | **Last Reviewed:** 2026-02-06 | **Owner:** Build & Release Team
