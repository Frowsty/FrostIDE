# SPDX-License-Identifier: Apache-2.0
#
# Build-time resources deployed next to frost (ADR 0015): the compiled themes (*.ftheme, from
# frost-themec), the icon atlases (icons-<pct>.fia, from frost-icon-baker), the bundled fonts
# and the licence files that travel with them. Everything lands in FROST_RESOURCE_OUTPUT_DIR,
# <frost's output directory>/resources, which is published as a GLOBAL property for tests/ui.
# Budget: B28. Every command depends only on its inputs and its tool, and both tools rewrite an
# output only when its bytes change, so an unrelated edit re-runs nothing here.

get_target_property(_frost_out frost RUNTIME_OUTPUT_DIRECTORY)
if(NOT _frost_out)
    get_target_property(_frost_out frost BINARY_DIR)
endif()
get_property(_frost_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_frost_multi)
    set(FROST_RESOURCE_OUTPUT_DIR "${_frost_out}/$<CONFIG>/resources")
else()
    set(FROST_RESOURCE_OUTPUT_DIR "${_frost_out}/resources")
endif()
set_property(GLOBAL PROPERTY FROST_RESOURCE_OUTPUT_DIR "${FROST_RESOURCE_OUTPUT_DIR}")

set(_frost_res "${FROST_RESOURCE_OUTPUT_DIR}")
set(_frost_outputs "")

# Themes: one command per theme so an edit recompiles only that theme.
set(_frost_themes
    glacier-dark
    glacier-light
    glacier-classic
    glacier-high-contrast
    rider-dark
    rider-light
    rider-islands-dark
    darcula
    monokai-pro
    dracula
    nord
    one-dark
    solarized-dark
    solarized-light
    catppuccin-mocha
    catppuccin-macchiato
    catppuccin-frappe
    catppuccin-latte
    tokyo-night
    tokyo-night-storm
    tokyo-night-light
    gruvbox-dark
    gruvbox-light
    github-dark
    github-light
    ayu-dark
    ayu-mirage
    ayu-light
    everforest-dark
    kanagawa-wave
    rose-pine
    rose-pine-dawn
    night-owl
    synthwave-84)
foreach(theme IN LISTS _frost_themes)
    set(json "${CMAKE_CURRENT_SOURCE_DIR}/themes/${theme}.theme.json")
    set(out "${_frost_res}/themes/${theme}.ftheme")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_frost_res}/themes"
        COMMAND $<TARGET_FILE:frost-themec> --out-dir "${_frost_res}/themes" "${json}"
        DEPENDS "${json}" frost-themec
        COMMENT "Compiling theme ${theme}"
        VERBATIM)
    list(APPEND _frost_outputs "${out}")
endforeach()

# Icons: one command bakes every scale; the depfile lists the manifest and every SVG it read.
set(_frost_icon_outputs "")
foreach(scale IN ITEMS 100 125 150 175 200 250 300)
    list(APPEND _frost_icon_outputs "${_frost_res}/icons/icons-${scale}.fia")
endforeach()
set(_frost_manifest "${CMAKE_CURRENT_SOURCE_DIR}/icons/icons.json")
add_custom_command(OUTPUT ${_frost_icon_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_frost_res}/icons"
    COMMAND $<TARGET_FILE:frost-icon-baker>
            --manifest "${_frost_manifest}"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/icons"
            --out-dir "${_frost_res}/icons"
            --depfile "${CMAKE_CURRENT_BINARY_DIR}/icons-$<CONFIG>.d"
    DEPENDS "${_frost_manifest}" frost-icon-baker
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/icons-$<CONFIG>.d"
    COMMENT "Baking icon atlases"
    VERBATIM)
list(APPEND _frost_outputs ${_frost_icon_outputs})

# Fonts and licence files, copied byte-exact (OFL-1.1: bundled unmodified, licence alongside).
function(_frost_copy_resource source destination)
    get_filename_component(directory "${destination}" DIRECTORY)
    add_custom_command(OUTPUT "${destination}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "${destination}"
        DEPENDS "${source}"
        VERBATIM)
    set_property(DIRECTORY APPEND PROPERTY _FROST_RESOURCE_COPIES "${destination}")
