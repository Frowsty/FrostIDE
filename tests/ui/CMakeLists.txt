# SPDX-License-Identifier: Apache-2.0
#
# Tests of the UI layer:
#   ui_input_tests               the Qt adapter of frost::input (frost/ui/key_translation.hpp,
#                                frost/ui/ime_tracker.hpp; ADR 0016), offscreen; events are constructed
#                                and translated directly or sent with QCoreApplication::sendEvent, and
#                                no window is shown;
#   ui_input_native_tests        NativeKeyDelivery on the windows platform plugin (Windows, see below);
#   ui_model_tests               frost::ui_model, a non-Qt executable (so a Qt include in a model
#                                header fails to compile), label "unit";
#   ui_model_integration_tests   readWindowsAppearance against reg.exe, label "integration" (Windows);
#   ui_theme_compiler_tests      the .theme.json compiler, the shipped themes, frost-themec and
#                                frost-icon-baker through their command lines (Qt Core);
#   ui_tests                     FrostStyle, AppearanceManager, icons and fonts (offscreen);
#   ui_shell_tests               frost::ui_shell (ADR 0038): the main-window shell, editor tabs,
#                                tool windows, popups, notifications and background tasks over fake
#                                editors and a simulated keyboard layout (offscreen);
#   ui_find_tests                frost::ui_find (ADR 0045): the Find/Replace in Files popup, the Find
#                                tool window and the find actions over a scratch tree (offscreen);
#   ui_screenshot_tests          the SamplePanel goldens and rendered contrast, label "unit" with the
#                                tag "screenshot" (ctest -L screenshot);
#   ui_hidpi_tests               the backing store under Qt's real scaling, also as *.qtscale150;
#   ui_appearance_perf_tests     in-process start-up probes, label "perf";
#   ui_native_tests              the windows platform plugin's palette and fonts (Windows), with
#                                RESOURCE_LOCK frost_desktop (see below).

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

# The fuzz harnesses, linked into the replay tests (and later into libFuzzer targets).
add_library(frost_ui_model_fuzz_harnesses STATIC
    fuzz/icon_atlas_harness.cpp
    fuzz/theme_blob_harness.cpp)
target_link_libraries(frost_ui_model_fuzz_harnesses PUBLIC frost::ui_model)
target_include_directories(frost_ui_model_fuzz_harnesses PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
frost_apply_compile_options(frost_ui_model_fuzz_harnesses TEST)

frost_add_test(ui_model_tests
    SOURCES
        model/fuzz_replay_test.cpp
        model/icon_atlas_test.cpp
        model/registry_test.cpp
        model/scale_settings_test.cpp
        model/theme_blob_test.cpp
    DEPS frost::ui_model frost::io frost_ui_model_fuzz_harnesses)
target_include_directories(ui_model_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/model)
target_compile_definitions(ui_model_tests PRIVATE FROST_SOURCE_DIR="${FROST_SOURCE_DIR}")

if(WIN32)
    frost_add_test(ui_model_integration_tests
        SOURCES model/windows_appearance_test.cpp
        DEPS frost::ui_model frost::io
        LABELS integration)
endif()

add_library(frost_ui_theme_json_harness STATIC fuzz/theme_json_harness.cpp)
target_link_libraries(frost_ui_theme_json_harness PUBLIC frost::ui_theme_compiler)
target_include_directories(frost_ui_theme_json_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
frost_apply_compile_options(frost_ui_theme_json_harness TEST)

frost_add_test(ui_theme_compiler_tests
    SOURCES
        compiler/icon_baker_cli_test.cpp
        compiler/theme_compiler_test.cpp
    DEPS frost::ui_theme_compiler frost::io frost_ui_theme_json_harness
    THIRD_PARTY Qt6::Core)
target_include_directories(ui_theme_compiler_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(ui_theme_compiler_tests PRIVATE
    FROST_SOURCE_DIR="${FROST_SOURCE_DIR}"
    FROST_THEMEC="$<TARGET_FILE:frost-themec>"
    FROST_ICON_BAKER="$<TARGET_FILE:frost-icon-baker>")
add_dependencies(ui_theme_compiler_tests frost-themec frost-icon-baker)

# Qt widget tests. Each gets the built resource directory for its configuration, the source
# tree (goldens, fixtures) and a dependency on the resources, so a test never runs against a
# stale bake.
get_property(_frost_resource_dir GLOBAL PROPERTY FROST_RESOURCE_OUTPUT_DIR)
function(_frost_ui_test_wiring target)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FROST_SOURCE_DIR}/tests)
    target_compile_definitions(${target} PRIVATE
        FROST_RESOURCE_DIR="${_frost_resource_dir}"
        FROST_SOURCE_DIR="${FROST_SOURCE_DIR}")
    add_dependencies(${target} frost_resources)
    # TSan cannot see Qt's internal thread-pool synchronisation (support/tsan_options.cpp).
    if("thread" IN_LIST FROST_SANITIZE)
        target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support/tsan_options.cpp)
    endif()
