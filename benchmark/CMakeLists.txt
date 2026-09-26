# ==============================================================================
# vnm_plot Benchmark
# Synthetic benchmark for vnm_plot rendering performance
# ==============================================================================
# Note: This file is included as a subdirectory from the root CMakeLists.txt
# and inherits cmake_minimum_required and project settings from parent.

find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Quick)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

# Find vnm_plot targets (should be available from parent)
if(NOT TARGET vnm_plot::rhi)
    message(FATAL_ERROR "vnm_plot RHI target not found. Build benchmark from vnm_plot root.")
endif()

if(NOT TARGET vnm_plot::qtquick)
    message(FATAL_ERROR "vnm_plot Qt Quick target not found. Build benchmark from vnm_plot root.")
endif()

# Benchmark should include text rendering by default.
if(NOT VNM_PLOT_ENABLE_TEXT)
    message(FATAL_ERROR "vnm_plot benchmark requires text rendering. Configure with -DVNM_PLOT_ENABLE_TEXT=ON or disable the benchmark.")
endif()

option(VNM_PLOT_BUILD_BENCHMARK_TESTS "Build benchmark helper tests" ON)

# -----------------------------------------------------------------------------
# Benchmark executable
# -----------------------------------------------------------------------------

set(BENCHMARK_SOURCES
    src/allocation_tracker.cpp
    src/main.cpp
    src/benchmark_window.cpp
)

set(BENCHMARK_HEADERS
    include/ring_buffer.h
    include/allocation_tracker.h
    include/path_io.h
    include/publication_rate_clock.h
    include/sample_types.h
    include/brownian_generator.h
    include/benchmark_profiler.h
    include/benchmark_data_source.h
    include/benchmark_window.h
)

add_executable(vnm_plot_benchmark
    ${BENCHMARK_SOURCES}
    ${BENCHMARK_HEADERS}
)

# Enable Qt MOC for benchmark window
set_target_properties(vnm_plot_benchmark PROPERTIES
    AUTOMOC ON
)

target_include_directories(vnm_plot_benchmark
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/qt
)

target_compile_features(vnm_plot_benchmark PRIVATE cxx_std_20)
target_compile_definitions(vnm_plot_benchmark PRIVATE
    VNM_PLOT_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)

