# SPDX-License-Identifier: Apache-2.0
#
# frost::diag (PLAN.md section 2, Self-diagnostics; ADR 0022): crash reports through an
# out-of-process crashpad handler, the fatal-path handlers, the logging backend (spdlog), the
# UI-thread stall watchdog with its thread sampler and freeze reports, and local retention of
# everything it writes. Qt-free; the Qt hooks live in src/app (diagnostics_qt.cpp).
#
# crashpad, cpptrace and spdlog are linked PRIVATE: no frost::diag header includes them, so
# their headers and macros stay out of every other module.

set(frost_diag_sources
    src/crash_reporter.cpp
    src/crashpad_store.cpp
    src/fatal_handlers.cpp
    src/freeze_report.cpp
    src/internal.cpp
    src/log_backend.cpp
    src/module_identity.cpp
    src/paths.cpp
    src/retention.cpp
    src/selftest_faults.cpp
    src/stall_watchdog.cpp)
if(WIN32)
    list(APPEND frost_diag_sources
        src/crash_reporter_win.cpp
        src/log_file_win.cpp
        src/module_identity_win.cpp
        src/thread_sampler_win.cpp)
else()
    list(APPEND frost_diag_sources
        src/crash_reporter_posix.cpp
        src/log_file_posix.cpp
        src/module_identity_posix.cpp
        src/thread_sampler_posix.cpp)
endif()

frost_add_library(diag LAYER diag
    SOURCES ${frost_diag_sources}
    DEPS frost::core frost::io)
# spdlog header-only (ADR 0022, D-H): log_backend.cpp is its only includer.
target_link_libraries(frost_diag PRIVATE crashpad::crashpad cpptrace::cpptrace spdlog::spdlog_header_only)
if(WIN32)
    # BCryptGenRandom: the handler's pipe name (crash_reporter_win.cpp).
    target_link_libraries(frost_diag PRIVATE bcrypt)
endif()
# The self-test faults must stay where tools/symbolize.py expects them: no inlining into the
# callers and no tail calls that would drop selfTestFreeze from the samples.
set_source_files_properties(src/selftest_faults.cpp PROPERTIES COMPILE_OPTIONS
    "$<IF:$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>,/clang:-fno-optimize-sibling-calls,-fno-optimize-sibling-calls>")

# The crashpad_handler executable vcpkg built for this triplet.
set(FROST_CRASHPAD_HANDLER_SOURCE
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/crashpad/crashpad_handler${CMAKE_EXECUTABLE_SUFFIX}"
    CACHE INTERNAL "crashpad_handler as vcpkg installed it")
if(NOT EXISTS "${FROST_CRASHPAD_HANDLER_SOURCE}")
    message(FATAL_ERROR "crashpad_handler not found at ${FROST_CRASHPAD_HANDLER_SOURCE} (vcpkg port crashpad)")
endif()
# crashpad's static libraries, for the test that proves clang built them (Toolchain.CrashpadBuiltByClang).
set(FROST_CRASHPAD_LIBRARY_DIR "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" CACHE INTERNAL
    "crashpad's static libraries as vcpkg installed them")

# frost_deploy_crashpad_handler(<target>): copies crashpad_handler next to <target>'s output
# after every build of <target>, which is where frost::diag's users look for it. A plain copy,
# not copy_if_different: that one keeps a stale copy's mode when the contents match.
function(frost_deploy_crashpad_handler target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "${FROST_CRASHPAD_HANDLER_SOURCE}"
                "$<TARGET_FILE_DIR:${target}>/crashpad_handler${CMAKE_EXECUTABLE_SUFFIX}"
        VERBATIM)
endfunction()
