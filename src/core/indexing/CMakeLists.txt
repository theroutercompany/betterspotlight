add_library(betterspotlight-core-indexing STATIC
    work_queue.cpp
    chunker.cpp
    indexer.cpp
    pipeline.cpp
    path_state_actor.cpp
    pipeline_scheduler_actor.cpp
    pipeline_telemetry_actor.cpp
)

target_include_directories(betterspotlight-core-indexing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-indexing PUBLIC
    betterspotlight-core-shared
    betterspotlight-core-fs
    betterspotlight-core-extraction
    sqlite3
    Qt6::Core
)
