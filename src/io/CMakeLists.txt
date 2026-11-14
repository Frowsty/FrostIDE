# SPDX-License-Identifier: Apache-2.0
#
# frost::io (PLAN.md section 3.2): the OS boundary for files and processes.
# UTF-8 <-> UTF-16 conversion, path rules (extended-length paths on Windows),
# the IFileSystem and IProcessLauncher seams with their real implementations,
# the MemFs and ScriptedProcessLauncher doubles, the SpawnLogLauncher decorator (FND-8), and the
# asio reactor, async pipes,
# the file watchers, the gitignore matcher and the tree scan (ADR 0020). Qt-free;
# depends on core and sched (watches, pipes and scans resume through IScheduler).
# TODO(M5): IHttpClient (WinHTTP; the libcurl client for Linux follows in M6, PLAN.md section 2.2).

set(frost_io_sources
    src/async_process.cpp
    src/change_coalescer.cpp
    src/file_system.cpp
    src/file_transaction_plan.cpp
    src/file_transaction_prepare.cpp
    src/file_transaction_replace.cpp
    src/ignore_rules.cpp
    src/internal.cpp
    src/loopback_stream.cpp
    src/mem_fs.cpp
    src/mem_fs_watcher.cpp
    src/os_error.cpp
    src/path.cpp
    src/polling_watcher.cpp
    src/process_common.cpp
    src/reactor.cpp
    src/real_file_watcher.cpp
    src/scripted_process_launcher.cpp
    src/spawn_log.cpp
    src/tree_scan.cpp
    src/utf.cpp
    src/watch_queue.cpp)
if(WIN32)
    list(APPEND frost_io_sources src/async_process_win.cpp src/file_system_win.cpp src/file_watcher_win.cpp
                                 src/process_win.cpp src/local_ipc_win.cpp)
else()
    list(APPEND frost_io_sources src/async_process_posix.cpp src/child_reaper_posix.cpp src/file_system_posix.cpp
                                 src/file_watcher_linux.cpp src/process_posix.cpp src/local_ipc_posix.cpp)
endif()

frost_add_library(io LAYER io
    SOURCES ${frost_io_sources}
    DEPS frost::core frost::sched)
# asio (header-only, BSL-1.0; ADR 0020) is an implementation detail: only src/io/src/reactor_impl.hpp
# includes it, so it is linked PRIVATE (frost_add_library's THIRD_PARTY links PUBLIC). Standalone
# (no Boost), without deprecated APIs and without asio's #pragma comment(lib) defaults.
target_link_libraries(frost_io PRIVATE asio::asio)
target_compile_definitions(frost_io PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED ASIO_NO_DEFAULT_LINKED_LIBS)
if(WIN32)
    # BCryptGenRandom: unpredictable pipe names (process_win.cpp).
    target_link_libraries(frost_io PRIVATE bcrypt advapi32)
    # asio's Windows backend references Winsock (ws2_32, mswsock); FrostIDE never uses sockets through
    # it, and reactor.cpp's Winsock guard keeps asio from calling WSAStartup. Delay-loading ws2_32 in
    # every binary that links frost_io keeps the DLL out of the process entirely (ADR 0020; the
    # FileWatcher.WinsockIsNeverLoaded test checks it). INTERFACE: the link options apply to the final
    # executables and DLLs, which is where delay-loading happens.
    target_link_libraries(frost_io INTERFACE ws2_32 delayimp)
    target_link_options(frost_io INTERFACE "LINKER:/DELAYLOAD:ws2_32.dll")
endif()
