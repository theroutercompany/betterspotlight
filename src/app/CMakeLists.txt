find_package(Qt6 REQUIRED COMPONENTS Qml Quick QuickControls2 Widgets)

qt_add_executable(betterspotlight
    main.cpp
    control_plane/control_plane_types.h
    control_plane/health_snapshot_v2.h
    control_plane/health_snapshot_v2.cpp
    control_plane/control_plane_actor.h
    control_plane/control_plane_actor.cpp
    control_plane/health_aggregator_actor.h
    control_plane/health_aggregator_actor.cpp
    service_manager.h
    service_manager.cpp
    hotkey_manager.h
    hotkey_manager.cpp
    search_controller.h
    search_controller.cpp
    runtime_environment.h
    runtime_environment.cpp
    update_manager.h
    update_manager.mm
    platform_integration.h
    platform_integration.cpp
    platform_integration_mac.mm
    status_bar_bridge.h
    onboarding_controller.h
    onboarding_controller.cpp
    settings_controller.h
    settings_controller.cpp
    assets/icons.qrc
)

set(APP_ICON_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/BetterSpotlight.icns")
set_source_files_properties(${APP_ICON_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
target_sources(betterspotlight PRIVATE ${APP_ICON_ICNS})

# Strip qml/ prefix so types register as "Main" not "qml.Main"
set_source_files_properties(qml/Main.qml          PROPERTIES QT_RESOURCE_ALIAS Main.qml)
set_source_files_properties(qml/SearchPanel.qml    PROPERTIES QT_RESOURCE_ALIAS SearchPanel.qml)
set_source_files_properties(qml/ResultItem.qml     PROPERTIES QT_RESOURCE_ALIAS ResultItem.qml)
set_source_files_properties(qml/SettingsPanel.qml  PROPERTIES QT_RESOURCE_ALIAS SettingsPanel.qml)
set_source_files_properties(qml/StatusBar.qml      PROPERTIES QT_RESOURCE_ALIAS StatusBar.qml)

# Onboarding QML files
set_source_files_properties(qml/onboarding/OnboardingWindow.qml PROPERTIES QT_RESOURCE_ALIAS onboarding/OnboardingWindow.qml)
set_source_files_properties(qml/onboarding/WelcomeStep.qml      PROPERTIES QT_RESOURCE_ALIAS onboarding/WelcomeStep.qml)
set_source_files_properties(qml/onboarding/FdaStep.qml          PROPERTIES QT_RESOURCE_ALIAS onboarding/FdaStep.qml)
set_source_files_properties(qml/onboarding/HomeMapStep.qml      PROPERTIES QT_RESOURCE_ALIAS onboarding/HomeMapStep.qml)
set_source_files_properties(qml/onboarding/ModelSetupStep.qml   PROPERTIES QT_RESOURCE_ALIAS onboarding/ModelSetupStep.qml)

# Component QML files
set_source_files_properties(qml/components/HotkeyRecorder.qml   PROPERTIES QT_RESOURCE_ALIAS components/HotkeyRecorder.qml)
set_source_files_properties(qml/components/DirectoryPicker.qml  PROPERTIES QT_RESOURCE_ALIAS components/DirectoryPicker.qml)
set_source_files_properties(qml/components/PatternEditor.qml    PROPERTIES QT_RESOURCE_ALIAS components/PatternEditor.qml)

qt_add_qml_module(betterspotlight
    URI BetterSpotlight
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/SearchPanel.qml
        qml/ResultItem.qml
        qml/SettingsPanel.qml
        qml/StatusBar.qml
        qml/onboarding/OnboardingWindow.qml
        qml/onboarding/WelcomeStep.qml
        qml/onboarding/FdaStep.qml
        qml/onboarding/HomeMapStep.qml
        qml/onboarding/ModelSetupStep.qml
        qml/components/HotkeyRecorder.qml
        qml/components/DirectoryPicker.qml
        qml/components/PatternEditor.qml
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
    "-framework ServiceManagement"
)

# Release-grade bundle layout depends on helper binaries existing before app
# post-build staging.
add_dependencies(betterspotlight
    betterspotlight-indexer
    betterspotlight-extractor
    betterspotlight-query
    betterspotlight-inference
)

if(APPLE AND BETTERSPOTLIGHT_ENABLE_SPARKLE)
    find_library(SPARKLE_FRAMEWORK Sparkle)
    if(SPARKLE_FRAMEWORK)
        target_link_libraries(betterspotlight PRIVATE ${SPARKLE_FRAMEWORK})
        target_compile_definitions(betterspotlight PRIVATE BETTERSPOTLIGHT_ENABLE_SPARKLE=1)
    else()
        message(WARNING "BETTERSPOTLIGHT_ENABLE_SPARKLE is ON but Sparkle.framework was not found. Building without Sparkle support.")
    endif()
endif()

set_target_properties(betterspotlight PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_ICON_FILE "BetterSpotlight.icns"
    MACOSX_BUNDLE_BUNDLE_NAME "BetterSpotlight"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.betterspotlight.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

if(TARGET betterspotlight-fetch-models)
    add_dependencies(betterspotlight betterspotlight-fetch-models)
endif()

add_custom_command(TARGET betterspotlight POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DBS_MODEL_SOURCE_DIR=${CMAKE_SOURCE_DIR}/data/models
        -DBS_MODEL_DEST_DIR=$<TARGET_FILE_DIR:betterspotlight>/../Resources/models
        -P ${CMAKE_SOURCE_DIR}/cmake/sync_embedding_models.cmake
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:betterspotlight>/../Helpers
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:betterspotlight-indexer>
        $<TARGET_FILE_DIR:betterspotlight>/../Helpers/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:betterspotlight-extractor>
        $<TARGET_FILE_DIR:betterspotlight>/../Helpers/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:betterspotlight-query>
        $<TARGET_FILE_DIR:betterspotlight>/../Helpers/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:betterspotlight-inference>
        $<TARGET_FILE_DIR:betterspotlight>/../Helpers/
    VERBATIM
)