endfunction()

# Shared by the UI tests here and by m1/shell-frame and m1/editor-view: the appearance
# fixture, the fake OS source, the sample panel and the golden comparator. Not layered.
add_library(frost_ui_test_support STATIC
    support/appearance_fixture.cpp
    support/golden.cpp
    support/sample_panel.cpp)
target_link_libraries(frost_ui_test_support PUBLIC frost::ui frost::io Qt6::Widgets GTest::gtest)
target_include_directories(frost_ui_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(frost_ui_test_support PRIVATE
    FROST_RESOURCE_DIR="${_frost_resource_dir}"
    FROST_SOURCE_DIR="${FROST_SOURCE_DIR}")
frost_apply_compile_options(frost_ui_test_support TEST)
add_dependencies(frost_ui_test_support frost_resources)

frost_add_test(ui_tests QT
    SOURCES
        widgets/appearance_manager_test.cpp
        widgets/fonts_test.cpp
        widgets/icons_test.cpp
        widgets/style_test.cpp
    DEPS frost::ui frost::io frost_ui_test_support)
_frost_ui_test_wiring(ui_tests)

frost_add_test(ui_shell_tests QT
    SOURCES
        shell/editor_tabs_test.cpp
        shell/notifications_test.cpp
        shell/shell_window_test.cpp
    DEPS frost::ui_shell frost::ui frost::input frost::io)
_frost_ui_test_wiring(ui_shell_tests)

frost_add_test(ui_find_tests QT
    SOURCES find/find_ui_test.cpp
    DEPS frost::ui_find frost::ui_shell frost::ui frost::search frost::document frost::sched frost::input frost::io)
_frost_ui_test_wiring(ui_find_tests)

frost_add_test(ui_screenshot_tests QT
    SOURCES screenshot/screenshot_test.cpp
    DEPS frost::ui frost::io frost_ui_test_support
    LABELS unit screenshot)
_frost_ui_test_wiring(ui_screenshot_tests)

# Registered twice: in a dpr-1 process, and with Qt's real scaling at 150% (*.qtscale150).
frost_add_test(ui_hidpi_tests QT
    SOURCES hidpi/hidpi_test.cpp
    DEPS frost::ui frost::io frost_ui_test_support)
_frost_ui_test_wiring(ui_hidpi_tests)
gtest_discover_tests(ui_hidpi_tests
    TEST_SUFFIX .qtscale150
    TEST_FILTER "RealDprCrossCheck.*:HiDpi.*"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS unit ENVIRONMENT "QT_SCALE_FACTOR=1.5;FROST_TEST_QT_SCALE=150")

frost_add_test(ui_appearance_perf_tests QT
    SOURCES perf/appearance_perf_test.cpp
    DEPS frost::ui frost::io frost_ui_test_support
    LABELS perf)
_frost_ui_test_wiring(ui_appearance_perf_tests)

# The windows platform plugin's palette and class fonts against the theme (no widget is shown).
# frost_add_test cannot pass an environment to its PRE_TEST discovery, so this repeats its QT
# steps (qt_test_main, compile options, runtime DLLs, the manifest) with its own discovery.
# Its native windows (never shown) hold RESOURCE_LOCK frost_desktop, as the process-level suites
# do through the integration label's default (frost_add_test's policy; ADR 0027 decision 3), so
# they never overlap with them under parallel ctest.
if(WIN32)
    add_executable(ui_native_tests native/native_test.cpp ${FROST_SOURCE_DIR}/tests/support/qt_test_main.cpp)
    target_link_libraries(ui_native_tests PRIVATE frost::ui frost::io frost_ui_test_support Qt6::Widgets
        GTest::gtest GTest::gmock)
    target_compile_definitions(ui_native_tests PRIVATE FROST_QT_PLUGIN_DIR="${FROST_QT_PLUGIN_DIR}")
    frost_apply_compile_options(ui_native_tests TEST)
    frost_copy_runtime_dlls(ui_native_tests)
    target_sources(ui_native_tests PRIVATE ${FROST_TEST_MANIFEST_RC})
    set_source_files_properties(${FROST_TEST_MANIFEST_RC} PROPERTIES OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
    target_link_options(ui_native_tests PRIVATE /MANIFEST:NO)
    _frost_ui_test_wiring(ui_native_tests)
    gtest_discover_tests(ui_native_tests
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 60
        PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=windows" RESOURCE_LOCK frost_desktop)
endif()
