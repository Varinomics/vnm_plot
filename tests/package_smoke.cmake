cmake_minimum_required(VERSION 3.16)
include("${VNM_TOOLCHAIN_CONTEXT}")

foreach(_required_var IN ITEMS
    VNM_PLOT_BINARY_DIR
    VNM_PLOT_DEPENDENCY_CONTEXT)
    if(NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
        message(FATAL_ERROR "tests/package_smoke.cmake requires ${_required_var}.")
    endif()
endforeach()

get_filename_component(_binary_dir "${VNM_PLOT_BINARY_DIR}" ABSOLUTE)
set(_work_dir "${_binary_dir}/package_smoke")
set(_install_prefix "${_work_dir}/install")

file(TO_CMAKE_PATH "${_binary_dir}" _binary_dir_cmake)
file(TO_CMAKE_PATH "${_work_dir}" _work_dir_cmake)
string(FIND "${_work_dir_cmake}/" "${_binary_dir_cmake}/" _work_dir_prefix)
if(NOT _work_dir_prefix EQUAL 0)
    message(FATAL_ERROR "Refusing to clean package smoke directory outside the build tree.")
endif()

file(REMOVE_RECURSE "${_work_dir}")
file(MAKE_DIRECTORY "${_work_dir}")

set(_install_command
    "${CMAKE_COMMAND}" --install "${_binary_dir}"
    --prefix "${_install_prefix}")
if(DEFINED VNM_PLOT_TEST_CONFIG AND
   NOT VNM_PLOT_TEST_CONFIG STREQUAL "")
    list(APPEND _install_command --config "${VNM_PLOT_TEST_CONFIG}")
endif()
execute_process(
    COMMAND ${_install_command}
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE  _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "Installing vnm_plot for package smoke failed.\n"
        "${_install_output}\n${_install_error}")
endif()

function(vnm_plot_consumer_configure_command out_var consumer_source_dir consumer_build_dir)
    set(_command
        "${CMAKE_COMMAND}"
        -S "${consumer_source_dir}"
        -B "${consumer_build_dir}"
        -C "${VNM_PLOT_DEPENDENCY_CONTEXT}"
        "-Dvnm_plot_DIR=${_install_prefix}/lib/cmake/vnm_plot"
        -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE)

    vnm_append_toolchain_args(_command)
    set(${out_var} "${_command}" PARENT_SCOPE)
endfunction()

set(_consumer_source_dir "${_work_dir}/consumer")
set(_consumer_build_dir "${_work_dir}/consumer-build")
file(MAKE_DIRECTORY "${_consumer_source_dir}")

file(WRITE "${_consumer_source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.16)
project(vnm_plot_package_consumer LANGUAGES CXX)

find_package(vnm_plot CONFIG REQUIRED COMPONENTS rhi)

foreach(_target IN ITEMS vnm_plot::data vnm_plot::layout vnm_plot::rhi)
    if(NOT TARGET ${_target})
        message(FATAL_ERROR "vnm_plot package did not export ${_target}.")
    endif()
endforeach()

if(NOT TARGET vnm_msdf_text::lcd_contract)
    message(FATAL_ERROR
        "vnm_plot public headers require vnm_msdf_text::lcd_contract.")
endif()
get_target_property(_data_links vnm_plot::data INTERFACE_LINK_LIBRARIES)
if(NOT _data_links MATCHES "vnm_msdf_text::lcd_contract")
    message(FATAL_ERROR
        "vnm_plot::data must publish the LCD contract dependency.")
endif()

foreach(_target IN ITEMS vnm_plot::data vnm_plot::layout vnm_plot::rhi)
    get_target_property(_links ${_target} INTERFACE_LINK_LIBRARIES)
    if(_links MATCHES "vnm_msdf_text::lcd_shader_reference")
        message(FATAL_ERROR
            "${_target} must not publish lcd_shader_reference.")
    endif()
endforeach()

add_executable(vnm_plot_package_consumer main.cpp)
target_compile_features(vnm_plot_package_consumer PRIVATE cxx_std_20)
target_link_libraries(vnm_plot_package_consumer PRIVATE vnm_plot::rhi)

add_custom_target(run_vnm_plot_package_consumer
    COMMAND vnm_plot_package_consumer
    DEPENDS vnm_plot_package_consumer
    VERBATIM)
]=])

file(WRITE "${_consumer_source_dir}/main.cpp" [=[
#include <vnm_plot/rhi/font_renderer.h>

#include <type_traits>

int main()
{
    using plot_order_t = vnm::plot::lcd_subpixel_order_t;
    using shared_order_t = vnm::msdf_text::lcd::Resolved_lcd_subpixel_order;

    static_assert(std::is_same_v<plot_order_t, shared_order_t>);

    constexpr auto request = vnm::plot::lcd_explicit_request(plot_order_t::RGB);
    static_assert(!request.automatic);
    static_assert(request.resolved_order == shared_order_t::RGB);

    vnm::plot::text_lcd_t lcd;
    lcd.subpixel_order = plot_order_t::BGR;
    return lcd.subpixel_order == shared_order_t::BGR ? 0 : 1;
}
]=])

vnm_plot_consumer_configure_command(
    _configure_command
    "${_consumer_source_dir}"
    "${_consumer_build_dir}")
execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE  _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Configuring vnm_plot package consumer failed.\n"
        "${_configure_output}\n${_configure_error}")
endif()

set(_build_command "${CMAKE_COMMAND}" --build "${_consumer_build_dir}" --parallel 1)
if(DEFINED VNM_PLOT_TEST_CONFIG AND
   NOT VNM_PLOT_TEST_CONFIG STREQUAL "")
    list(APPEND _build_command --config "${VNM_PLOT_TEST_CONFIG}")
endif()
execute_process(
    COMMAND ${_build_command}
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE  _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Building vnm_plot package consumer failed.\n"
        "${_build_output}\n${_build_error}")
endif()

set(_run_command
    "${CMAKE_COMMAND}" --build "${_consumer_build_dir}"
    --target run_vnm_plot_package_consumer)
if(DEFINED VNM_PLOT_TEST_CONFIG AND
   NOT VNM_PLOT_TEST_CONFIG STREQUAL "")
    list(APPEND _run_command --config "${VNM_PLOT_TEST_CONFIG}")
endif()
execute_process(
    COMMAND ${_run_command}
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_output
    ERROR_VARIABLE  _run_error)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR
        "Running vnm_plot package consumer failed.\n"
        "${_run_output}\n${_run_error}")
endif()
