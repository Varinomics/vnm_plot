cmake_minimum_required(VERSION 3.16)

set(_vnm_plot_test_names)
set(_vnm_plot_test_targets)
set(_vnm_plot_windows_test_targets)

function(vnm_plot_add_test test_name target_name component)
    add_executable(${target_name} ${target_name}.cpp)
    target_link_libraries(${target_name} PRIVATE vnm_plot::${component})
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME ${test_name} COMMAND ${target_name})

    list(APPEND _vnm_plot_test_names ${test_name})
    set(_vnm_plot_test_names "${_vnm_plot_test_names}" PARENT_SCOPE)
    list(APPEND _vnm_plot_test_targets ${target_name})
    set(_vnm_plot_test_targets "${_vnm_plot_test_targets}" PARENT_SCOPE)
    if(component STREQUAL "rhi" OR component STREQUAL "qtquick")
        list(APPEND _vnm_plot_windows_test_targets ${target_name})
        set(_vnm_plot_windows_test_targets "${_vnm_plot_windows_test_targets}" PARENT_SCOPE)
    endif()
endfunction()

# Series diagnostics affect the class layout; every consumer of this test build
# must see the same definition, including when font rendering is disabled.
target_compile_definitions(vnm_plot_rhi PUBLIC VNM_PLOT_ENABLE_TEST_HOOKS=1)

vnm_plot_add_test(CacheInvalidation test_cache_invalidation layout)
vnm_plot_add_test(SnapshotCaching test_snapshot_caching rhi)
vnm_plot_add_test(TypedApi test_typed_api data)
vnm_plot_add_test(FunctionSampleSource test_function_sample_source data)
vnm_plot_add_test(CoreAlgo test_core_algo rhi)
vnm_plot_add_test(AssetLoader test_asset_loader rhi)
vnm_plot_add_test(ConcurrentSeries test_concurrent_series data)
vnm_plot_add_test(DataSourceQueries test_data_source_queries data)
vnm_plot_add_test(GpuSampleOrigin test_gpu_sample_origin data)
vnm_plot_add_test(LayoutCalculator test_layout_calculator layout)
vnm_plot_add_test(SeriesWindowPlanner test_series_window_planner layout)
vnm_plot_add_test(TimeGrid test_time_grid layout)
vnm_plot_add_test(RhiHelpers test_rhi_helpers rhi)
vnm_plot_add_test(LabelPaneGeometry test_label_pane_geometry rhi)
vnm_plot_add_test(QrhiLayerLifecycle test_qrhi_layer_lifecycle rhi)
vnm_plot_add_test(QrhiPublicApi test_qrhi_public_api rhi)
vnm_plot_add_test(LinePrecision test_line_precision rhi)
vnm_plot_add_test(SampleUploadReuse test_sample_upload_reuse rhi)

if(VNM_PLOT_ENABLE_TEXT)
    vnm_plot_add_test(AtomicFileWrite test_atomic_file_write rhi)
    vnm_plot_add_test(FontAtlasCache test_font_atlas_cache rhi)
    vnm_plot_add_test(FontDiskCache test_font_disk_cache rhi)
    vnm_plot_add_test(FontIdentity test_font_identity rhi)
    vnm_plot_add_test(FontRendererBounds test_font_renderer_bounds rhi)
    vnm_plot_add_test(FontRendererDrawRanges test_font_renderer_draw_ranges rhi)
endif()
vnm_plot_add_test(MsdfLcdShaderReference test_msdf_lcd_shader_reference rhi)

target_include_directories(test_function_sample_source PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../examples/common
)

find_package(Threads QUIET)
if(TARGET Threads::Threads)
    target_link_libraries(test_concurrent_series PRIVATE Threads::Threads)
endif()

target_link_libraries(test_qrhi_layer_lifecycle PRIVATE Qt6::GuiPrivate Qt6::Gui)
target_link_libraries(test_qrhi_public_api PRIVATE Qt6::GuiPrivate Qt6::Gui)
target_link_libraries(test_rhi_helpers PRIVATE Qt6::GuiPrivate Qt6::Gui)
target_link_libraries(test_line_precision PRIVATE Qt6::Quick Qt6::GuiPrivate Qt6::Gui)
target_link_libraries(test_sample_upload_reuse PRIVATE Qt6::Quick Qt6::GuiPrivate Qt6::Gui)

if(TARGET test_atomic_file_write)
    if(TARGET Threads::Threads)
        target_link_libraries(test_atomic_file_write PRIVATE Threads::Threads)
    endif()
endif()
if(TARGET test_font_atlas_cache)
    # The cache header carries the built atlas type, so the test needs the
    # atlas builder's headers as well as the library it is linked against.
    target_link_libraries(test_font_atlas_cache PRIVATE vnm_msdf_text::rhi)
    if(TARGET Threads::Threads)
        target_link_libraries(test_font_atlas_cache PRIVATE Threads::Threads)
    endif()
