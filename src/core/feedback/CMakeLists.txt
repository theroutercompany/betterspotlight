# M2: Feedback module — interaction tracking, path preferences, type affinity, aggregation

add_library(betterspotlight-core-feedback STATIC
    interaction_tracker.cpp
    feedback_aggregator.cpp
    path_preferences.cpp
    type_affinity.cpp
)

target_include_directories(betterspotlight-core-feedback PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-feedback PUBLIC
    betterspotlight-core-shared
    sqlite3
    Qt6::Core
)
