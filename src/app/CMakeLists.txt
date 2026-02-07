find_package(Qt6 REQUIRED COMPONENTS Qml Quick QuickControls2 Widgets)

qt_add_executable(betterspotlight
    main.cpp
    service_manager.h
    service_manager.cpp
    hotkey_manager.h
    hotkey_manager.cpp
    search_controller.h
    search_controller.cpp
    status_bar_bridge.h
)

# Strip qml/ prefix so types register as "Main" not "qml.Main"
set_source_files_properties(qml/Main.qml          PROPERTIES QT_RESOURCE_ALIAS Main.qml)
set_source_files_properties(qml/SearchPanel.qml    PROPERTIES QT_RESOURCE_ALIAS SearchPanel.qml)
set_source_files_properties(qml/ResultItem.qml     PROPERTIES QT_RESOURCE_ALIAS ResultItem.qml)
set_source_files_properties(qml/SettingsPanel.qml  PROPERTIES QT_RESOURCE_ALIAS SettingsPanel.qml)
set_source_files_properties(qml/StatusBar.qml      PROPERTIES QT_RESOURCE_ALIAS StatusBar.qml)

qt_add_qml_module(betterspotlight
    URI BetterSpotlight
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/SearchPanel.qml
        qml/ResultItem.qml
        qml/SettingsPanel.qml
        qml/StatusBar.qml
)

target_link_libraries(betterspotlight PRIVATE
    betterspotlight-core
    Qt6::Core
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Network
    "-framework Carbon"
)

set_target_properties(betterspotlight PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "BetterSpotlight"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.betterspotlight.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)
