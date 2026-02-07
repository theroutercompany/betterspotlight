add_library(betterspotlight-core-ranking STATIC
    match_classifier.cpp
    context_signals.cpp
    scorer.cpp
)

target_include_directories(betterspotlight-core-ranking PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-ranking PUBLIC
    betterspotlight-core-shared
    Qt6::Core
)
