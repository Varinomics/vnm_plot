cmake_minimum_required(VERSION 3.16)

set(_vnm_plot_data_tests
    test_typed_api
    test_function_sample_source
    test_concurrent_series
    test_data_source_queries
    test_gpu_sample_origin
)

set(_vnm_plot_layout_tests
    test_cache_invalidation
    test_layout_calculator
    test_time_grid
)

set(_vnm_plot_rhi_tests
    test_snapshot_caching
    test_core_algo
    test_asset_loader
    test_rhi_helpers
    test_label_pane_geometry
    test_qrhi_layer_lifecycle
    test_qrhi_public_api
)

if(VNM_PLOT_ENABLE_TEXT)
    list(APPEND _vnm_plot_rhi_tests
        test_atomic_file_write
        test_font_atlas_cache
        test_font_disk_cache
        test_font_identity
        test_font_renderer_bounds
        test_font_renderer_draw_ranges
        test_msdf_lcd_shader_reference
    )
    target_compile_definitions(vnm_plot_rhi PRIVATE VNM_PLOT_ENABLE_TEST_HOOKS=1)
endif()

foreach(_test IN LISTS _vnm_plot_data_tests)
    add_executable(${_test} ${_test}.cpp)
    target_link_libraries(${_test} PRIVATE vnm_plot::data)
    set_target_properties(${_test} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endforeach()

foreach(_test IN LISTS _vnm_plot_layout_tests)
    add_executable(${_test} ${_test}.cpp)
    target_link_libraries(${_test} PRIVATE vnm_plot::layout)
    set_target_properties(${_test} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endforeach()

foreach(_test IN LISTS _vnm_plot_rhi_tests)
    add_executable(${_test} ${_test}.cpp)
    target_link_libraries(${_test} PRIVATE vnm_plot::rhi)
    set_target_properties(${_test} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endforeach()

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

if(TARGET test_atomic_file_write)
    if(TARGET Threads::Threads)
        target_link_libraries(test_atomic_file_write PRIVATE Threads::Threads)
    endif()
endif()
if(TARGET test_font_atlas_cache)
    # The cache header carries the built atlas type, so the test needs the
    # atlas builder's headers as well as the library it is linked against.
    target_link_libraries(test_font_atlas_cache PRIVATE vnm_msdf_text::vnm_msdf_text)
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
        VNM_PLOT_MSDF_TEXT_FRAG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../shaders/qsb/msdf_text.frag"
    )
endif()

if(TARGET vnm_plot_qtquick)
    add_executable(test_plot_interaction_item test_plot_interaction_item.cpp)
    target_link_libraries(test_plot_interaction_item PRIVATE vnm_plot::qtquick)

    add_executable(test_plot_renderer_lifecycle test_plot_renderer_lifecycle.cpp)
    target_link_libraries(test_plot_renderer_lifecycle
        PRIVATE
            vnm_plot::qtquick
            Qt6::GuiPrivate
            Qt6::Gui
    )
    target_include_directories(test_plot_renderer_lifecycle PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/qt
    )

    add_executable(test_plot_time_axis test_plot_time_axis.cpp)
    target_link_libraries(test_plot_time_axis PRIVATE vnm_plot::qtquick)

    add_executable(test_lcd_resolver test_lcd_resolver.cpp)
    target_link_libraries(test_lcd_resolver
        PRIVATE
            vnm_plot::qtquick
            Qt6::GuiPrivate
            Qt6::Gui
    )
    target_include_directories(test_lcd_resolver PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/qt
    )
endif()

if(TARGET vnm_plot_qtquick)
    set_target_properties(
        test_plot_interaction_item
        test_plot_renderer_lifecycle
        test_plot_time_axis
        test_lcd_resolver
        PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endif()

set(_vnm_plot_test_names)

function(vnm_plot_add_test test_name target_name)
    add_test(NAME ${test_name} COMMAND ${target_name})
    list(APPEND _vnm_plot_test_names ${test_name})
    set(_vnm_plot_test_names "${_vnm_plot_test_names}" PARENT_SCOPE)
endfunction()

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

function(vnm_plot_target_header_include_dir out_var target_name relative_header)
    set(_resolved_include_dir)
    get_target_property(_include_dirs ${target_name} INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_include_dir IN LISTS _include_dirs)
        string(GENEX_STRIP "${_include_dir}" _stripped_include_dir)
        if(IS_ABSOLUTE "${_stripped_include_dir}" AND
           EXISTS "${_stripped_include_dir}/${relative_header}")
            set(_resolved_include_dir "${_stripped_include_dir}")
            break()
        endif()
    endforeach()
    set(${out_var} "${_resolved_include_dir}" PARENT_SCOPE)
endfunction()

vnm_plot_add_test(CacheInvalidation test_cache_invalidation)
vnm_plot_add_test(SnapshotCaching test_snapshot_caching)
vnm_plot_add_test(TypedApi test_typed_api)
vnm_plot_add_test(FunctionSampleSource test_function_sample_source)
vnm_plot_add_test(CoreAlgo test_core_algo)
vnm_plot_add_test(AssetLoader test_asset_loader)
vnm_plot_add_test(ConcurrentSeries test_concurrent_series)
vnm_plot_add_test(DataSourceQueries test_data_source_queries)
vnm_plot_add_test(GpuSampleOrigin test_gpu_sample_origin)
vnm_plot_add_test(LayoutCalculator test_layout_calculator)
vnm_plot_add_test(TimeGrid test_time_grid)
vnm_plot_add_test(RhiHelpers test_rhi_helpers)
vnm_plot_add_test(LabelPaneGeometry test_label_pane_geometry)
vnm_plot_add_test(QrhiLayerLifecycle test_qrhi_layer_lifecycle)
vnm_plot_add_test(QrhiPublicApi test_qrhi_public_api)
if(TARGET test_atomic_file_write)
    vnm_plot_add_test(AtomicFileWrite test_atomic_file_write)
endif()
if(TARGET test_font_atlas_cache)
    vnm_plot_add_test(FontAtlasCache test_font_atlas_cache)
endif()
if(TARGET test_font_disk_cache)
    vnm_plot_add_test(FontDiskCache test_font_disk_cache)
endif()
if(TARGET test_font_identity)
    vnm_plot_add_test(FontIdentity test_font_identity)
endif()
if(TARGET test_font_renderer_bounds)
    vnm_plot_add_test(FontRendererBounds test_font_renderer_bounds)
endif()
if(TARGET test_font_renderer_draw_ranges)
    vnm_plot_add_test(FontRendererDrawRanges test_font_renderer_draw_ranges)
endif()
if(TARGET test_msdf_lcd_shader_reference)
    vnm_plot_add_test(MsdfLcdShaderReference test_msdf_lcd_shader_reference)
endif()

if(TARGET vnm_plot_qtquick)
    vnm_plot_add_test(PlotInteractionItem test_plot_interaction_item)
    vnm_plot_add_test(PlotRendererLifecycle test_plot_renderer_lifecycle)
    vnm_plot_add_test(PlotTimeAxis test_plot_time_axis)
    vnm_plot_add_test(LcdResolver test_lcd_resolver)
endif()

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
    vnm_plot_target_header_include_dir(
        _vnm_plot_glm_include_dir
        glm::glm
        glm/glm.hpp)
    vnm_plot_target_header_include_dir(
        _vnm_plot_msdf_text_include_dir
        vnm_msdf_text::lcd_contract
        vnm_msdf_text/lcd_contract.h)
    if(NOT _vnm_plot_glm_include_dir OR
       NOT _vnm_plot_msdf_text_include_dir)
        set(_vnm_plot_package_smoke_viable FALSE)
        set(_vnm_plot_package_smoke_skip_reason
            "package smoke could not resolve public dependency include directories")
    endif()
endif()

if(_vnm_plot_package_smoke_viable)
    add_test(
        NAME PackageSmoke
        COMMAND ${CMAKE_COMMAND}
            "-DVNM_PLOT_BINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DVNM_PLOT_GLM_INCLUDE_DIR=${_vnm_plot_glm_include_dir}"
            "-DVNM_PLOT_MSDF_TEXT_INCLUDE_DIR=${_vnm_plot_msdf_text_include_dir}"
            "-DVNM_PLOT_TEST_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DVNM_PLOT_TEST_CONFIG=$<CONFIG>"
            "-DVNM_PLOT_TEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DVNM_PLOT_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DVNM_PLOT_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DVNM_PLOT_TEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DVNM_PLOT_TEST_QT6_DIR=${Qt6_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/package_smoke.cmake")
    list(APPEND _vnm_plot_test_names PackageSmoke)
else()
    message(STATUS
        "vnm_plot: PackageSmoke test not configured: "
        "${_vnm_plot_package_smoke_skip_reason}")
endif()

unset(_vnm_plot_dep_imported)
unset(_vnm_plot_glm_include_dir)
unset(_vnm_plot_msdf_text_atlas_imported)
unset(_vnm_plot_msdf_text_include_dir)
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

    set(_vnm_plot_windows_test_targets ${_vnm_plot_rhi_tests})
    if(TARGET test_plot_interaction_item)
        list(APPEND _vnm_plot_windows_test_targets test_plot_interaction_item)
    endif()
    if(TARGET test_plot_renderer_lifecycle)
        list(APPEND _vnm_plot_windows_test_targets test_plot_renderer_lifecycle)
    endif()
    if(TARGET test_plot_time_axis)
        list(APPEND _vnm_plot_windows_test_targets test_plot_time_axis)
    endif()
    if(TARGET test_lcd_resolver)
        list(APPEND _vnm_plot_windows_test_targets test_lcd_resolver)
    endif()
    if(TARGET test_msdf_lcd_shader_reference)
        list(APPEND _vnm_plot_windows_test_targets test_msdf_lcd_shader_reference)
    endif()

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
foreach(_test IN LISTS _vnm_plot_data_tests)
    message(STATUS "  - ${_test}")
endforeach()
foreach(_test IN LISTS _vnm_plot_layout_tests)
    message(STATUS "  - ${_test}")
endforeach()
foreach(_test IN LISTS _vnm_plot_rhi_tests)
    message(STATUS "  - ${_test}")
endforeach()
if(TARGET vnm_plot_qtquick)
    message(STATUS "  - test_plot_interaction_item")
    message(STATUS "  - test_plot_renderer_lifecycle")
    message(STATUS "  - test_plot_time_axis")
    message(STATUS "  - test_lcd_resolver")
endif()