# Link dependencies
target_link_libraries(vnm_plot_benchmark
    PRIVATE
        vnm_plot::qtquick
        vnm_plot::rhi
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Quick
        Threads::Threads
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(vnm_plot_benchmark PRIVATE NOMINMAX)
    target_link_libraries(vnm_plot_benchmark PRIVATE psapi)
    # Console application for benchmark output
    set_target_properties(vnm_plot_benchmark PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(vnm_plot_benchmark PRIVATE /W4)
else()
    target_compile_options(vnm_plot_benchmark PRIVATE -Wall -Wextra)
endif()

# Deploy Qt runtime DLLs next to the benchmark so it can be launched by
# double-clicking from Explorer (no manual PATH or vcvarsall shell needed).
if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
    get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")

    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET vnm_plot_benchmark POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    --no-translations
                    --no-system-d3d-compiler
                    "$<TARGET_FILE:vnm_plot_benchmark>"
            COMMENT "Deploying Qt runtime for vnm_plot_benchmark"
            VERBATIM
        )
    else()
        message(WARNING
            "windeployqt not found near Qt6::qmake (${_qt_bin_dir}); "
            "vnm_plot_benchmark.exe will not have Qt runtime DLLs alongside "
            "it and will fail to launch from Explorer.")
    endif()
endif()

message(STATUS "vnm_plot: Building benchmark executable")

if(VNM_PLOT_BUILD_BENCHMARK_TESTS)
# -----------------------------------------------------------------------------
# Ring buffer test executable
# -----------------------------------------------------------------------------

add_executable(test_ring_buffer
    src/test_ring_buffer.cpp
    ${BENCHMARK_HEADERS}
)

target_include_directories(test_ring_buffer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(test_ring_buffer PRIVATE cxx_std_20)

target_link_libraries(test_ring_buffer PRIVATE vnm_plot::data Threads::Threads)

if(WIN32)
    target_compile_definitions(test_ring_buffer PRIVATE NOMINMAX)
endif()

if(MSVC)
    target_compile_options(test_ring_buffer PRIVATE /W4)
else()
    target_compile_options(test_ring_buffer PRIVATE -Wall -Wextra)
endif()

# Register with CTest
include(CTest)
add_test(NAME ring_buffer_tests COMMAND test_ring_buffer)

message(STATUS "vnm_plot: Building ring buffer tests")

# -----------------------------------------------------------------------------
# Brownian generator test executable
# -----------------------------------------------------------------------------

add_executable(test_brownian_generator
    src/test_brownian_generator.cpp
    ${BENCHMARK_HEADERS}
)

target_include_directories(test_brownian_generator
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(test_brownian_generator PRIVATE cxx_std_20)

if(WIN32)
    target_compile_definitions(test_brownian_generator PRIVATE NOMINMAX)
endif()

if(MSVC)
    target_compile_options(test_brownian_generator PRIVATE /W4)
else()
    target_compile_options(test_brownian_generator PRIVATE -Wall -Wextra)
endif()

add_test(NAME brownian_generator_tests COMMAND test_brownian_generator)

message(STATUS "vnm_plot: Building Brownian generator tests")

# -----------------------------------------------------------------------------
# Benchmark profiler test executable
# -----------------------------------------------------------------------------

add_executable(test_benchmark_profiler
    src/allocation_tracker.cpp
    src/test_benchmark_profiler.cpp
    ${BENCHMARK_HEADERS}
)

target_include_directories(test_benchmark_profiler
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(test_benchmark_profiler PRIVATE cxx_std_20)

target_link_libraries(test_benchmark_profiler PRIVATE vnm_plot::data)

if(WIN32)
    target_compile_definitions(test_benchmark_profiler PRIVATE NOMINMAX)
endif()

if(MSVC)
    target_compile_options(test_benchmark_profiler PRIVATE /W4)
else()
    target_compile_options(test_benchmark_profiler PRIVATE -Wall -Wextra)
endif()

add_test(NAME benchmark_profiler_tests COMMAND test_benchmark_profiler)

message(STATUS "vnm_plot: Building benchmark profiler tests")

# -----------------------------------------------------------------------------
# Benchmark data source test executable
# -----------------------------------------------------------------------------

add_executable(test_benchmark_data_source
    src/test_benchmark_data_source.cpp
    ${BENCHMARK_HEADERS}
)

target_include_directories(test_benchmark_data_source
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(test_benchmark_data_source PRIVATE cxx_std_20)

target_link_libraries(test_benchmark_data_source PRIVATE vnm_plot::data Threads::Threads)

if(WIN32)
    target_compile_definitions(test_benchmark_data_source PRIVATE NOMINMAX)
endif()

if(MSVC)
    target_compile_options(test_benchmark_data_source PRIVATE /W4)
else()
    target_compile_options(test_benchmark_data_source PRIVATE -Wall -Wextra)
endif()

add_test(NAME benchmark_data_source_tests COMMAND test_benchmark_data_source)

message(STATUS "vnm_plot: Building benchmark data source tests")

add_test(
    NAME benchmark_native_smoke
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_smoke.py"
        --executable "$<TARGET_FILE:vnm_plot_benchmark>"
        --output-dir "${CMAKE_CURRENT_BINARY_DIR}/smoke-reports/native"
        --graphics-backend native
        --frames 3
)
set_tests_properties(benchmark_native_smoke PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME benchmark_native_stacked_area_smoke
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_smoke.py"
        --executable "$<TARGET_FILE:vnm_plot_benchmark>"
        --output-dir "${CMAKE_CURRENT_BINARY_DIR}/smoke-reports/native-stacked-area"
        --graphics-backend native
        --frames 3
        --stacked
)
set_tests_properties(benchmark_native_stacked_area_smoke PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME benchmark_phase_trace_flush
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_phase_trace_flush.py"
        --executable "$<TARGET_FILE:vnm_plot_benchmark>"
)
set_tests_properties(benchmark_phase_trace_flush PROPERTIES
    TIMEOUT 45
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME benchmark_shared_frame_pipeline
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_frame_pipeline.py"
        --executable "$<TARGET_FILE:vnm_plot_benchmark>"
)
set_tests_properties(benchmark_shared_frame_pipeline PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

endif()
