# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_editor (PLAN.md section 2 "Editor rendering"; ADR 0037): the editor widget. EditorView
# (a QAbstractScrollArea over frost::text's DisplayMap and frost::text_editor's Editor) with the
# backend-neutral display list behind IEditorRenderer (the raster QPainter backend), the Qt
# ITextShaper, tree-sitter highlighting through frost::syntax and the theme's token colours, the
# gutter, the find/replace bar, Go to Line, IME, the editor actions in frost::input's registry and
# the settings adapter over frost::settings; and (ADR 0047) semantic highlighting, inactive regions,
# diagnostics with squiggles, the error stripe, the inspection widget, F2 navigation and the
# status-bar problems indicator. Not in the fuzz trees (it needs Qt and vcpkg ports).
#
# The QRhi glyph-atlas backend (ADR 0043; ADR 0011 A1) is contained in editor_backend, glyph_atlas,
# rhi_glyph_painter and rhi_renderer, and shares display_strokes (the squiggles' and chrome shapes'
# QPainter code) with the raster backend for parity: only rhi_glyph_painter.cpp and rhi_renderer.cpp include Qt's
# QRhi headers (rhi/qrhi.h), which Qt ships in its private Gui package under Gui's licence (LGPL;
# tools/licensecheck.py maps GuiPrivate to Gui). Qt may change that API between minors, so the
# package is pinned to FROST_QT_VERSION like the S1 spike's.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 ${FROST_QT_VERSION} EXACT REQUIRED COMPONENTS GuiPrivate)

frost_add_library(ui_editor LAYER ui
    SOURCES
        src/display_list.cpp
        src/display_strokes.cpp
        src/editor_actions.cpp
        src/editor_backend.cpp
        src/editor_colors.cpp
        src/editor_insight.cpp
        src/editor_key_routing.cpp
        src/editor_settings.cpp
        src/editor_view.cpp
        src/editor_view_input.cpp
        src/editor_view_insight.cpp
        src/editor_view_paint.cpp
        src/editor_view_renderer.cpp
        src/find_bar.cpp
        src/glyph_atlas.cpp
        src/go_to_line.cpp
        src/insight_status.cpp
        src/qt_text_shaper.cpp
        src/raster_renderer.cpp
        src/rhi_glyph_painter.cpp
        src/rhi_renderer.cpp
        src/syntax_outline.cpp
    DEPS
        frost::ui frost::ui_input frost::ui_model
        frost::text_editor frost::text frost::syntax frost::settings frost::input frost::sched frost::io
        frost::core frost::compat
    THIRD_PARTY Qt6::Widgets Qt6::GuiPrivate)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_editor PRIVATE
    include/frost/ui/editor/editor_settings.hpp
    include/frost/ui/editor/editor_view.hpp
    include/frost/ui/editor/find_bar.hpp
    include/frost/ui/editor/go_to_line.hpp
    include/frost/ui/editor/insight_status.hpp
    include/frost/ui/editor/rhi_renderer.hpp)
set_target_properties(frost_ui_editor PROPERTIES AUTOMOC ON)
if(WIN32)
    # GetSystemMetrics(SM_REMOTESESSION) for the renderer rule's RDP check.
    target_link_libraries(frost_ui_editor PRIVATE user32)
endif()