endif()
if(TARGET test_font_disk_cache)
    target_compile_definitions(test_font_disk_cache PRIVATE VNM_PLOT_ENABLE_TEST_HOOKS=1)
endif()
if(TARGET test_font_identity)
    if(TARGET Threads::Threads)
        target_link_libraries(test_font_identity PRIVATE Threads::Threads)
    endif()
endif()
if(TARGET test_font_renderer_draw_ranges)
    # The frames it compares are rasterized through the QRhi of a Qt Quick
    # render control, which picks a real backend on every platform.
    target_link_libraries(test_font_renderer_draw_ranges PRIVATE Qt6::Quick Qt6::GuiPrivate Qt6::Gui)
endif()
if(TARGET test_msdf_lcd_shader_reference)
    if(NOT TARGET vnm_msdf_text::lcd_shader_reference)
        find_package(vnm_msdf_text CONFIG REQUIRED
            COMPONENTS lcd_shader_reference)
    endif()
    target_link_libraries(test_msdf_lcd_shader_reference PRIVATE vnm_msdf_text::lcd_shader_reference)
    target_compile_definitions(test_msdf_lcd_shader_reference PRIVATE
        VNM_PLOT_LCD_FILTER_PATH="${VNM_MSDF_TEXT_SHADER_DIR}/lcd_filter.glsl"
        VNM_PLOT_GRID_FRAG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../shaders/qsb/grid_quad.frag"
    )
endif()

if(TARGET vnm_plot_qtquick)
    vnm_plot_add_test(QmlModule test_qml_module qtquick)

    vnm_plot_add_test(PlotInteractionItem test_plot_interaction_item qtquick)

    vnm_plot_add_test(PlotRendererLifecycle test_plot_renderer_lifecycle qtquick)
    target_link_libraries(test_plot_renderer_lifecycle
        PRIVATE
            Qt6::GuiPrivate
            Qt6::Gui
    )
    target_include_directories(test_plot_renderer_lifecycle PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/qt
    )

    vnm_plot_add_test(PlotTimeAxis test_plot_time_axis qtquick)

    vnm_plot_add_test(LcdResolver test_lcd_resolver qtquick)
    target_link_libraries(test_lcd_resolver
        PRIVATE
            vnm_msdf_text::qt_lcd
            Qt6::GuiPrivate
            Qt6::Gui
    )
    target_include_directories(test_lcd_resolver PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/qt
    )
endif()

function(vnm_plot_target_is_imported out_var target_name)
    set(_imported FALSE)
    if(TARGET ${target_name})
        get_target_property(_target_imported ${target_name} IMPORTED)
        if(_target_imported)
            set(_imported TRUE)
        endif()
    endif()
    set(${out_var} ${_imported} PARENT_SCOPE)
endfunction()

set(_vnm_plot_package_smoke_viable TRUE)
set(_vnm_plot_package_smoke_skip_reason)

