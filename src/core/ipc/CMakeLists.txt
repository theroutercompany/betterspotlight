add_library(betterspotlight-core-ipc STATIC
    message.cpp
    socket_server.cpp
    socket_client.cpp
    service_base.cpp
    supervisor.cpp
)

target_include_directories(betterspotlight-core-ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-ipc PUBLIC
    betterspotlight-core-shared
    Qt6::Core
    Qt6::Network
)
