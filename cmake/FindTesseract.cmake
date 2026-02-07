# FindTesseract.cmake — locate Tesseract OCR + Leptonica via pkg-config
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(TESSERACT_PC QUIET tesseract)
    pkg_check_modules(LEPT_PC QUIET lept)
endif()

if(TESSERACT_PC_FOUND AND LEPT_PC_FOUND)
    set(Tesseract_FOUND TRUE)
    set(Tesseract_LIBRARIES ${TESSERACT_PC_LINK_LIBRARIES} ${LEPT_PC_LINK_LIBRARIES})
    set(Tesseract_LIBRARY_DIRS ${TESSERACT_PC_LIBRARY_DIRS} ${LEPT_PC_LIBRARY_DIRS})
    set(Tesseract_INCLUDE_DIRS ${TESSERACT_PC_INCLUDE_DIRS})

    # Leptonica's pkg-config often points to the leptonica/ subdirectory itself
    # (e.g. /opt/homebrew/.../include/leptonica). We need the parent directory
    # so that #include <leptonica/allheaders.h> resolves correctly.
    foreach(_dir IN LISTS LEPT_PC_INCLUDE_DIRS)
        get_filename_component(_basename "${_dir}" NAME)
        if(_basename STREQUAL "leptonica")
            get_filename_component(_parent "${_dir}" DIRECTORY)
            list(APPEND Tesseract_INCLUDE_DIRS "${_parent}")
        else()
            list(APPEND Tesseract_INCLUDE_DIRS "${_dir}")
        endif()
    endforeach()

    if(NOT TARGET Tesseract::Tesseract)
        add_library(Tesseract::Tesseract INTERFACE IMPORTED)
        set_target_properties(Tesseract::Tesseract PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Tesseract_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${Tesseract_LIBRARIES}"
            INTERFACE_LINK_DIRECTORIES "${Tesseract_LIBRARY_DIRS}"
        )
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Tesseract DEFAULT_MSG Tesseract_LIBRARIES Tesseract_INCLUDE_DIRS)
