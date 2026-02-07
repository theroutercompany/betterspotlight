# Core static library — shared types + index + fs + extraction + ipc + ranking

add_subdirectory(shared)
add_subdirectory(fs)
add_subdirectory(extraction)
add_subdirectory(ipc)
add_subdirectory(ranking)
add_subdirectory(indexing)

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
    sqlite3
    Qt6::Core
)
