find_package(Qt6 REQUIRED COMPONENTS Test)

set(BS_TEST_REPORT_DIR "${CMAKE_BINARY_DIR}/test-reports")
file(MAKE_DIRECTORY "${BS_TEST_REPORT_DIR}")

add_library(betterspotlight-test-support STATIC
    Support/service_process_harness.cpp
    Support/ipc_test_utils.cpp
)
target_include_directories(betterspotlight-test-support PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/Support
)
target_link_libraries(betterspotlight-test-support PUBLIC
    betterspotlight-core
    sqlite3
    Qt6::Core
    Qt6::Test
    Qt6::Network
)

function(bs_add_test name source)
    set(options)
    set(oneValueArgs TIMEOUT)
    set(multiValueArgs LABELS SOURCES LINK_LIBS ENV COMPILE_DEFINITIONS COMPILE_OPTIONS)
    cmake_parse_arguments(BS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${name}
        ${source}
        ${BS_SOURCES}
    )
    target_link_libraries(${name} PRIVATE
        betterspotlight-core
        betterspotlight-test-support
        sqlite3
        Qt6::Core
        Qt6::Test
        Qt6::Network
        ${BS_LINK_LIBS}
    )
    if(BS_COMPILE_DEFINITIONS)
        target_compile_definitions(${name} PRIVATE ${BS_COMPILE_DEFINITIONS})
    endif()
    if(BS_COMPILE_OPTIONS)
        target_compile_options(${name} PRIVATE ${BS_COMPILE_OPTIONS})
    endif()

    add_test(NAME ${name} COMMAND ${name})

    if(BS_TIMEOUT)
        set(_timeout "${BS_TIMEOUT}")
    else()
        set(_timeout 60)
    endif()
    if(BS_LABELS)
        set(_labels "${BS_LABELS}")
    else()
        set(_labels "unit")
    endif()
    set_tests_properties(${name} PROPERTIES
        TIMEOUT "${_timeout}"
        LABELS "${_labels}"
    )
    if(BS_ENV)
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "${BS_ENV}"
        )
    endif()
endfunction()

function(bs_add_unit_test name source)
    bs_add_test(${name} ${source} LABELS "unit" TIMEOUT 60 ${ARGN})
endfunction()

function(bs_add_integration_test name source)
    bs_add_test(${name} ${source} LABELS "integration" TIMEOUT 180 ${ARGN})
endfunction()

function(bs_add_relevance_test name source)
    bs_add_test(${name} ${source} LABELS "integration;relevance" TIMEOUT 180 ${ARGN})
endfunction()

