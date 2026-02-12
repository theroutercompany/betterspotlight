add_executable(betterspotlight-inference
    main.cpp
    inference_service.cpp
    inference_supervisor_actor.cpp
    inference_worker_actor.cpp
)

target_link_libraries(betterspotlight-inference PRIVATE
    betterspotlight-core
    Qt6::Core
    Qt6::Network
)

if(TARGET betterspotlight-fetch-models)
    add_dependencies(betterspotlight-inference betterspotlight-fetch-models)
endif()

add_custom_command(TARGET betterspotlight-inference POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DBS_MODEL_SOURCE_DIR=${CMAKE_SOURCE_DIR}/data/models
        -DBS_MODEL_DEST_DIR=$<TARGET_FILE_DIR:betterspotlight-inference>/../Resources/models
        -P ${CMAKE_SOURCE_DIR}/cmake/sync_embedding_models.cmake
    VERBATIM
)
