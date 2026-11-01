# SPDX-License-Identifier: Apache-2.0
#
# Tests for the ui layer's libraries. ui_input_tests: the Qt adapter of frost::input
# (frost/ui/key_translation.hpp, frost/ui/ime_tracker.hpp), offscreen; events are
# constructed and translated directly or sent with QCoreApplication::sendEvent, and no
# window is shown.

frost_add_test(ui_input_tests QT
    SOURCES key_translation_test.cpp ime_tracker_test.cpp adapter_integration_test.cpp
    DEPS frost::ui_input frost::input)
