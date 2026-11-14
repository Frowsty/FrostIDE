# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_lang (W2, ADR 0041): the Rider-like language features over frost::lang. The widgets
# (completion lookup, parameter info, quick documentation, find-usages panel, file structure and
# go-to-symbol popups, rename field, quick-fix list) and LanguageController, which registers the
# IntelliJ-keymap actions and drives the widgets through the IEditorLanguageHost seam the
# workbench implements over its editor. EditorInsightBinding (ADR 0047) feeds one EditorView the
# server's semantic tokens, inactive regions and live diagnostics and the build's diagnostics.

frost_add_library(ui_lang LAYER ui
    SOURCES
        src/ast_view.cpp
        src/editor_insight_binding.cpp
        src/lang_widgets.cpp
        src/lang_widgets_internal.hpp
        src/lang_widgets_panels.cpp
        src/lang_widgets_popups.cpp
        src/language_controller.cpp
        src/language_controller_completion.cpp
        src/language_controller_editing.cpp
        src/language_controller_navigation.cpp
        src/language_controller_codegen.cpp
        src/language_controller_templates.cpp
        src/language_controller_spelling.cpp
        src/language_controller_hierarchy.cpp
        src/language_controller_refactor.cpp
        src/refactoring_preview.cpp
        src/resolve_context.cpp
    DEPS frost::ui_editor frost::lang frost::build frost::input frost::text frost::sched frost::io frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_lang PRIVATE
    include/frost/ui/lang/ast_view.hpp
    include/frost/ui/lang/editor_insight_binding.hpp
    include/frost/ui/lang/language_controller.hpp
    include/frost/ui/lang/lang_widgets.hpp
    include/frost/ui/lang/resolve_context.hpp)
set_target_properties(frost_ui_lang PROPERTIES AUTOMOC ON)
