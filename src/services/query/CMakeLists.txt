add_executable(betterspotlight-query
    main.cpp
    query_service.cpp
    query_service_m2.cpp
)

target_link_libraries(betterspotlight-query PRIVATE
    betterspotlight-core
    Qt6::Core
    Qt6::Network
)
