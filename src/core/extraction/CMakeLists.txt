add_library(betterspotlight-core-extraction STATIC
    mdls_text_extractor.cpp
    text_extractor.cpp
    pdf_extractor.cpp
    ocr_extractor.cpp
    text_cleaner.cpp
    extraction_manager.cpp
)

target_include_directories(betterspotlight-core-extraction PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(betterspotlight-core-extraction PUBLIC
    betterspotlight-core-shared
    Qt6::Core
)

if(BETTERSPOTLIGHT_POPPLER_BACKEND STREQUAL "poppler-qt6")
    if(NOT TARGET PkgConfig::POPPLER_QT6)
        message(FATAL_ERROR "Validated Poppler backend is poppler-qt6, but PkgConfig::POPPLER_QT6 was not created")
    endif()
    target_compile_definitions(betterspotlight-core-extraction PRIVATE BS_POPPLER_QT6=1)
    target_link_libraries(betterspotlight-core-extraction PUBLIC PkgConfig::POPPLER_QT6)
elseif(BETTERSPOTLIGHT_POPPLER_BACKEND STREQUAL "poppler-cpp")
    if(NOT TARGET PkgConfig::POPPLER_CPP)
        message(FATAL_ERROR "Validated Poppler backend is poppler-cpp, but PkgConfig::POPPLER_CPP was not created")
    endif()
    target_compile_definitions(betterspotlight-core-extraction PRIVATE BS_POPPLER_CPP=1)
    target_link_libraries(betterspotlight-core-extraction PUBLIC PkgConfig::POPPLER_CPP)
endif()

if(Tesseract_FOUND)
    target_compile_definitions(betterspotlight-core-extraction PRIVATE TESSERACT_FOUND)
    target_link_libraries(betterspotlight-core-extraction PRIVATE Tesseract::Tesseract)
endif()
