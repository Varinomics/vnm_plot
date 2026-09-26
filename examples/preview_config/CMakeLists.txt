cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vnm_plot_preview_config
        VERSION 0.1.0
        DESCRIPTION "vnm_plot preview config example"
        LANGUAGES CXX
    )
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick)

if(NOT TARGET vnm_plot::qtquick)
    set(VNM_PLOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.."
                     "${CMAKE_CURRENT_BINARY_DIR}/vnm_plot")
endif()

add_executable(vnm_plot_preview_config
    src/main.cpp
    src/preview_controller.cpp
    src/preview_controller.h
    src/top_controller.cpp
    src/top_controller.h
    resources.qrc
)

target_include_directories(vnm_plot_preview_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(vnm_plot_preview_config PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    vnm_plot::qtquick
)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/deploy_qt.cmake)
vnm_plot_deploy_example(vnm_plot_preview_config)
