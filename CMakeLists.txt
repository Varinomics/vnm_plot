# ==============================================================================
# VNM Plot Library
# A high-performance, GPU-accelerated plotting library
# ==============================================================================

cmake_minimum_required(VERSION 3.16)

project(vnm_plot
    VERSION 0.1.0
    DESCRIPTION "High-performance GPU-accelerated plotting library"
    LANGUAGES CXX C
)

# Options
option(VNM_PLOT_BUILD_EXAMPLES "Build example applications" OFF)
option(VNM_PLOT_BUILD_FUNCTION_PLOTTER "Build the Qt Multimedia function_plotter example" OFF)
option(VNM_PLOT_ENABLE_TEXT "Enable MSDF text rendering (requires FreeType + msdfgen)" ON)
option(VNM_PLOT_BUILD_TESTS "Build test suite" OFF)
option(VNM_PLOT_USE_SYSTEM_LIBS "Prefer find_package dependencies over FetchContent when available" OFF)

include(FetchContent)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake)

if(VNM_PLOT_DEP_OVERRIDES_FILE AND EXISTS "${VNM_PLOT_DEP_OVERRIDES_FILE}")
    message(STATUS "vnm_plot: Loading dependency overrides from ${VNM_PLOT_DEP_OVERRIDES_FILE}")
    include("${VNM_PLOT_DEP_OVERRIDES_FILE}")
endif()

# -----------------------------------------------------------------------------
# Find or fetch required dependencies
# -----------------------------------------------------------------------------

# QRhi is the only renderer backend.
find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Quick)
find_package(Qt6ShaderTools QUIET)
if(NOT Qt6ShaderTools_FOUND)
    message(FATAL_ERROR
        "vnm_plot: Qt6ShaderTools is required for the QSB shader pipeline. "
        "Install the Qt 6 'Shader Tools' module on the build machine.")
endif()

# Text rendering is optional - stub implementations maintain stable layout when OFF
if(NOT VNM_PLOT_ENABLE_TEXT)
    message(STATUS "vnm_plot: Text rendering disabled (using stub implementations)")
endif()

# --- GLM ---
if(TARGET glm::glm)
    message(STATUS "vnm_plot: Using existing glm::glm target")
elseif(TARGET glm)
    add_library(glm::glm ALIAS glm)
    message(STATUS "vnm_plot: Using existing glm target (aliased)")
