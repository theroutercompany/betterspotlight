# M3: Model registry — manifest, session management, tokenizer factory

set(MODELS_SOURCES
    model_manifest.cpp
    model_session.cpp
    model_registry.cpp
)

if(BETTERSPOTLIGHT_WITH_ONNX)
    list(APPEND MODELS_SOURCES tokenizer_factory.cpp)
endif()

add_library(betterspotlight-core-models STATIC ${MODELS_SOURCES})

target_include_directories(betterspotlight-core-models PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-models PUBLIC
    betterspotlight-core-shared
    Qt6::Core
)

if(BETTERSPOTLIGHT_WITH_ONNX)
    target_link_libraries(betterspotlight-core-models PUBLIC
        betterspotlight-core-embedding
    )
    target_compile_definitions(betterspotlight-core-models PUBLIC BETTERSPOTLIGHT_WITH_ONNX)
endif()

if(ONNXRuntime_FOUND)
    target_include_directories(betterspotlight-core-models PRIVATE ${ONNXRuntime_INCLUDE_DIRS})
    target_link_libraries(betterspotlight-core-models PUBLIC ONNXRuntime::ONNXRuntime)
    target_compile_definitions(betterspotlight-core-models PRIVATE ONNXRUNTIME_FOUND)
endif()
