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
bs_add_test(test-query-ranking      Unit/test_query_ranking.cpp)
bs_add_test(test-extraction-office-pdf Unit/test_extraction_office_pdf.cpp)

# ── Integration tests ────────────────────────────────────────────
bs_add_test(test-full-pipeline      Integration/test_full_pipeline.cpp)
bs_add_test(test-index-persistence  Integration/test_index_persistence.cpp)
bs_add_test(test-incremental-update Integration/test_incremental_update.cpp)
bs_add_test(test-crash-isolation    Integration/test_crash_isolation.cpp)
bs_add_test(test-index-backpressure Integration/test_index_backpressure.cpp)

# ── M2 Unit tests ────────────────────────────────────────────────
bs_add_test(test-tokenizer           Unit/test_tokenizer.cpp)
bs_add_test(test-embedding           Unit/test_embedding.cpp)
bs_add_test(test-quantizer           Unit/test_quantizer.cpp)
bs_add_test(test-vector-index        Unit/test_vector_index.cpp)
bs_add_test(test-search-merger       Unit/test_search_merger.cpp)
bs_add_test(test-interaction-tracker Unit/test_interaction_tracker.cpp)
bs_add_test(test-feedback-aggregator Unit/test_feedback_aggregator.cpp)
bs_add_test(test-path-preferences    Unit/test_path_preferences.cpp)
bs_add_test(test-type-affinity       Unit/test_type_affinity.cpp)

# ── M2 Integration tests ────────────────────────────────────────
bs_add_test(test-semantic-search     Integration/test_semantic_search.cpp)
bs_add_test(test-embedding-fallback  Integration/test_embedding_fallback.cpp)
bs_add_test(test-boost-verification  Integration/test_boost_verification.cpp)
bs_add_test(test-context-boost       Integration/test_context_boost.cpp)
bs_add_test(test-embedding-recovery  Integration/test_embedding_recovery.cpp)

# ── Relevance gate tests ────────────────────────────────────────
bs_add_test(test-ui-sim-query-suite Integration/test_ui_sim_query_suite.cpp)
target_compile_definitions(test-ui-sim-query-suite PRIVATE
    BS_RELEVANCE_SUITE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/relevance/ui_sim_query_suite.json"
)