endfunction()
foreach(font IN ITEMS Inter-Regular Inter-Medium Inter-SemiBold Inter-Bold)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/inter/${font}.ttf" "${_frost_res}/fonts/inter/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/inter/LICENSE.txt" "${_frost_res}/fonts/inter/LICENSE.txt")
foreach(font IN ITEMS JetBrainsMono-Regular JetBrainsMono-Italic JetBrainsMono-Bold JetBrainsMono-BoldItalic)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/jetbrains-mono/${font}.ttf"
                         "${_frost_res}/fonts/jetbrains-mono/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/jetbrains-mono/OFL.txt" "${_frost_res}/fonts/jetbrains-mono/OFL.txt")
foreach(font IN ITEMS FiraCode-Regular FiraCode-Medium FiraCode-SemiBold FiraCode-Bold)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/fira-code/${font}.ttf"
                         "${_frost_res}/fonts/fira-code/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/fira-code/LICENSE" "${_frost_res}/fonts/fira-code/LICENSE")
foreach(font IN ITEMS CascadiaCode-Regular CascadiaCode-Italic CascadiaCode-Bold CascadiaCode-BoldItalic
                      CascadiaMono-Regular CascadiaMono-Italic CascadiaMono-Bold CascadiaMono-BoldItalic)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/cascadia-code/${font}.ttf"
                         "${_frost_res}/fonts/cascadia-code/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/cascadia-code/LICENSE" "${_frost_res}/fonts/cascadia-code/LICENSE")
foreach(font IN ITEMS SourceCodePro-Regular SourceCodePro-It SourceCodePro-Bold SourceCodePro-BoldIt)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/source-code-pro/${font}.ttf"
                         "${_frost_res}/fonts/source-code-pro/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/source-code-pro/LICENSE.md" "${_frost_res}/fonts/source-code-pro/LICENSE.md")
foreach(font IN ITEMS IBMPlexMono-Regular IBMPlexMono-Italic IBMPlexMono-Bold IBMPlexMono-BoldItalic)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/ibm-plex-mono/${font}.ttf"
                         "${_frost_res}/fonts/ibm-plex-mono/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/ibm-plex-mono/LICENSE.txt" "${_frost_res}/fonts/ibm-plex-mono/LICENSE.txt")
foreach(font IN ITEMS Inconsolata-Regular Inconsolata-Medium Inconsolata-SemiBold Inconsolata-Bold)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/inconsolata/${font}.ttf"
                         "${_frost_res}/fonts/inconsolata/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/inconsolata/OFL.txt" "${_frost_res}/fonts/inconsolata/OFL.txt")
foreach(font IN ITEMS IntelOneMono-Regular IntelOneMono-Italic IntelOneMono-Bold IntelOneMono-BoldItalic)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/intel-one-mono/${font}.ttf"
                         "${_frost_res}/fonts/intel-one-mono/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/intel-one-mono/OFL.txt" "${_frost_res}/fonts/intel-one-mono/OFL.txt")
foreach(font IN ITEMS CommitMono-400-Regular CommitMono-400-Italic CommitMono-700-Regular CommitMono-700-Italic)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/commit-mono/${font}.ttf"
                         "${_frost_res}/fonts/commit-mono/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/commit-mono/OFL.txt" "${_frost_res}/fonts/commit-mono/OFL.txt")
foreach(font IN ITEMS FantasqueSansMono-Regular FantasqueSansMono-Italic FantasqueSansMono-Bold FantasqueSansMono-BoldItalic)
    _frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/fantasque-sans-mono/${font}.ttf"
                         "${_frost_res}/fonts/fantasque-sans-mono/${font}.ttf")
endforeach()
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/fonts/fantasque-sans-mono/LICENSE.txt" "${_frost_res}/fonts/fantasque-sans-mono/LICENSE.txt")
_frost_copy_resource("${CMAKE_CURRENT_SOURCE_DIR}/icons/lucide/LICENSE" "${_frost_res}/icons/LICENSE.lucide.txt")
get_property(_frost_copies DIRECTORY PROPERTY _FROST_RESOURCE_COPIES)
list(APPEND _frost_outputs ${_frost_copies})

add_custom_target(frost_resources ALL DEPENDS ${_frost_outputs})
add_dependencies(frost frost_resources)
