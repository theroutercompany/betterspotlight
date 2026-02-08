add_executable(betterspotlight-query
    main.cpp
    query_service.cpp
    query_service_m2.cpp
)

target_compile_definitions(betterspotlight-query PRIVATE
    BETTERSPOTLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(betterspotlight-query PRIVATE
    betterspotlight-core
    Qt6::Core
    Qt6::Network
)

if(TARGET betterspotlight-fetch-models)
    add_dependencies(betterspotlight-query betterspotlight-fetch-models)
endif()

add_custom_command(TARGET betterspotlight-query POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DBS_MODEL_SOURCE_DIR=${CMAKE_SOURCE_DIR}/data/models
        -DBS_MODEL_DEST_DIR=$<TARGET_FILE_DIR:betterspotlight-query>/../Resources/models
        -P ${CMAKE_SOURCE_DIR}/cmake/sync_embedding_models.cmake
    VERBATIM
)
