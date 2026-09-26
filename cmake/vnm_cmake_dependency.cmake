include_guard(GLOBAL)
if(COMMAND vnm_acquire_owned_dependency)
    return()
endif()
if(NOT FETCHCONTENT_SOURCE_DIR_VNM_CMAKE)
    find_package(vnm_cmake CONFIG QUIET)
endif()
if(NOT COMMAND vnm_acquire_owned_dependency)
    include(FetchContent)
    FetchContent_Declare(vnm_cmake
        GIT_REPOSITORY https://github.com/Varinomics/vnm_cmake.git
        GIT_TAG master)
    FetchContent_MakeAvailable(vnm_cmake)
endif()
