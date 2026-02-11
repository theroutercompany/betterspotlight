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

function(bs_filter_existing_paths out_var)
    set(_filtered)
    foreach(_path IN LISTS ARGN)
        if(EXISTS "${_path}")
            list(APPEND _filtered "${_path}")
        endif()
    endforeach()
    set(${out_var} "${_filtered}" PARENT_SCOPE)
endfunction()

if(POPPLER_QT6_FOUND)
    bs_filter_existing_paths(_bs_poppler_qt6_include_dirs ${POPPLER_QT6_INCLUDE_DIRS})
    bs_filter_existing_paths(_bs_poppler_qt6_library_dirs ${POPPLER_QT6_LIBRARY_DIRS})

    target_compile_definitions(betterspotlight-core-extraction PRIVATE BS_POPPLER_QT6=1)
    target_include_directories(betterspotlight-core-extraction PRIVATE ${_bs_poppler_qt6_include_dirs})
    target_link_directories(betterspotlight-core-extraction PUBLIC ${_bs_poppler_qt6_library_dirs})
    target_link_libraries(betterspotlight-core-extraction PUBLIC ${POPPLER_QT6_LIBRARIES})
elseif(POPPLER_CPP_FOUND)
    bs_filter_existing_paths(_bs_poppler_cpp_include_dirs ${POPPLER_CPP_INCLUDE_DIRS})
    bs_filter_existing_paths(_bs_poppler_cpp_library_dirs ${POPPLER_CPP_LIBRARY_DIRS})

    set(_bs_has_poppler_cpp_headers OFF)
    foreach(_inc IN LISTS _bs_poppler_cpp_include_dirs)
        if(EXISTS "${_inc}/poppler-document.h")
            set(_bs_has_poppler_cpp_headers ON)
            break()
        endif()
    endforeach()

    if(APPLE)
        if(NOT _bs_has_poppler_cpp_headers)
            list(APPEND _bs_poppler_cpp_include_dirs
                /opt/homebrew/opt/poppler/include/poppler/cpp
                /opt/homebrew/opt/poppler/include/poppler
            )
            bs_filter_existing_paths(_bs_poppler_cpp_include_dirs ${_bs_poppler_cpp_include_dirs})
        endif()
        if(NOT _bs_poppler_cpp_library_dirs)
            set(_bs_poppler_cpp_library_dirs /opt/homebrew/opt/poppler/lib)
        endif()
    endif()

    target_compile_definitions(betterspotlight-core-extraction PRIVATE BS_POPPLER_CPP=1)
    target_include_directories(betterspotlight-core-extraction PRIVATE ${_bs_poppler_cpp_include_dirs})
    target_link_directories(betterspotlight-core-extraction PUBLIC ${_bs_poppler_cpp_library_dirs})
    target_link_libraries(betterspotlight-core-extraction PUBLIC ${POPPLER_CPP_LIBRARIES})
endif()

if(Tesseract_FOUND)
    target_compile_definitions(betterspotlight-core-extraction PRIVATE TESSERACT_FOUND)
    target_link_libraries(betterspotlight-core-extraction PRIVATE Tesseract::Tesseract)
endif()
