# SPDX-License-Identifier: Apache-2.0
#
# M0 spike S1 (PLAN.md section 7; ADR 0007), Windows only. The parent directory
# adds this one only when WIN32 and FROST_BUILD_UI, so the Linux build never
# configures it. These executables open windows on the real desktop and are run
# by run-s1.ps1 in the interactive session, never by CTest:
#   s1_probe_validate  GDI window with injected paint delays; validates the probes (no Qt);
#   s1_first_frame     launches frost.exe and times its first frame on screen (no Qt);
#   s1_editor          the two minimal editor views (raster QGlyphRun cache and a
#                      QRhiWidget glyph atlas) for the 10 s fling and typing runs.
# The Qt-free tools are in the diag layer with the probes; s1_editor uses Qt and is
# in the app layer (cmake/FrostLayers.cmake). Its headless tests are s1_editor_tests.

# Embeds the FrostIDE manifest (per-monitor v2 DPI awareness, which Desktop
# Duplication coordinates rely on; UTF-8 code page; long paths).
function(frost_s1_embed_manifest target)
    target_sources(${target} PRIVATE ${FROST_TEST_MANIFEST_RC})
    set_source_files_properties(${FROST_TEST_MANIFEST_RC} PROPERTIES OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
    target_link_options(${target} PRIVATE /MANIFEST:NO)
endfunction()

frost_add_executable(s1_probe_validate LAYER diag
    SOURCES probe_validate.cpp
    DEPS frost::perf_probes)
target_link_libraries(s1_probe_validate PRIVATE gdi32 shell32 user32)
frost_s1_embed_manifest(s1_probe_validate)

frost_add_executable(s1_first_frame LAYER diag
    SOURCES first_frame.cpp child_process.cpp child_process.hpp
    DEPS frost::perf_probes frost::core)
target_link_libraries(s1_first_frame PRIVATE dwmapi gdi32 shell32 user32)
frost_s1_embed_manifest(s1_first_frame)

# The QRhi API (rhi/qrhi.h, QShader) is semi-public: Qt ships it in the private
# Gui package, under the same LGPL licence as Gui. Qt warns that private APIs
# may change between minors; this is spike code pinned to FROST_QT_VERSION.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 ${FROST_QT_VERSION} EXACT REQUIRED COMPONENTS GuiPrivate)

# Compiled into both s1_editor and its headless tests.
set(s1_editor_sources
    editor/corpus.cpp
    editor/corpus.hpp
    editor/gates.cpp
    editor/gates.hpp
    editor/rhi_view.cpp
    editor/views.cpp
    editor/views.hpp)

frost_add_executable(s1_editor LAYER app
    SOURCES editor/main.cpp ${s1_editor_sources}
    DEPS frost::perf_probes frost::core
    THIRD_PARTY Qt6::Widgets Qt6::GuiPrivate)
target_link_libraries(s1_editor PRIVATE user32)
frost_s1_embed_manifest(s1_editor)
frost_copy_runtime_dlls(s1_editor)
add_custom_command(TARGET s1_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:s1_editor>/platforms
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FROST_QT_PLUGIN_DIR}/platforms/qwindows$<$<CONFIG:Debug>:d>.dll
            $<TARGET_FILE_DIR:s1_editor>/platforms
    VERBATIM)

frost_add_test(s1_editor_tests QT
    SOURCES editor/editor_test.cpp ${s1_editor_sources}
    DEPS frost::perf_probes frost::core
    THIRD_PARTY Qt6::GuiPrivate)
# Both targets copy the Qt DLLs into this one output directory after linking. In a
# fresh tree, two concurrent copies of the same file fail with a sharing violation
# ("permission denied"), so the tests link only after s1_editor's copies are done.
add_dependencies(s1_editor_tests s1_editor)
