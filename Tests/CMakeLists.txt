find_package(Qt6 REQUIRED COMPONENTS Test)

# Helper macro to create a test executable.
# Each test file gets its own executable and CTest registration.
macro(bs_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        betterspotlight-core
        sqlite3
        Qt6::Core
        Qt6::Test
        Qt6::Network
    )
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# ── Unit tests ────────────────────────────────────────────────────
bs_add_test(test-sqlite-store       Unit/test_sqlite_store.cpp)
bs_add_test(test-path-rules         Unit/test_path_rules.cpp)
bs_add_test(test-scoring            Unit/test_scoring.cpp)
bs_add_test(test-chunker            Unit/test_chunker.cpp)
bs_add_test(test-bsignore-parser    Unit/test_bsignore_parser.cpp)
bs_add_test(test-ipc-messages       Unit/test_ipc_messages.cpp)
bs_add_test(test-match-classifier   Unit/test_match_classifier.cpp)
bs_add_test(test-corrupt-files     Unit/test_corrupt_files.cpp)

# ── Integration tests ────────────────────────────────────────────
bs_add_test(test-full-pipeline      Integration/test_full_pipeline.cpp)
bs_add_test(test-index-persistence  Integration/test_index_persistence.cpp)
bs_add_test(test-incremental-update Integration/test_incremental_update.cpp)
bs_add_test(test-crash-isolation    Integration/test_crash_isolation.cpp)
