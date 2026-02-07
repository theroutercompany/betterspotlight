add_library(betterspotlight-core-shared STATIC
    types.cpp
    chunk.cpp
    search_result.cpp
    logging.cpp
)

target_include_directories(betterspotlight-core-shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-shared PUBLIC
    Qt6::Core
)
