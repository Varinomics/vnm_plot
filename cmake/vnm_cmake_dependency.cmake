include_guard(GLOBAL)
if(COMMAND vnm_acquire_owned_dependency)
    return()
endif()
find_package(vnm_cmake CONFIG QUIET)
if(NOT vnm_cmake_FOUND)
    include(FetchContent)
    FetchContent_Declare(vnm_cmake
        GIT_REPOSITORY https://github.com/Varinomics/vnm_cmake.git
        GIT_TAG master)
    FetchContent_MakeAvailable(vnm_cmake)
endif()