else()
    if(VNM_PLOT_USE_SYSTEM_LIBS)
        find_package(glm CONFIG QUIET)
        if(TARGET glm::glm)
            message(STATUS "vnm_plot: Using glm from find_package")
        endif()
    endif()
    if(NOT TARGET glm::glm)
        if(FETCHCONTENT_SOURCE_DIR_GLM AND
           EXISTS "${FETCHCONTENT_SOURCE_DIR_GLM}/CMakeLists.txt")
            message(STATUS "vnm_plot: Using local glm checkout: ${FETCHCONTENT_SOURCE_DIR_GLM}")
        else()
            message(STATUS "vnm_plot: Fetching glm")
        endif()
        FetchContent_Declare(glm
            GIT_REPOSITORY https://github.com/g-truc/glm.git
            GIT_TAG        1.0.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(glm)
        if(TARGET glm AND NOT TARGET glm::glm)
            add_library(glm::glm ALIAS glm)
        endif()
    endif()
endif()

# --- Shared MSDF LCD contract and text atlas builder ---
function(_vnm_plot_alias_existing_msdf_text_targets)
    if(TARGET vnm_msdf_text_lcd_contract AND
       NOT TARGET vnm_msdf_text::lcd_contract)
        add_library(vnm_msdf_text::lcd_contract ALIAS vnm_msdf_text_lcd_contract)
    endif()
    if(TARGET vnm_msdf_text AND
       NOT TARGET vnm_msdf_text::vnm_msdf_text)
        add_library(vnm_msdf_text::vnm_msdf_text ALIAS vnm_msdf_text)
    endif()
endfunction()

function(_vnm_plot_missing_targets out_var)
    set(_missing)
    foreach(_target IN LISTS ARGN)
        if(NOT TARGET ${_target})
            list(APPEND _missing ${_target})
        endif()
    endforeach()
    set(${out_var} "${_missing}" PARENT_SCOPE)
endfunction()

function(_vnm_plot_has_any_msdf_text_target out_var)
    set(_has_target FALSE)
    foreach(_target IN ITEMS
        vnm_msdf_text::lcd_contract
        vnm_msdf_text::vnm_msdf_text
        vnm_msdf_text_lcd_contract
        vnm_msdf_text
    )
        if(TARGET ${_target})
            set(_has_target TRUE)
        endif()
    endforeach()
    set(${out_var} ${_has_target} PARENT_SCOPE)
endfunction()

function(_vnm_plot_add_msdf_text_lcd_contract_from_source source_dir)
    if(TARGET vnm_msdf_text::lcd_contract)
        return()
    endif()

    if(TARGET vnm_msdf_text_lcd_contract)
        _vnm_plot_alias_existing_msdf_text_targets()
        return()
    endif()

    if(NOT EXISTS "${source_dir}/include/vnm_msdf_text/lcd_contract.h")
        message(FATAL_ERROR
            "vnm_plot: vnm_msdf_text LCD contract header not found in "
            "${source_dir}.")
    endif()

    add_library(vnm_msdf_text_lcd_contract INTERFACE)
    add_library(vnm_msdf_text::lcd_contract ALIAS vnm_msdf_text_lcd_contract)

    set_target_properties(vnm_msdf_text_lcd_contract PROPERTIES
        EXPORT_NAME lcd_contract
    )

    target_include_directories(vnm_msdf_text_lcd_contract
        INTERFACE
            $<BUILD_INTERFACE:${source_dir}/include>
    )

    target_compile_features(vnm_msdf_text_lcd_contract INTERFACE cxx_std_17)
endfunction()

set(_vnm_plot_msdf_text_components lcd_contract)
set(_vnm_plot_msdf_text_required_targets vnm_msdf_text::lcd_contract)
if(VNM_PLOT_ENABLE_TEXT)
    list(APPEND _vnm_plot_msdf_text_components atlas)
    list(APPEND _vnm_plot_msdf_text_required_targets
        vnm_msdf_text::vnm_msdf_text
    )
endif()

if(VNM_PLOT_USE_SYSTEM_LIBS)
    set(_vnm_plot_msdf_text_fetch_deps OFF)
else()
    set(_vnm_plot_msdf_text_fetch_deps ON)
endif()

_vnm_plot_alias_existing_msdf_text_targets()
_vnm_plot_missing_targets(
    _vnm_plot_missing_msdf_text_targets
    ${_vnm_plot_msdf_text_required_targets})

if(_vnm_plot_missing_msdf_text_targets)
    _vnm_plot_has_any_msdf_text_target(_vnm_plot_has_partial_msdf_text_targets)
    if(VNM_PLOT_USE_SYSTEM_LIBS AND
       NOT _vnm_plot_has_partial_msdf_text_targets)
        find_package(vnm_msdf_text CONFIG QUIET
            COMPONENTS ${_vnm_plot_msdf_text_components})
        _vnm_plot_alias_existing_msdf_text_targets()
        _vnm_plot_missing_targets(
            _vnm_plot_missing_msdf_text_targets
            ${_vnm_plot_msdf_text_required_targets})
        if(NOT _vnm_plot_missing_msdf_text_targets)
            message(STATUS "vnm_plot: Using vnm_msdf_text from find_package")
        endif()
    endif()
endif()

if(_vnm_plot_missing_msdf_text_targets)
    _vnm_plot_has_any_msdf_text_target(_vnm_plot_has_partial_msdf_text_targets)
    if(NOT _vnm_plot_has_partial_msdf_text_targets)
        get_filename_component(_vnm_plot_msdf_text_source
            "${CMAKE_CURRENT_SOURCE_DIR}/../vnm_msdf_text"
            ABSOLUTE)
        if(VNM_PLOT_ENABLE_TEXT)
            unset(VNM_MSDF_TEXT_USE_SYSTEM_LIBS CACHE)
            set(VNM_MSDF_TEXT_FETCH_DEPS
                "${_vnm_plot_msdf_text_fetch_deps}"
                CACHE BOOL "" FORCE)

            if(EXISTS "${_vnm_plot_msdf_text_source}/CMakeLists.txt")
                message(STATUS "vnm_plot: Using sibling vnm_msdf_text source")
                FetchContent_Declare(vnm_msdf_text
                    SOURCE_DIR "${_vnm_plot_msdf_text_source}"
                )
            else()
                message(STATUS "vnm_plot: Fetching vnm_msdf_text")
                FetchContent_Declare(vnm_msdf_text
                    GIT_REPOSITORY https://github.com/Varinomics/vnm_msdf_text.git
                    GIT_TAG        master
                    GIT_SHALLOW    TRUE
                )
            endif()
            FetchContent_MakeAvailable(vnm_msdf_text)
        else()
            if(EXISTS "${_vnm_plot_msdf_text_source}/include/vnm_msdf_text/lcd_contract.h")
                message(STATUS "vnm_plot: Using sibling vnm_msdf_text LCD contract headers")
                _vnm_plot_add_msdf_text_lcd_contract_from_source("${_vnm_plot_msdf_text_source}")
            else()
                message(STATUS "vnm_plot: Fetching vnm_msdf_text LCD contract headers")
                FetchContent_Declare(vnm_msdf_text
                    GIT_REPOSITORY https://github.com/Varinomics/vnm_msdf_text.git
                    GIT_TAG        master
                    GIT_SHALLOW    TRUE
                )
                FetchContent_GetProperties(vnm_msdf_text)
                if(NOT vnm_msdf_text_POPULATED)
                    FetchContent_Populate(vnm_msdf_text)
                endif()
                _vnm_plot_add_msdf_text_lcd_contract_from_source("${vnm_msdf_text_SOURCE_DIR}")
            endif()
        endif()
        unset(_vnm_plot_msdf_text_source)

        _vnm_plot_alias_existing_msdf_text_targets()
        _vnm_plot_missing_targets(
            _vnm_plot_missing_msdf_text_targets
            ${_vnm_plot_msdf_text_required_targets})
    endif()
endif()

if(_vnm_plot_missing_msdf_text_targets)
    string(REPLACE ";" ", " _vnm_plot_missing_msdf_text_targets_display
        "${_vnm_plot_missing_msdf_text_targets}")
    message(FATAL_ERROR
        "vnm_plot: missing required vnm_msdf_text target(s): "
        "${_vnm_plot_missing_msdf_text_targets_display}. Provide the full "
        "target set in the superbuild or a vnm_msdf_text package/source tree "
        "with components: ${_vnm_plot_msdf_text_components}.")
endif()

unset(_vnm_plot_has_partial_msdf_text_targets)
unset(_vnm_plot_missing_msdf_text_targets)
unset(_vnm_plot_missing_msdf_text_targets_display)
unset(_vnm_plot_msdf_text_components)
unset(_vnm_plot_msdf_text_fetch_deps)
unset(_vnm_plot_msdf_text_required_targets)

if(VNM_PLOT_ENABLE_TEXT)
    if(NOT VNM_MSDF_TEXT_DEFAULT_FONT_FILE)
        if(DEFINED vnm_msdf_text_SOURCE_DIR AND
           EXISTS "${vnm_msdf_text_SOURCE_DIR}/fonts/monospace.ttf")
            set(VNM_MSDF_TEXT_DEFAULT_FONT_FILE
                "${vnm_msdf_text_SOURCE_DIR}/fonts/monospace.ttf"
                CACHE FILEPATH "Default font embedded by vnm_plot" FORCE)
        else()
            get_filename_component(_vnm_plot_sibling_msdf_text_font
                "${CMAKE_CURRENT_SOURCE_DIR}/../vnm_msdf_text/fonts/monospace.ttf"
                ABSOLUTE)
            if(EXISTS "${_vnm_plot_sibling_msdf_text_font}")
                set(VNM_MSDF_TEXT_DEFAULT_FONT_FILE
                    "${_vnm_plot_sibling_msdf_text_font}"
                    CACHE FILEPATH "Default font embedded by vnm_plot" FORCE)
            endif()
            unset(_vnm_plot_sibling_msdf_text_font)
        endif()
    endif()

    if(NOT VNM_MSDF_TEXT_DEFAULT_FONT_FILE OR
       NOT EXISTS "${VNM_MSDF_TEXT_DEFAULT_FONT_FILE}")
        message(FATAL_ERROR
            "vnm_plot: VNM_MSDF_TEXT_DEFAULT_FONT_FILE must name an existing "
            "font file when VNM_PLOT_ENABLE_TEXT=ON.")
    endif()
endif()

# -----------------------------------------------------------------------------
# Source files - data/layout/RHI/Qt Quick
# -----------------------------------------------------------------------------

set(VNM_PLOT_DATA_SOURCES
    src/core/types.cpp
)

set(VNM_PLOT_DATA_HEADERS
    include/vnm_plot/core/basic_series_builder.h
    include/vnm_plot/core/access_policy.h
    include/vnm_plot/core/color_palette.h
    include/vnm_plot/core/constants.h
    include/vnm_plot/core/plot_config.h
    include/vnm_plot/core/series_builder.h
    include/vnm_plot/core/lcd.h
    include/vnm_plot/core/time_units.h
    include/vnm_plot/core/types.h
)

set(VNM_PLOT_LAYOUT_SOURCES
    src/core/layout_calculator.cpp
    src/core/auto_range_resolver.cpp
    src/core/frame_range_planner.cpp
    src/core/series_window_planner.cpp
    src/core/time_grid.cpp
)

set(VNM_PLOT_LAYOUT_HEADERS
    include/vnm_plot/core.h
    include/vnm_plot/core/algo.h
    include/vnm_plot/core/layout_calculator.h
    include/vnm_plot/core/series_window.h
    include/vnm_plot/core/time_grid.h
)

set(VNM_PLOT_RHI_SOURCES
    src/core/asset_loader.cpp
    src/core/platform_paths.cpp
    src/core/primitive_renderer.cpp
    src/core/chrome_renderer.cpp
    src/core/series_renderer.cpp
)

if(VNM_PLOT_ENABLE_TEXT)
    list(APPEND VNM_PLOT_RHI_SOURCES
        src/core/atomic_file_write.cpp
        src/core/font_atlas_cache.cpp
        src/core/font_renderer.cpp
        src/core/msdf_text_api_guard.cpp
        src/core/text_renderer.cpp
    )
else()
    list(APPEND VNM_PLOT_RHI_SOURCES
        src/core/font_renderer_stub.cpp
        src/core/text_renderer_stub.cpp
    )
endif()

set(VNM_PLOT_RHI_HEADERS
    include/vnm_plot/rhi.h
    include/vnm_plot/rhi/asset_loader.h
    include/vnm_plot/rhi/chrome_renderer.h
    include/vnm_plot/rhi/frame_context.h
    include/vnm_plot/rhi/font_renderer.h
    include/vnm_plot/rhi/primitive_renderer.h
    include/vnm_plot/rhi/qrhi_series_layer.h
    include/vnm_plot/rhi/series_builder.h
    include/vnm_plot/rhi/series_data.h
    include/vnm_plot/rhi/series_renderer.h
    include/vnm_plot/rhi/text_renderer.h
)

set(VNM_PLOT_QTQUICK_SOURCES
    src/qt/plot_render_feedback.cpp
    src/qt/plot_renderer.cpp
    src/qt/lcd_resolver.cpp
    src/qt/plot_widget.cpp
    src/qt/plot_interaction_item.cpp
    src/qt/plot_time_axis.cpp
)

set(VNM_PLOT_QTQUICK_HEADERS
    include/vnm_plot/vnm_plot.h
    include/vnm_plot/qt.h
    include/vnm_plot/qt/plot_widget.h
    include/vnm_plot/qt/plot_interaction_item.h
    include/vnm_plot/qt/plot_time_axis.h
    src/qt/plot_render_feedback.h
    src/qt/plot_renderer.h
    src/qt/lcd_resolver.h
)

# -----------------------------------------------------------------------------
# Generate embedded assets (shaders, fonts) for the RHI library
# -----------------------------------------------------------------------------

set(VNM_PLOT_EMBEDDED_ASSETS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_assets.cpp")
set(VNM_PLOT_RHI_ASSETS)

if(VNM_PLOT_ENABLE_TEXT)
    list(APPEND VNM_PLOT_RHI_ASSETS
        "${VNM_MSDF_TEXT_DEFAULT_FONT_FILE}:fonts/monospace.ttf"
    )
endif()

embed_assets(
    OUTPUT "${VNM_PLOT_EMBEDDED_ASSETS_OUTPUT}"
    NAMESPACE "vnm::plot"
    ASSETS ${VNM_PLOT_RHI_ASSETS}
)

list(APPEND VNM_PLOT_RHI_SOURCES "${VNM_PLOT_EMBEDDED_ASSETS_OUTPUT}")

function(vnm_plot_configure_library_target target_name)
    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${target_name} PUBLIC cxx_std_20)

    if(WIN32)
        target_compile_definitions(${target_name} PRIVATE NOMINMAX)
    endif()

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Data target
# -----------------------------------------------------------------------------

add_library(vnm_plot_data STATIC
    ${VNM_PLOT_DATA_SOURCES}
    ${VNM_PLOT_DATA_HEADERS}
)

add_library(vnm_plot::data ALIAS vnm_plot_data)
set_target_properties(vnm_plot_data PROPERTIES EXPORT_NAME data)
vnm_plot_configure_library_target(vnm_plot_data)

target_link_libraries(vnm_plot_data
    PUBLIC
        glm::glm
        vnm_msdf_text::lcd_contract
)

message(STATUS "vnm_plot: Building data library")

# -----------------------------------------------------------------------------
# Layout target
# -----------------------------------------------------------------------------

add_library(vnm_plot_layout STATIC
    ${VNM_PLOT_LAYOUT_SOURCES}
    ${VNM_PLOT_LAYOUT_HEADERS}
)

add_library(vnm_plot::layout ALIAS vnm_plot_layout)
set_target_properties(vnm_plot_layout PROPERTIES EXPORT_NAME layout)
vnm_plot_configure_library_target(vnm_plot_layout)

target_link_libraries(vnm_plot_layout
    PUBLIC
        vnm_plot_data
)

message(STATUS "vnm_plot: Building layout library")

# -----------------------------------------------------------------------------
# RHI target
# -----------------------------------------------------------------------------

add_library(vnm_plot_rhi STATIC
    ${VNM_PLOT_RHI_SOURCES}
    ${VNM_PLOT_RHI_HEADERS}
)

add_library(vnm_plot::rhi ALIAS vnm_plot_rhi)
set_target_properties(vnm_plot_rhi PROPERTIES EXPORT_NAME rhi)
vnm_plot_configure_library_target(vnm_plot_rhi)

target_include_directories(vnm_plot_rhi
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)

target_compile_definitions(vnm_plot_rhi PUBLIC VNM_PLOT_WITH_RHI=1)

target_link_libraries(vnm_plot_rhi
    PUBLIC
        vnm_plot_layout
    PRIVATE
        Qt6::Gui
        Qt6::GuiPrivate
)

if(VNM_PLOT_ENABLE_TEXT)
    target_compile_definitions(vnm_plot_rhi PUBLIC VNM_PLOT_ENABLE_TEXT=1)
    target_link_libraries(vnm_plot_rhi
        PRIVATE
            vnm_msdf_text::vnm_msdf_text
    )
endif()

message(STATUS "vnm_plot: Building RHI library")

# -----------------------------------------------------------------------------
# QSB shader pipeline (Qt RHI)
#
# Bake every shader in shaders/qsb/ into a multi-profile .qsb artifact and
# embed it under :/vnm_plot/shaders/qsb/<name>.qsb. Profile set covers Vulkan
# (SPIR-V), Metal 1.2 + 2.1, D3D11 (HLSL 5.0), OpenGL ES 3.0, and desktop
# GL 3.30 + 4.10 so RHI can pick a working artifact on every supported backend.
# -----------------------------------------------------------------------------

set(VNM_PLOT_QSB_SHADERS
    shaders/qsb/generic_rect.vert
    shaders/qsb/generic_rect.frag
    shaders/qsb/grid_quad.vert
    shaders/qsb/grid_quad.frag
    shaders/qsb/plot_line.vert
    shaders/qsb/plot_line.frag
    shaders/qsb/plot_dot_quad.vert
    shaders/qsb/plot_dot_quad.frag
    shaders/qsb/plot_area.vert
    shaders/qsb/plot_area.frag
)

if(VNM_PLOT_ENABLE_TEXT)
    list(APPEND VNM_PLOT_QSB_SHADERS
        shaders/qsb/msdf_text.vert
        shaders/qsb/msdf_text.frag
    )
endif()

# Each input shader yields :/vnm_plot/shaders/qsb/<name>.<ext>.qsb.
# The RHI shaders use vertex attributes and std140 uniform blocks, so retain
# GLSL ES 300 for mobile contexts, desktop GLSL 330 for GL 3.3 contexts, and
# GLSL 410 for existing GL 4 paths.
foreach(_vnm_plot_qsb_shader IN LISTS VNM_PLOT_QSB_SHADERS)
    get_filename_component(_vnm_plot_qsb_output_dir
        "${CMAKE_CURRENT_BINARY_DIR}/.qsb/${_vnm_plot_qsb_shader}"
        DIRECTORY)
    file(MAKE_DIRECTORY "${_vnm_plot_qsb_output_dir}")
endforeach()
unset(_vnm_plot_qsb_output_dir)
unset(_vnm_plot_qsb_shader)

qt_add_shaders(vnm_plot_rhi "vnm_plot_qsb_shaders"
    PREFIX "/vnm_plot"
    BASE   "${CMAKE_CURRENT_SOURCE_DIR}"
    GLSL   "300 es,330,410"
    HLSL   "50"
    MSL    "12,21"
    OUTPUT_TARGETS VNM_PLOT_RHI_RESOURCE_TARGETS
    FILES  ${VNM_PLOT_QSB_SHADERS}
)

# -----------------------------------------------------------------------------
# Qt Resources (only if building Qt wrapper)
# -----------------------------------------------------------------------------

qt6_add_resources(VNM_PLOT_QRC_SOURCES vnm_plot.qrc)

# -----------------------------------------------------------------------------
# Qt Quick target
# -----------------------------------------------------------------------------

add_library(vnm_plot_qtquick STATIC
    ${VNM_PLOT_QTQUICK_SOURCES}
    ${VNM_PLOT_QTQUICK_HEADERS}
    ${VNM_PLOT_QRC_SOURCES}
)

add_library(vnm_plot::qtquick ALIAS vnm_plot_qtquick)

set_target_properties(vnm_plot_qtquick PROPERTIES
    AUTOMOC ON
    EXPORT_NAME qtquick
)

vnm_plot_configure_library_target(vnm_plot_qtquick)

target_compile_definitions(vnm_plot_qtquick PUBLIC VNM_PLOT_WITH_QT=1)

# Link dependencies. Qt6::GuiPrivate exposes the rhi/qrhi.h public-by-private
# header tree that the QQuickRhiItem renderer uses inside plot_renderer.cpp;
# keep it PRIVATE so consumers do not inherit Qt's private headers.
target_link_libraries(vnm_plot_qtquick
    PUBLIC
        vnm_plot_rhi
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
    PRIVATE
        Qt6::GuiPrivate
)

if(WIN32)
    target_link_libraries(vnm_plot_qtquick PRIVATE user32)
endif()

message(STATUS "vnm_plot: Building Qt Quick library")

# -----------------------------------------------------------------------------
# Examples (require Qt)
# -----------------------------------------------------------------------------

if(VNM_PLOT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# -----------------------------------------------------------------------------
# Benchmark (requires Qt for window management)
# -----------------------------------------------------------------------------

option(VNM_PLOT_BUILD_BENCHMARK "Build benchmark executable" OFF)

if(VNM_PLOT_BUILD_BENCHMARK)
    enable_testing()
    add_subdirectory(benchmark)
endif()

# -----------------------------------------------------------------------------
# Tests (headless/unit-style)
# -----------------------------------------------------------------------------

if(VNM_PLOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Installation (only when building as top-level project)
# -----------------------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(GNUInstallDirs)

    # Install headers
    install(DIRECTORY include/vnm_plot
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    set(_vnm_plot_install_targets
        vnm_plot_data
        vnm_plot_layout
        vnm_plot_rhi
        vnm_plot_qtquick
        ${VNM_PLOT_RHI_RESOURCE_TARGETS}
    )

    # install(EXPORT) needs every link dependency to be IMPORTED (or in a
    # peer export set). glm and vnm_msdf_text only satisfy that when
    # VNM_PLOT_USE_SYSTEM_LIBS=ON brought them in via find_package; the
    # FetchContent path produces locally-built targets which can't be
    # exported here.
    set(_vnm_plot_export_ok TRUE)
    foreach(_dep IN ITEMS glm::glm vnm_msdf_text::lcd_contract)
        if(TARGET ${_dep})
            get_target_property(_dep_imported ${_dep} IMPORTED)
            if(NOT _dep_imported)
                set(_vnm_plot_export_ok FALSE)
            endif()
        endif()
    endforeach()
    if(VNM_PLOT_ENABLE_TEXT)
        foreach(_dep IN ITEMS vnm_msdf_text::vnm_msdf_text)
            if(TARGET ${_dep})
                get_target_property(_dep_imported ${_dep} IMPORTED)
                if(NOT _dep_imported)
                    set(_vnm_plot_export_ok FALSE)
                endif()
            endif()
        endforeach()
    endif()

    if(_vnm_plot_export_ok)
        install(TARGETS ${_vnm_plot_install_targets}
            EXPORT vnm_plotTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            OBJECTS DESTINATION ${CMAKE_INSTALL_LIBDIR}/vnm_plot/objects
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )

        install(EXPORT vnm_plotTargets
            NAMESPACE vnm_plot::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vnm_plot
        )

        include(CMakePackageConfigHelpers)

        write_basic_package_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/vnm_plot-config-version.cmake"
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion
        )

        set(VNM_PLOT_CONFIG_ENABLE_TEXT ${VNM_PLOT_ENABLE_TEXT})

        configure_package_config_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vnm_plot-config.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/vnm_plot-config.cmake"
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vnm_plot
        )

        install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/vnm_plot-config.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/vnm_plot-config-version.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vnm_plot
        )
    else()
        install(TARGETS ${_vnm_plot_install_targets}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            OBJECTS DESTINATION ${CMAKE_INSTALL_LIBDIR}/vnm_plot/objects
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
        message(STATUS
            "vnm_plot: Skipping find_package export because glm or vnm_msdf_text "
            "is built locally via FetchContent. Configure with "
            "-DVNM_PLOT_USE_SYSTEM_LIBS=ON (and have those packages "
            "find_package-able) to install a find_package-ready vnm_plot.")
    endif()
endif()
