# ==============================================================================
# Function Plotter Example
# Demonstrates vnm_plot with mexce for plotting arbitrary mathematical functions
# ==============================================================================

cmake_minimum_required(VERSION 3.18)

# Only define project when building standalone (not as subdirectory)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(function_plotter
        VERSION 0.1.0
        DESCRIPTION "Interactive function plotter using vnm_plot and mexce"
        LANGUAGES CXX
    )
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Multimedia)

# vnm_plot library (from parent repo)
if(NOT TARGET vnm_plot::qtquick)
    set(VNM_PLOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.."
                     "${CMAKE_CURRENT_BINARY_DIR}/vnm_plot")
endif()

# mexce - use local checkout if available, otherwise fetch from GitHub
set(MEXCE_LOCAL_PATH "" CACHE PATH "Path to local mexce checkout (optional)")

if(MEXCE_LOCAL_PATH AND EXISTS "${MEXCE_LOCAL_PATH}/mexce.h")
    message(STATUS "Using local mexce checkout: ${MEXCE_LOCAL_PATH}")
    set(mexce_SOURCE_DIR "${MEXCE_LOCAL_PATH}")
else()
    include(FetchContent)
    # SOURCE_SUBDIR points at a non-existent path so FetchContent_MakeAvailable
    # downloads the sources but skips add_subdirectory. Without this, mexce's
    # own CMakeLists forces /O2 on its benchmark / unit_tests targets, which
    # collides with the parent build's /RTC1 in MSVC Debug. We only need the
    # header — the parent project does not consume mexce's executables.
    FetchContent_Declare(mexce
        GIT_REPOSITORY https://github.com/imakris/mexce.git
        GIT_TAG        master
        # Keep full history for fetched libraries because this workspace patches
        # and reviews them directly during active development.
        GIT_SHALLOW    FALSE
        SOURCE_SUBDIR  cmake_unused_skip_subdirectory
    )
    FetchContent_MakeAvailable(mexce)
endif()

# Create mexce interface library (header is at repo root, not in include/)
add_library(mexce_lib INTERFACE)
target_include_directories(mexce_lib INTERFACE ${mexce_SOURCE_DIR})

# -----------------------------------------------------------------------------
# Application
# -----------------------------------------------------------------------------

add_executable(function_plotter
    main.cpp
    function_plotter.cpp
    function_plotter.h
    resources.qrc
)

if(WIN32)
    target_sources(function_plotter PRIVATE app_icon.rc)
endif()

target_link_libraries(function_plotter PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Multimedia
    vnm_plot::qtquick
    mexce_lib
)

target_include_directories(function_plotter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# The icon face the toolbar buttons draw their glyphs with. It lives in
# vnm_fonts rather than beside this example, so it joins the resources here
# instead of in resources.qrc, which can only name files relative to itself.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/VnmFonts.cmake)
get_filename_component(_function_plotter_icon_font_dir
    "${VNM_PLOT_ICON_FONT_FILE}" DIRECTORY)
qt_add_resources(function_plotter "function_plotter_icon_font"
    PREFIX "/rc"
    BASE   "${_function_plotter_icon_font_dir}"
    FILES  "${VNM_PLOT_ICON_FONT_FILE}"
)
unset(_function_plotter_icon_font_dir)

if(VNM_PLOT_BUILD_TESTS)
    enable_testing()
    add_executable(test_function_plotter_stacking
        test_function_plotter_stacking.cpp
        function_plotter.cpp
        function_plotter.h
    )
    target_link_libraries(test_function_plotter_stacking PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::Multimedia
        vnm_plot::qtquick
        mexce_lib
    )
    target_include_directories(test_function_plotter_stacking PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME FunctionPlotterStacking COMMAND test_function_plotter_stacking)

    get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
    find_program(QMLTESTRUNNER_EXECUTABLE qmltestrunner HINTS "${_qt_bin_dir}")
    if(QMLTESTRUNNER_EXECUTABLE)
        add_test(NAME FunctionPlotterControls
            COMMAND "${QMLTESTRUNNER_EXECUTABLE}"
                -input "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        )
        set_tests_properties(FunctionPlotterControls PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
endif()

# windows.h ships min/max as macros; std::min / std::max in this TU expand
# wrong without NOMINMAX, breaking the format_timestamp lambda's digit clamp.
if(WIN32)
    target_compile_definitions(function_plotter PRIVATE NOMINMAX)
    if(TARGET test_function_plotter_stacking)
        target_compile_definitions(test_function_plotter_stacking PRIVATE NOMINMAX)
    endif()
endif()

# Deploy Qt runtime DLLs next to the example so it can be launched by
# double-clicking from Explorer (no manual PATH or vcvarsall shell needed).
if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
    get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")

    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET function_plotter POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    --no-translations
                    --no-system-d3d-compiler
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    "$<TARGET_FILE:function_plotter>"
            COMMENT "Deploying Qt runtime for function_plotter"
            VERBATIM
        )
    else()
        message(WARNING
            "windeployqt not found near Qt6::qmake (${_qt_bin_dir}); "
            "function_plotter.exe will not have Qt runtime DLLs alongside "
            "it and will fail to launch from Explorer.")
    endif()
endif()
