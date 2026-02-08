add_library(betterspotlight-core-extraction STATIC
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

if(POPPLER_QT6_FOUND)
    target_compile_definitions(betterspotlight-core-extraction PRIVATE BS_POPPLER_QT6=1)
    target_include_directories(betterspotlight-core-extraction PRIVATE ${POPPLER_QT6_INCLUDE_DIRS})
    target_link_directories(betterspotlight-core-extraction PUBLIC ${POPPLER_QT6_LIBRARY_DIRS})
    target_link_libraries(betterspotlight-core-extraction PUBLIC ${POPPLER_QT6_LIBRARIES})
elseif(POPPLER_CPP_FOUND)
    target_compile_definitions(betterspotlight-core-extraction PRIVATE BS_POPPLER_CPP=1)
    target_include_directories(betterspotlight-core-extraction PRIVATE ${POPPLER_CPP_INCLUDE_DIRS})
    target_link_directories(betterspotlight-core-extraction PUBLIC ${POPPLER_CPP_LIBRARY_DIRS})
    target_link_libraries(betterspotlight-core-extraction PUBLIC ${POPPLER_CPP_LIBRARIES})
endif()

if(Tesseract_FOUND)
    target_compile_definitions(betterspotlight-core-extraction PRIVATE TESSERACT_FOUND)
    target_link_libraries(betterspotlight-core-extraction PRIVATE Tesseract::Tesseract)
endif()
