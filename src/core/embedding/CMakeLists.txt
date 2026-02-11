# Embedding module — ONNX Runtime inference + tokenizer.
# Note: quantizer.cpp remains for legacy/offline compatibility utilities,
# but online retrieval paths now persist float32 embeddings.

add_library(betterspotlight-core-embedding STATIC
    tokenizer.cpp
    embedding_manager.cpp
    quantizer.cpp
    embedding_pipeline.cpp
)

target_include_directories(betterspotlight-core-embedding PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-embedding PUBLIC
    betterspotlight-core-shared
    Qt6::Core
)

if(ONNXRuntime_FOUND)
    target_include_directories(betterspotlight-core-embedding PRIVATE ${ONNXRuntime_INCLUDE_DIRS})
    target_link_libraries(betterspotlight-core-embedding PUBLIC ONNXRuntime::ONNXRuntime)
    target_compile_definitions(betterspotlight-core-embedding PRIVATE ONNXRUNTIME_FOUND)
endif()