if(NOT CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(_vnm_plot_package_smoke_viable FALSE)
    set(_vnm_plot_package_smoke_skip_reason
        "top-level install packaging is not active")
endif()

if(_vnm_plot_package_smoke_viable)
    foreach(_dep IN ITEMS glm::glm vnm_msdf_text::lcd_contract)
        vnm_plot_target_is_imported(_vnm_plot_dep_imported ${_dep})
        if(NOT _vnm_plot_dep_imported)
            set(_vnm_plot_package_smoke_viable FALSE)
            set(_vnm_plot_package_smoke_skip_reason
                "install export requires imported glm and vnm_msdf_text targets")
        endif()
    endforeach()
endif()

if(_vnm_plot_package_smoke_viable AND VNM_PLOT_ENABLE_TEXT)
    vnm_plot_target_is_imported(
        _vnm_plot_msdf_text_atlas_imported
        vnm_msdf_text::vnm_msdf_text)
    if(NOT _vnm_plot_msdf_text_atlas_imported)
        set(_vnm_plot_package_smoke_viable FALSE)
        set(_vnm_plot_package_smoke_skip_reason
            "text-enabled install export requires imported vnm_msdf_text atlas target")
    endif()
endif()

if(_vnm_plot_package_smoke_viable)
    include(GNUInstallDirs)
    include("${PROJECT_SOURCE_DIR}/cmake/vnm_cmake_dependency.cmake")
    vnm_write_toolchain_context("${CMAKE_CURRENT_BINARY_DIR}/nested-toolchain.cmake")
    set(_dependency_context "${CMAKE_CURRENT_BINARY_DIR}/package-dependencies.cmake")
    file(WRITE "${_dependency_context}"
        "set(CMAKE_PREFIX_PATH [==[${CMAKE_PREFIX_PATH}]==] CACHE STRING \"\" FORCE)\n")
    foreach(_package IN ITEMS glm vnm_msdf_text Freetype msdfgen Qt6)
        if(${_package}_DIR AND NOT "${${_package}_DIR}" MATCHES "-NOTFOUND$")
            file(APPEND "${_dependency_context}"
                "set(${_package}_DIR [==[${${_package}_DIR}]==] CACHE PATH \"\" FORCE)\n")
        endif()
    endforeach()
    add_test(
        NAME PackageSmoke
        COMMAND ${CMAKE_COMMAND}
            "-DVNM_PLOT_BINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DVNM_PLOT_INSTALL_CMAKEDIR=${CMAKE_INSTALL_LIBDIR}/cmake/vnm_plot"
            "-DVNM_PLOT_DEPENDENCY_CONTEXT=${_dependency_context}"
            "-DVNM_PLOT_TEST_CONFIG=$<CONFIG>"
            "-DVNM_TOOLCHAIN_CONTEXT=${CMAKE_CURRENT_BINARY_DIR}/nested-toolchain.cmake"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/package_smoke.cmake")
    list(APPEND _vnm_plot_test_names PackageSmoke)
else()
    message(STATUS
        "vnm_plot: PackageSmoke test not configured: "
        "${_vnm_plot_package_smoke_skip_reason}")
endif()

unset(_vnm_plot_dep_imported)
unset(_vnm_plot_msdf_text_atlas_imported)
unset(_vnm_plot_package_smoke_skip_reason)
unset(_vnm_plot_package_smoke_viable)

if(WIN32)
    get_target_property(_vnm_plot_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_vnm_plot_qt_bin_dir "${_vnm_plot_qmake_executable}" DIRECTORY)
    get_filename_component(_vnm_plot_compiler_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)

    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        set_tests_properties(${_vnm_plot_test_names} PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${_vnm_plot_compiler_bin_dir};PATH=path_list_prepend:${_vnm_plot_qt_bin_dir}"
        )
    else()
        set(_vnm_plot_test_path "$ENV{PATH}")
        set(_vnm_plot_test_path "${_vnm_plot_qt_bin_dir};${_vnm_plot_compiler_bin_dir};${_vnm_plot_test_path}")
        string(REPLACE ";" "\\;" _vnm_plot_test_path "${_vnm_plot_test_path}")
        set_tests_properties(${_vnm_plot_test_names} PROPERTIES
            ENVIRONMENT "PATH=${_vnm_plot_test_path}"
        )
    endif()

    set(_vnm_plot_qt_runtime_targets Qt6::Core Qt6::Gui)
    foreach(_qt_target IN ITEMS
        Qt6::Quick
        Qt6::Qml
        Qt6::QmlModels
        Qt6::Network
        Qt6::OpenGL
        Qt6::QmlWorkerScript
    )
        if(TARGET ${_qt_target})
            list(APPEND _vnm_plot_qt_runtime_targets ${_qt_target})
        endif()
    endforeach()

    set(_vnm_plot_windows_test_runtime_dir "${CMAKE_CURRENT_BINARY_DIR}")
    set(_vnm_plot_windows_test_runtime_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_vnm_plot_windows_test_runtime_dir}"
    )

    foreach(_qt_target IN LISTS _vnm_plot_qt_runtime_targets)
        list(APPEND _vnm_plot_windows_test_runtime_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${_qt_target}>"
                "${_vnm_plot_windows_test_runtime_dir}"
        )
    endforeach()

    if(EXISTS "${_vnm_plot_qt_bin_dir}/../plugins/platforms/qwindows.dll")
        list(APPEND _vnm_plot_windows_test_runtime_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${_vnm_plot_windows_test_runtime_dir}/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_vnm_plot_qt_bin_dir}/../plugins/platforms/qwindows.dll"
                "${_vnm_plot_windows_test_runtime_dir}/platforms"
        )
    endif()

    foreach(_qt_plugin_dir IN ITEMS
        iconengines
        imageformats
        styles
        tls
    )
        if(EXISTS "${_vnm_plot_qt_bin_dir}/../plugins/${_qt_plugin_dir}")
            list(APPEND _vnm_plot_windows_test_runtime_commands
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_vnm_plot_qt_bin_dir}/../plugins/${_qt_plugin_dir}"
                    "${_vnm_plot_windows_test_runtime_dir}/${_qt_plugin_dir}"
            )
        endif()
    endforeach()

    foreach(_runtime_dll IN ITEMS
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll
    )
        if(EXISTS "${_vnm_plot_compiler_bin_dir}/${_runtime_dll}")
            list(APPEND _vnm_plot_windows_test_runtime_commands
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_vnm_plot_compiler_bin_dir}/${_runtime_dll}"
                    "${_vnm_plot_windows_test_runtime_dir}"
            )
        endif()
    endforeach()

    add_custom_target(vnm_plot_deploy_windows_test_runtime
        ${_vnm_plot_windows_test_runtime_commands}
        VERBATIM
    )

    foreach(_test IN LISTS _vnm_plot_windows_test_targets)
        add_dependencies(${_test} vnm_plot_deploy_windows_test_runtime)
    endforeach()
endif()

# Print test info
message(STATUS "Configured tests:")
foreach(_test IN LISTS _vnm_plot_test_targets)
    message(STATUS "  - ${_test}")
endforeach()
