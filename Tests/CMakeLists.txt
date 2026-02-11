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
bs_add_test(test-sqlite-store-joined Unit/test_sqlite_store_joined.cpp)
bs_add_test(test-path-rules         Unit/test_path_rules.cpp)
bs_add_test(test-scoring            Unit/test_scoring.cpp)
bs_add_test(test-chunker            Unit/test_chunker.cpp)
bs_add_test(test-bsignore-parser    Unit/test_bsignore_parser.cpp)
bs_add_test(test-ipc-messages       Unit/test_ipc_messages.cpp)
bs_add_test(test-match-classifier   Unit/test_match_classifier.cpp)
bs_add_test(test-corrupt-files     Unit/test_corrupt_files.cpp)
bs_add_test(test-query-ranking      Unit/test_query_ranking.cpp)
bs_add_test(test-extraction-office-pdf Unit/test_extraction_office_pdf.cpp)
bs_add_test(test-extraction-extension-fallback Unit/test_extraction_extension_fallback.cpp)
bs_add_test(test-docs-memo-location Unit/test_docs_memo_location.cpp)
target_compile_definitions(test-docs-memo-location PRIVATE
    BETTERSPOTLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# ── Integration tests ────────────────────────────────────────────
bs_add_test(test-full-pipeline      Integration/test_full_pipeline.cpp)
bs_add_test(test-index-persistence  Integration/test_index_persistence.cpp)
bs_add_test(test-incremental-update Integration/test_incremental_update.cpp)
bs_add_test(test-crash-isolation    Integration/test_crash_isolation.cpp)
bs_add_test(test-index-backpressure Integration/test_index_backpressure.cpp)
bs_add_test(test-query-service-core-improvements Integration/test_query_service_core_improvements.cpp)
bs_add_test(test-query-service-relevance-fixture Integration/test_query_service_relevance_fixture.cpp)
target_compile_definitions(test-query-service-relevance-fixture PRIVATE
    BS_RELEVANCE_BASELINES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/relevance/baselines.json"
)
set_tests_properties(test-query-service-relevance-fixture PROPERTIES
    ENVIRONMENT "BS_RELEVANCE_FIXTURE_REPORT_PATH=/tmp/bs_query_service_relevance_fixture_report.json"
)
#
# Deterministic relevance gate promotion policy:
# - Transitional gatePassRate=90.0 while the 100-case corpus is stabilized.
# - Promote to gatePassRate=95.0 in tests/relevance/baselines.json only
#   after 3 consecutive clean runs of test-query-service-relevance-fixture.
#

add_executable(test-app-lifecycle-states
    Integration/test_app_lifecycle_states.cpp
    ${CMAKE_SOURCE_DIR}/src/app/service_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/app/onboarding_controller.cpp
)
target_link_libraries(test-app-lifecycle-states PRIVATE
    betterspotlight-core
    sqlite3
    Qt6::Core
    Qt6::Test
    Qt6::Network
)
target_compile_options(test-app-lifecycle-states PRIVATE -Wno-keyword-macro)
add_test(NAME test-app-lifecycle-states COMMAND test-app-lifecycle-states)

add_executable(test-settings-controller-platform
    Unit/test_settings_controller_platform.cpp
    ${CMAKE_SOURCE_DIR}/src/app/settings_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/app/platform_integration.cpp
)
target_link_libraries(test-settings-controller-platform PRIVATE
    betterspotlight-core
    sqlite3
    Qt6::Core
    Qt6::Test
    Qt6::Network
)
target_compile_options(test-settings-controller-platform PRIVATE -Wno-keyword-macro)
add_test(NAME test-settings-controller-platform COMMAND test-settings-controller-platform)

# ── M3 Query understanding tests ───────────────────────────────
bs_add_test(test-temporal-parser     Unit/test_temporal_parser.cpp)
bs_add_test(test-entity-extractor    Unit/test_entity_extractor.cpp)
bs_add_test(test-doctype-classifier  Unit/test_doctype_classifier.cpp)
bs_add_test(test-rules-engine        Unit/test_rules_engine.cpp)

# ── M3 Unit tests ────────────────────────────────────────────────
bs_add_test(test-model-registry    Unit/test_model_registry.cpp)

# ── Wave 3: Stability & reliability tests ─────────────────────────
bs_add_test(test-sqlite-store-reliability     Unit/test_sqlite_store_reliability.cpp)
bs_add_test(test-ipc-reliability              Unit/test_ipc_reliability.cpp)
bs_add_test(test-embedding-circuit-breaker    Unit/test_embedding_circuit_breaker.cpp)
bs_add_test(test-extraction-timeout           Unit/test_extraction_timeout.cpp)

# ── Wave 4: Relevance push tests ─────────────────────────────────
bs_add_test(test-score-transparency      Unit/test_score_transparency.cpp)
bs_add_test(test-typo-recall             Unit/test_typo_recall.cpp)
bs_add_test(test-semantic-adaptive       Unit/test_semantic_adaptive.cpp)
bs_add_test(test-query-cache             Unit/test_query_cache.cpp)

# ── Wave 2: Cross-encoder + StructuredQuery signal tests ─────────
bs_add_test(test-tokenizer-pair              Unit/test_tokenizer_pair.cpp)
bs_add_test(test-cross-encoder-reranker      Unit/test_cross_encoder_reranker.cpp)
bs_add_test(test-structured-query-boost      Unit/test_structured_query_boost.cpp)

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
set_tests_properties(test-ui-sim-query-suite PROPERTIES
    ENVIRONMENT "BS_RELEVANCE_GATE_MODE=report_only;BS_RELEVANCE_REPORT_PATH=/tmp/bs_ui_sim_query_suite_report.json"
)
add_test(NAME test-ui-sim-query-suite-stress COMMAND test-ui-sim-query-suite)
set_tests_properties(test-ui-sim-query-suite-stress PROPERTIES
    ENVIRONMENT "BS_RELEVANCE_GATE_MODE=report_only;BS_RELEVANCE_REPORT_PATH=/tmp/bs_ui_sim_query_stress_report.json"
)
