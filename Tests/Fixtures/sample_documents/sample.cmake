# Sample CMake fixture for text extraction testing.
# Build configuration for the planned BetterSpotlight C++ rewrite.

cmake_minimum_required(VERSION 3.24)
project(BetterSpotlight
    VERSION 1.0.0
    DESCRIPTION "Spotlight replacement for technical macOS users"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies via vcpkg
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Sql)
find_package(SQLite3 REQUIRED)
find_package(Poppler REQUIRED COMPONENTS cpp)
find_package(Tesseract REQUIRED)

# Core library
add_library(bs_core STATIC
    src/core/fs/fs_events.cpp
    src/core/fs/file_scanner.cpp
    src/core/fs/path_rules.cpp
    src/core/index/sqlite_store.cpp
    src/core/extraction/extraction_manager.cpp
    src/core/extraction/text_extractor.cpp
    src/core/extraction/pdf_extractor.cpp
    src/core/extraction/ocr_extractor.cpp
    src/core/ranking/scoring.cpp
    src/core/ranking/context_signals.cpp
)

target_link_libraries(bs_core
    PUBLIC Qt6::Core Qt6::Sql SQLite::SQLite3
    PRIVATE Poppler::cpp Tesseract::libtesseract
)

target_include_directories(bs_core PUBLIC src/)

# Application
add_executable(betterspotlight
    src/app/main.cpp
    src/app/search_window.cpp
    src/app/status_bar.cpp
    src/app/onboarding.cpp
)

target_link_libraries(betterspotlight PRIVATE bs_core Qt6::Widgets)

# Tests
enable_testing()
find_package(GTest REQUIRED)

add_executable(bs_tests
    tests/core/test_path_rules.cpp
    tests/core/test_sqlite_store.cpp
    tests/core/test_scoring.cpp
    tests/core/test_text_extractor.cpp
)

target_link_libraries(bs_tests PRIVATE bs_core GTest::gtest_main)
gtest_discover_tests(bs_tests)

# Install
install(TARGETS betterspotlight RUNTIME DESTINATION bin)
