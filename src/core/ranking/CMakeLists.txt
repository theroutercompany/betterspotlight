add_library(betterspotlight-core-ranking STATIC
    match_classifier.cpp
    context_signals.cpp
    scorer.cpp
    cross_encoder_reranker.cpp
    qa_extractive_model.cpp
    reranker_cascade.cpp
    personalized_ltr.cpp
)

target_include_directories(betterspotlight-core-ranking PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-ranking PUBLIC
    betterspotlight-core-shared
    betterspotlight-core-models
    Qt6::Core
)

if(BETTERSPOTLIGHT_WITH_ONNX)
    target_link_libraries(betterspotlight-core-ranking PUBLIC
        betterspotlight-core-embedding
    )
endif()

if(ONNXRuntime_FOUND)
    target_include_directories(betterspotlight-core-ranking PRIVATE ${ONNXRuntime_INCLUDE_DIRS})
    target_link_libraries(betterspotlight-core-ranking PUBLIC ONNXRuntime::ONNXRuntime)
    target_compile_definitions(betterspotlight-core-ranking PRIVATE ONNXRUNTIME_FOUND)
endif()
