include_guard(GLOBAL)
if(COMMAND vnm_acquire_owned_dependency)
    return()
endif()
if("${FETCHCONTENT_SOURCE_DIR_VNM_CMAKE}" STREQUAL "")
    find_package(vnm_cmake CONFIG QUIET)
elseif(NOT EXISTS "${FETCHCONTENT_SOURCE_DIR_VNM_CMAKE}/CMakeLists.txt")
    message(FATAL_ERROR
        "FETCHCONTENT_SOURCE_DIR_VNM_CMAKE must name a vnm_cmake source directory: "
        "${FETCHCONTENT_SOURCE_DIR_VNM_CMAKE}")
endif()
if(NOT COMMAND vnm_acquire_owned_dependency)
    include(FetchContent)
    FetchContent_Declare(vnm_cmake
        GIT_REPOSITORY https://github.com/Varinomics/vnm_cmake.git
        GIT_TAG master)
    FetchContent_MakeAvailable(vnm_cmake)
endif()