# Unit tests
bs_add_unit_test(test-sqlite-store Unit/test_sqlite_store.cpp)
bs_add_unit_test(test-sqlite-store-extended Unit/test_sqlite_store_extended.cpp)
bs_add_unit_test(test-sqlite-store-joined Unit/test_sqlite_store_joined.cpp)
bs_add_unit_test(test-path-rules Unit/test_path_rules.cpp)
bs_add_unit_test(test-scoring Unit/test_scoring.cpp)
bs_add_unit_test(test-query-normalizer Unit/test_query_normalizer.cpp)
bs_add_unit_test(test-search-result Unit/test_search_result.cpp)
bs_add_unit_test(test-chunker Unit/test_chunker.cpp)
bs_add_unit_test(test-bsignore-parser Unit/test_bsignore_parser.cpp)
bs_add_unit_test(test-ipc-messages Unit/test_ipc_messages.cpp)
bs_add_unit_test(test-match-classifier Unit/test_match_classifier.cpp)
bs_add_unit_test(test-corrupt-files Unit/test_corrupt_files.cpp)
bs_add_unit_test(test-query-ranking Unit/test_query_ranking.cpp)
bs_add_unit_test(test-pdf-extractor Unit/test_pdf_extractor.cpp
    COMPILE_DEFINITIONS BETTERSPOTLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
bs_add_unit_test(test-ocr-extractor Unit/test_ocr_extractor.cpp
    LINK_LIBS Qt6::Gui
)
bs_add_unit_test(test-extraction-office-pdf Unit/test_extraction_office_pdf.cpp)
bs_add_unit_test(test-extraction-extension-fallback Unit/test_extraction_extension_fallback.cpp)

# M3 query understanding tests
bs_add_unit_test(test-temporal-parser Unit/test_temporal_parser.cpp)
bs_add_unit_test(test-entity-extractor Unit/test_entity_extractor.cpp)
bs_add_unit_test(test-doctype-classifier Unit/test_doctype_classifier.cpp)
bs_add_unit_test(test-rules-engine Unit/test_rules_engine.cpp)

# M3 unit tests
bs_add_unit_test(test-model-registry Unit/test_model_registry.cpp
    SOURCES
        Utils/model_fixture_paths.cpp
)
bs_add_unit_test(test-query-router Unit/test_query_router.cpp)
bs_add_unit_test(test-reranker-cascade Unit/test_reranker_cascade.cpp
    SOURCES
        Utils/model_fixture_paths.cpp
)
bs_add_unit_test(test-personalized-ltr Unit/test_personalized_ltr.cpp)
bs_add_unit_test(test-learning-engine Unit/test_learning_engine.cpp)
bs_add_unit_test(test-online-ranker Unit/test_online_ranker.cpp)
bs_add_unit_test(test-dual-index-calibration Unit/test_dual_index_calibration.cpp)

# Stability/reliability tests
bs_add_unit_test(test-sqlite-store-reliability Unit/test_sqlite_store_reliability.cpp)
bs_add_unit_test(test-ipc-reliability Unit/test_ipc_reliability.cpp)
bs_add_unit_test(test-embedding-circuit-breaker Unit/test_embedding_circuit_breaker.cpp)
bs_add_unit_test(test-extraction-timeout Unit/test_extraction_timeout.cpp)

# Relevance push unit tests
bs_add_unit_test(test-score-transparency Unit/test_score_transparency.cpp)
bs_add_unit_test(test-typo-recall Unit/test_typo_recall.cpp)
bs_add_unit_test(test-semantic-adaptive Unit/test_semantic_adaptive.cpp)
bs_add_unit_test(test-query-cache Unit/test_query_cache.cpp)

# Cross-encoder + StructuredQuery tests
bs_add_unit_test(test-tokenizer-pair Unit/test_tokenizer_pair.cpp)
bs_add_unit_test(test-cross-encoder-reranker Unit/test_cross_encoder_reranker.cpp
    SOURCES
        Utils/model_fixture_paths.cpp
)
bs_add_unit_test(test-structured-query-boost Unit/test_structured_query_boost.cpp)

# M2 unit tests
bs_add_unit_test(test-tokenizer Unit/test_tokenizer.cpp)
bs_add_unit_test(test-embedding Unit/test_embedding.cpp)
bs_add_unit_test(test-quantizer Unit/test_quantizer.cpp)
bs_add_unit_test(test-vector-index Unit/test_vector_index.cpp)
bs_add_unit_test(test-search-merger Unit/test_search_merger.cpp)
bs_add_unit_test(test-interaction-tracker Unit/test_interaction_tracker.cpp)
bs_add_unit_test(test-feedback-aggregator Unit/test_feedback_aggregator.cpp)
bs_add_unit_test(test-path-preferences Unit/test_path_preferences.cpp)
bs_add_unit_test(test-type-affinity Unit/test_type_affinity.cpp)

# New hotspot unit tests
bs_add_unit_test(test-indexer Unit/test_indexer.cpp)
bs_add_test(test-pipeline Unit/test_pipeline.cpp
    TIMEOUT 180
    LABELS "unit"
)
bs_add_unit_test(test-service-base Unit/test_service_base.cpp)
bs_add_unit_test(test-supervisor Unit/test_supervisor.cpp
    COMPILE_OPTIONS -Wno-keyword-macro
)
bs_add_test(test-control-plane-actor Unit/test_control_plane_actor.cpp
    TIMEOUT 60
    LABELS "unit"
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/control_plane_actor.cpp
    COMPILE_OPTIONS -Wno-keyword-macro
)
bs_add_test(test-health-aggregator-actor Unit/test_health_aggregator_actor.cpp
    TIMEOUT 60
    LABELS "unit"
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/health_aggregator_actor.cpp
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/health_snapshot_v2.cpp
    COMPILE_OPTIONS -Wno-keyword-macro
)
bs_add_unit_test(test-pipeline-scheduler-actor Unit/test_pipeline_scheduler_actor.cpp)
bs_add_unit_test(test-path-state-actor Unit/test_path_state_actor.cpp)
bs_add_test(test-inference-supervisor-actor Unit/test_inference_supervisor_actor.cpp
    TIMEOUT 60
    LABELS "unit"
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/services/inference/inference_supervisor_actor.cpp
    COMPILE_OPTIONS -Wno-keyword-macro
)
bs_add_unit_test(test-migration Unit/test_migration.cpp)
bs_add_unit_test(test-vector-store Unit/test_vector_store.cpp)
bs_add_unit_test(test-model-session Unit/test_model_session.cpp)
bs_add_unit_test(test-tokenizer-factory Unit/test_tokenizer_factory.cpp)
bs_add_unit_test(test-mdls-text-extractor Unit/test_mdls_text_extractor.cpp)
bs_add_unit_test(test-qa-extractive-model Unit/test_qa_extractive_model.cpp
    SOURCES
        Utils/model_fixture_paths.cpp
)

# Non-runtime docs/lint check
bs_add_test(test-docs-memo-location Unit/test_docs_memo_location.cpp
    TIMEOUT 30
    LABELS "docs_lint"
    COMPILE_DEFINITIONS BETTERSPOTLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# Integration tests
bs_add_integration_test(test-full-pipeline Integration/test_full_pipeline.cpp)
bs_add_integration_test(test-index-persistence Integration/test_index_persistence.cpp)
bs_add_integration_test(test-incremental-update Integration/test_incremental_update.cpp)
bs_add_integration_test(test-crash-isolation Integration/test_crash_isolation.cpp)
bs_add_integration_test(test-index-backpressure Integration/test_index_backpressure.cpp)
bs_add_integration_test(test-query-service-core-improvements Integration/test_query_service_core_improvements.cpp)
bs_add_integration_test(test-query-service-ipc-extensions Integration/test_query_service_ipc_extensions.cpp)
bs_add_integration_test(test-query-service-semantic-offload Integration/test_query_service_semantic_offload.cpp)
bs_add_integration_test(test-semantic-search Integration/test_semantic_search.cpp)
bs_add_integration_test(test-embedding-fallback Integration/test_embedding_fallback.cpp)
bs_add_integration_test(test-boost-verification Integration/test_boost_verification.cpp)
bs_add_integration_test(test-context-boost Integration/test_context_boost.cpp)
bs_add_integration_test(test-embedding-recovery Integration/test_embedding_recovery.cpp)

# New service IPC integration tests
bs_add_test(test-indexer-service-ipc Integration/test_indexer_service_ipc.cpp
    TIMEOUT 180
    LABELS "integration;service_ipc"
)
bs_add_test(test-extractor-service-ipc Integration/test_extractor_service_ipc.cpp
    TIMEOUT 180
    LABELS "integration;service_ipc"
)
bs_add_test(test-inference-service-ipc Integration/test_inference_service_ipc.cpp
    TIMEOUT 180
    LABELS "integration;service_ipc"
)
bs_add_test(test-query-service-m2-ipc Integration/test_query_service_m2_ipc.cpp
    TIMEOUT 180
    LABELS "integration;service_ipc"
)

# Relevance gates (enforced in CI)
bs_add_relevance_test(test-query-service-relevance-fixture Integration/test_query_service_relevance_fixture.cpp
    COMPILE_DEFINITIONS BS_RELEVANCE_BASELINES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/relevance/baselines.json"
    ENV
        BS_RELEVANCE_FIXTURE_REPORT_PATH=${BS_TEST_REPORT_DIR}/query_service_relevance_fixture.json
        BS_RELEVANCE_GATE_MODE=enforce
        BS_RELEVANCE_REQUIRE_SEMANTIC=1
)

bs_add_relevance_test(test-ui-sim-query-suite Integration/test_ui_sim_query_suite.cpp
    COMPILE_DEFINITIONS BS_RELEVANCE_SUITE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/relevance/ui_sim_query_suite.json"
    ENV
        BS_RELEVANCE_GATE_MODE=enforce
        BS_RELEVANCE_REQUIRE_SEMANTIC=1
        BS_RELEVANCE_REPORT_PATH=${BS_TEST_REPORT_DIR}/ui_sim_query_suite.json
)

add_test(NAME test-ui-sim-query-suite-stress COMMAND test-ui-sim-query-suite)
set_tests_properties(test-ui-sim-query-suite-stress PROPERTIES
    TIMEOUT 300
    LABELS "relevance_stress"
    ENVIRONMENT "BS_RELEVANCE_GATE_MODE=report_only;BS_RELEVANCE_REPORT_PATH=${BS_TEST_REPORT_DIR}/ui_sim_query_suite_stress.json"
)

# App-level integration tests that compile app sources into the test binary
bs_add_test(test-app-lifecycle-states Integration/test_app_lifecycle_states.cpp
    TIMEOUT 180
    LABELS "integration"
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/service_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/control_plane_actor.h
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/control_plane_actor.cpp
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/health_aggregator_actor.h
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/health_aggregator_actor.cpp
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/health_snapshot_v2.h
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/health_snapshot_v2.cpp
        ${CMAKE_SOURCE_DIR}/src/app/onboarding_controller.cpp
    COMPILE_OPTIONS -Wno-keyword-macro
)
bs_add_test(test-single-instance-handoff Integration/test_single_instance_handoff.cpp
    TIMEOUT 120
    LABELS "integration"
)
bs_add_test(test-health-consistency-v2 Integration/test_health_consistency_v2.cpp
    TIMEOUT 180
    LABELS "integration"
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/service_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/app/search_controller.cpp
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/control_plane_actor.cpp
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/health_aggregator_actor.cpp
        ${CMAKE_SOURCE_DIR}/src/app/control_plane/health_snapshot_v2.cpp
    LINK_LIBS
        Qt6::Gui
    COMPILE_OPTIONS -Wno-keyword-macro
)
bs_add_test(test-orphan-reconciliation Integration/test_orphan_reconciliation.cpp
    TIMEOUT 120
    LABELS "integration"
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/runtime_environment.cpp
)

bs_add_test(test-settings-controller-platform Unit/test_settings_controller_platform.cpp
    TIMEOUT 60
    LABELS "unit"
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/settings_controller.cpp
        ${CMAKE_SOURCE_DIR}/src/app/platform_integration.cpp
    COMPILE_OPTIONS -Wno-keyword-macro
)
