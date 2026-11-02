# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_editor (PLAN.md section 2 "Editor rendering"; ADR 0037): the editor widget. EditorView
# (a QAbstractScrollArea over frost::text's DisplayMap and frost::text_editor's Editor) with the
# backend-neutral display list behind IEditorRenderer (the raster QPainter backend), the Qt
# ITextShaper, tree-sitter highlighting through frost::syntax and the theme's token colours, the
# gutter, the find/replace bar, Go to Line, IME, the editor actions in frost::input's registry and
# the settings adapter over frost::settings. Not in the fuzz trees (it needs Qt and vcpkg ports).

frost_add_library(ui_editor LAYER ui
    SOURCES
        src/display_list.cpp
        src/editor_actions.cpp
        src/editor_colors.cpp
        src/editor_key_routing.cpp
        src/editor_settings.cpp
        src/editor_view.cpp
        src/editor_view_input.cpp
        src/editor_view_paint.cpp
        src/find_bar.cpp
        src/go_to_line.cpp
        src/qt_text_shaper.cpp
        src/raster_renderer.cpp
        src/syntax_outline.cpp
    DEPS
        frost::ui frost::ui_input frost::ui_model
        frost::text_editor frost::text frost::syntax frost::settings frost::input frost::sched frost::io
        frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_editor PRIVATE
    include/frost/ui/editor/editor_settings.hpp
    include/frost/ui/editor/editor_view.hpp
    include/frost/ui/editor/find_bar.hpp
    include/frost/ui/editor/go_to_line.hpp)
set_target_properties(frost_ui_editor PROPERTIES AUTOMOC ON)
