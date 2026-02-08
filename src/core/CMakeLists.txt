# Core static library — shared types + index + fs + extraction + ipc + ranking + M2 modules

add_subdirectory(shared)
add_subdirectory(fs)
add_subdirectory(extraction)
add_subdirectory(ipc)
add_subdirectory(ranking)
add_subdirectory(indexing)

# M2 modules (embedding, vector search, feedback)
if(BETTERSPOTLIGHT_WITH_ONNX)
    add_subdirectory(embedding)
    add_subdirectory(vector)
endif()
add_subdirectory(feedback)

add_library(betterspotlight-core STATIC
    index/sqlite_store.cpp
    index/migration.cpp
)

target_include_directories(betterspotlight-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(betterspotlight-core PUBLIC
    betterspotlight-core-shared
    betterspotlight-core-fs
    betterspotlight-core-extraction
    betterspotlight-core-ipc
    betterspotlight-core-ranking
    betterspotlight-core-indexing
    betterspotlight-core-feedback
    sqlite3
    Qt6::Core
)

if(BETTERSPOTLIGHT_WITH_ONNX)
    target_link_libraries(betterspotlight-core PUBLIC
        betterspotlight-core-embedding
        betterspotlight-core-vector
    )
    target_compile_definitions(betterspotlight-core PUBLIC BETTERSPOTLIGHT_WITH_ONNX)
endif()
