add_executable(betterspotlight-indexer
    main.cpp
    indexer_service.cpp
)

target_link_libraries(betterspotlight-indexer PRIVATE
    betterspotlight-core
    Qt6::Core
    Qt6::Network
)
