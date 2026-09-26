include_guard(GLOBAL)

# Make examples launchable outside a developer shell, including QML imports.
function(vnm_plot_deploy_example target)
    if(NOT WIN32 OR CMAKE_CROSSCOMPILING)
        return()
    endif()

    get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")

    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    --no-translations
                    --no-system-d3d-compiler
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    "$<TARGET_FILE:${target}>"
            COMMENT "Deploying Qt runtime for ${target}"
            VERBATIM
        )
    else()
        message(WARNING
            "windeployqt not found near Qt6::qmake (${_qt_bin_dir}); "
            "${target}.exe will not have Qt runtime DLLs alongside it "
            "and will fail to launch from Explorer.")
    endif()
endfunction()
