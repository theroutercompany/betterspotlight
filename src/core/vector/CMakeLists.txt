# M2: Vector search module — hnswlib wrapper, vector store, search merger

add_library(betterspotlight-core-vector STATIC
    vector_index.cpp
    vector_store.cpp
    search_merger.cpp
)

target_include_directories(betterspotlight-core-vector PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_SOURCE_DIR}/src/vendor  # hnswlib headers
)

target_link_libraries(betterspotlight-core-vector PUBLIC
    betterspotlight-core-shared
    betterspotlight-core-ranking
    sqlite3
    Qt6::Core
)

target_compile_options(betterspotlight-core-vector PRIVATE
    -Wno-unused-parameter
)
