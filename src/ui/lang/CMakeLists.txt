# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_lang (W2, ADR 0041): the Rider-like language features over frost::lang. The widgets
# (completion lookup, parameter info, quick documentation, find-usages panel, file structure and
# go-to-symbol popups, rename field, quick-fix list) and LanguageController, which registers the
# IntelliJ-keymap actions and drives the widgets through the IEditorLanguageHost seam the
# workbench implements over its editor.

frost_add_library(ui_lang LAYER ui
    SOURCES
        src/lang_widgets.cpp
        src/language_controller.cpp
    DEPS frost::lang frost::input frost::text frost::sched frost::io frost::core
    THIRD_PARTY Qt6::Widgets)
# The Q_OBJECT classes live in public headers; moc runs on them through AUTOMOC.
target_sources(frost_ui_lang PRIVATE
    include/frost/ui/lang/language_controller.hpp
    include/frost/ui/lang/lang_widgets.hpp)
set_target_properties(frost_ui_lang PROPERTIES AUTOMOC ON)
