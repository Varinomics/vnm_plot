# The fonts this project ships, taken from vnm_fonts, which ships every font
# Varinomics embeds byte-verbatim together with its licence and notice.
#
# vnm_fonts serves a file contract and a library contract. This project uses
# only the first: VNM_FONTS_DIRECTORY names the verbatim files, and the library
# marks a family name on the way into QFontDatabase, which two files declaring
# one family need so they cannot merge into a single entry and serve glyph
# lookup and rasterisation from different files. The face vnm_plot_rhi embeds
# never reaches a font database - its atlas is baked straight from the bytes -
# so there is no family entry to collide with, and vnm::fonts is deliberately
# not linked.

if(NOT VNM_FONTS_DIRECTORY)
    # Nothing in this tree has published the files yet, so this build obtains
    # vnm_fonts itself. Adding it sets VNM_FONTS_DIRECTORY below.
    include(FetchContent)

    get_filename_component(_vnm_plot_vnm_fonts_sibling
        "${CMAKE_CURRENT_LIST_DIR}/../../vnm_fonts" ABSOLUTE)
    if(NOT EXISTS "${_vnm_plot_vnm_fonts_sibling}/CMakeLists.txt")
        set(_vnm_plot_vnm_fonts_sibling "")
    endif()

    set(VNM_PLOT_VNM_FONTS_SOURCE_DIR "${_vnm_plot_vnm_fonts_sibling}"
        CACHE PATH "Local vnm_fonts checkout; empty fetches vnm_fonts from GitHub")
    unset(_vnm_plot_vnm_fonts_sibling)

    if(VNM_PLOT_VNM_FONTS_SOURCE_DIR)
        message(STATUS
            "vnm_plot: Using local vnm_fonts checkout: ${VNM_PLOT_VNM_FONTS_SOURCE_DIR}")
        FetchContent_Declare(vnm_fonts
            SOURCE_DIR "${VNM_PLOT_VNM_FONTS_SOURCE_DIR}"
        )
    else()
        message(STATUS "vnm_plot: Fetching vnm_fonts")
        FetchContent_Declare(vnm_fonts
            GIT_REPOSITORY https://github.com/Varinomics/vnm_fonts.git
            GIT_TAG        master
            GIT_SHALLOW    TRUE
        )
    endif()
    FetchContent_MakeAvailable(vnm_fonts)
endif()

# The monospace face vnm_plot_rhi embeds and bakes its MSDF atlas from, and the
# icon face the function plotter example puts in its resources.
set(VNM_PLOT_MONOSPACE_FONT_FILE "${VNM_FONTS_DIRECTORY}/UbuntuMono-Bront.ttf")
set(VNM_PLOT_ICON_FONT_FILE      "${VNM_FONTS_DIRECTORY}/FontAwesome7Free-Solid.otf")

foreach(_vnm_plot_font IN ITEMS
    "${VNM_PLOT_MONOSPACE_FONT_FILE}"
    "${VNM_PLOT_ICON_FONT_FILE}"
)
    if(NOT EXISTS "${_vnm_plot_font}")
        message(FATAL_ERROR
            "vnm_plot: ${_vnm_plot_font} does not exist. ${VNM_FONTS_DIRECTORY} "
            "does not carry the fonts this project ships.")
    endif()
endforeach()

unset(_vnm_plot_font)
