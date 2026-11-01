# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_input (PLAN.md section 2.2 "Keyboard and IME"; ADR 0016): the Qt adapter of
# frost::input. It translates QKeyEvents into frost::input::KeyEvents (scan codes, the
# Windows AltGr refinement, dead keys, NumLock and IME state) and tracks IME composition.
# No QObject, so no AUTOMOC; the event filter that uses it belongs to the shell.

frost_add_library(ui_input LAYER ui
    SOURCES
        src/ime_tracker.cpp
        src/key_translation.cpp
    DEPS frost::input frost::core
    THIRD_PARTY Qt6::Gui)
