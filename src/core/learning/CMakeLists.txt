add_library(betterspotlight-core-learning STATIC
    online_ranker.cpp
    learning_engine.cpp
)

if(APPLE)
    target_sources(betterspotlight-core-learning PRIVATE
        coreml_ranker.mm
    )
else()
    target_sources(betterspotlight-core-learning PRIVATE
        coreml_ranker_stub.cpp
    )
endif()

target_include_directories(betterspotlight-core-learning PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-learning PUBLIC
    betterspotlight-core-shared
    betterspotlight-core-feedback
    sqlite3
    Qt6::Core
)

if(APPLE)
    target_link_libraries(betterspotlight-core-learning PUBLIC
        "-framework CoreML"
        "-framework Foundation"
    )
endif()
