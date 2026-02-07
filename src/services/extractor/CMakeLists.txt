add_executable(betterspotlight-extractor
    main.cpp
    extractor_service.cpp
)

target_link_libraries(betterspotlight-extractor PRIVATE
    betterspotlight-core
    Qt6::Core
    Qt6::Network
)
