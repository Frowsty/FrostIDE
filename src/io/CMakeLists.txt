# SPDX-License-Identifier: Apache-2.0
#
# frost::io (PLAN.md section 3.2): the OS boundary for files and processes.
# UTF-8 <-> UTF-16 conversion, path rules (extended-length paths on Windows),
# the IFileSystem and IProcessLauncher seams with their real implementations,
# and the MemFs and ScriptedProcessLauncher doubles. Qt-free; depends on core only.
# TODO(M1): the asio reactor (async pipes), file watchers and IHttpClient join here.

set(frost_io_sources
    src/internal.cpp
    src/mem_fs.cpp
    src/os_error.cpp
    src/path.cpp
    src/process_common.cpp
    src/scripted_process_launcher.cpp
    src/utf.cpp)
if(WIN32)
    list(APPEND frost_io_sources src/file_system_win.cpp src/process_win.cpp)
else()
    list(APPEND frost_io_sources src/file_system_posix.cpp src/process_posix.cpp)
endif()

frost_add_library(io LAYER io
    SOURCES ${frost_io_sources}
    DEPS frost::core)
if(WIN32)
    # BCryptGenRandom: unpredictable pipe names (process_win.cpp).
    target_link_libraries(frost_io PRIVATE bcrypt)
endif()
