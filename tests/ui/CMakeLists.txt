# SPDX-License-Identifier: Apache-2.0
#
# Tests for the ui layer's libraries. ui_input_tests: the Qt adapter of frost::input
# (frost/ui/key_translation.hpp, frost/ui/ime_tracker.hpp), offscreen; events are
# constructed and translated directly or sent with QCoreApplication::sendEvent, and no
# window is shown.

frost_add_test(ui_input_tests QT
    SOURCES key_translation_test.cpp ime_tracker_test.cpp adapter_integration_test.cpp
    DEPS frost::ui_input frost::input)

if(WIN32)
    # ui_input_native_tests: NativeKeyDelivery on Qt's "windows" platform plugin (ADR 0016
    # decision 4). Key messages are posted to a native window that is created but never
    # shown, and the key state it sets is the test thread's own: no system input, no layout
    # change, no visible window. Label "integration".
    frost_add_test(ui_input_native_tests QT
        SOURCES native_key_delivery_test.cpp
        DEPS frost::ui_input frost::input
        LABELS integration)
    target_compile_definitions(ui_input_native_tests PRIVATE FROST_QT_TEST_PLATFORM="windows")
    # PostMessageW, GetKeyboardState and SetKeyboardState.
    target_link_libraries(ui_input_native_tests PRIVATE user32)
endif()
