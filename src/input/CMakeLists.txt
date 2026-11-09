# SPDX-License-Identifier: Apache-2.0
#
# frost::input (PLAN.md section 2.2 "Keyboard and IME"; ADR 0016): the Qt-free key model,
# keyboard-layout tables and the layout-aware mapping, the shortcut matcher, keymaps (the
# built-in IntelliJ-compatible keymap and format v1), the action registry, the Find Action
# model and the KeymapService facade, plus the SimulatedKeyboard double. Depends on core
# only; the Qt adapter is frost::ui_input (src/ui/input).

set(frost_input_sources
    src/action.cpp
    src/binding_index.cpp
    src/find_action.cpp
    src/key.cpp
    src/key_event.cpp
    src/keymap.cpp
    src/keymap_intellij.cpp
    src/keymap_visual_studio.cpp
    src/keymap_service.cpp
    src/layout.cpp
    src/layout_reader.cpp
    src/shortcut_matcher.cpp
    src/simulated_keyboard.cpp
    src/simulated_layouts.cpp)
if(WIN32)
    list(APPEND frost_input_sources src/layout_reader_win.cpp)
else()
    list(APPEND frost_input_sources src/layout_reader_posix.cpp)
endif()

frost_add_library(input LAYER input
    SOURCES ${frost_input_sources}
    DEPS frost::core)
if(WIN32)
    # The read-only active-layout reader (layout_reader_win.cpp).
    target_link_libraries(frost_input PRIVATE user32)
endif()
