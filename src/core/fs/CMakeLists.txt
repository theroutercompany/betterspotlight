add_library(betterspotlight-core-fs STATIC
    file_monitor_macos.cpp
    path_rules.cpp
    file_scanner.cpp
    bsignore_parser.cpp
)

target_include_directories(betterspotlight-core-fs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-fs PUBLIC
    betterspotlight-core-shared
    Qt6::Core
    "-framework CoreServices"
)
