# SPDX-License-Identifier: Apache-2.0
#
# The io_no_static_init test (PLAN.md section 2.1 start-up hygiene; ADR 0020 decision 1), run with
# cmake -P from tests/cmake. FrostIDE's own code is held to -Wglobal-constructors and
# -Wexit-time-destructors, but asio's headers bring dynamic initialisers those warnings cannot see in
# system headers. This script pins exactly which ones frost_io carries:
#   1. Only the objects listed in kAsioObjects (the translation units that include asio) may have an
#      initialiser section at all: `.CRT$XC[A-Z]` on COFF, `.init_array*` or `.ctors*` on ELF.
#   2. Every function those initialisers call, demangled, must match one of the kExpected patterns
#      below. COFF: the relocation targets of `.CRT$XCU`, and for a translation unit's own
#      `_GLOBAL__sub_I_*` the call targets inside it. ELF: the relocation targets inside
#      `.text.startup` (the per-TU initialiser code), minus the guard variables, `.bss` and
#      __dso_handle, which belong to the same initialisers.
#   3. COFF: the reactor object carries the Winsock guard in `.CRT$XCL` (reactor.cpp), and no other
#      object has a `.CRT$XCL` entry.
# The observed set is printed every time, so a change in asio or in the code shows in the log.
# Registered only when FROST_SANITIZE is empty: instrumentation adds module constructors of its own.
# Inputs (-D): OBJDUMP (llvm-objdump), DEMANGLER (llvm-undname on COFF, llvm-cxxfilt on ELF),
# ARCHIVE (the frost_io static library), FORMAT (COFF or ELF), WORK_DIR (scratch for the symbol list),
# PYTHON_EXECUTABLE (COFF only; the exact optimized Asio lowering recognizer).

# Translation units of frost_io that include asio (through reactor_impl.hpp); file stems.
set(kAsioObjects
    reactor
    real_file_watcher
    polling_watcher
    file_watcher_win
    file_watcher_linux
    async_process
    async_process_win
    async_process_posix
    child_reaper_posix)

# What asio's headers initialise dynamically, as regular expressions over demangled names after the
# `class `, `struct `, quote and backtick noise of MSVC demangling is removed. Each group is justified in
# ADR 0020 ("Static initialisers"):
set(kExpected
    # Service ids: one static `id` object per asio service type the code uses (execution_context's
    # service registry keys). Constructing one stores nothing but its own address; no OS call.
    "service_id<asio::detail::(scheduler|win_iocp_io_context|null_reactor|epoll_reactor|strand_executor_service|strand_service|win_iocp_handle_service|win_object_handle_service|reactive_descriptor_service|deadline_timer_service<.*steady_clock.*>)>"
    "(execution_context_service_base|service_base)<asio::detail::(scheduler|win_iocp_io_context|null_reactor|epoll_reactor|strand_executor_service|strand_service|win_iocp_handle_service|win_object_handle_service|reactive_descriptor_service|deadline_timer_service<.*steady_clock.*>)>::id"
    # Thread-local call stacks (which strand or io_context runs on this thread): one TLS key each,
    # TlsAlloc on Windows and pthread_key_create on Linux.
    "call_stack<asio::detail::(thread_context|strand_executor_service::strand_impl|strand_service::strand_impl), .*>::top_"
    "tss_ptr<asio::detail::call_stack<asio::detail::(thread_context|strand_executor_service::strand_impl|strand_service::strand_impl), .*>::context>"
    # The error categories asio/error.hpp binds to static references (function-local statics inside).
    "asio::error::.*(system|netdb|addrinfo|misc)_category"
    # Windows: each TU's winsock_init_instance, made inert by reactor.cpp's .CRT$XCL guard.
    "asio::detail::.*winsock_init_instance"
    # ELF: the exit-time destructor registrations of the objects above.
    "^__cxa_atexit$"
    # COFF: the Winsock guard's own entry (rule 3).
    "frost::io::.*startWinsockManually")

foreach(input OBJDUMP DEMANGLER ARCHIVE FORMAT WORK_DIR)
    if(NOT ${input})
        message(FATAL_ERROR "check_static_init: -D${input}=... is required")
    endif()
endforeach()

# Runs llvm-objdump with `args` on the archive; the output goes to `out`.
function(frost_objdump out)
    execute_process(COMMAND "${OBJDUMP}" ${ARGN} "${ARCHIVE}"
        OUTPUT_VARIABLE text ERROR_VARIABLE errors RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "llvm-objdump ${ARGN} failed (${rc}): ${errors}")
    endif()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# The stem of an archive member named in a "<archive>(<member>):" header line.
function(frost_member_stem line out)
    string(REGEX MATCH "\\(([^()]*)\\):" match "${line}")
    set(member "${CMAKE_MATCH_1}")
    string(REPLACE "\\" "/" member "${member}")
    get_filename_component(member "${member}" NAME)
    string(REGEX REPLACE "\\.(cpp|cc)\\.(obj|o)$" "" member "${member}")
    set(${out} "${member}" PARENT_SCOPE)
endfunction()

# Splits text into a CMake list of lines (semicolons in the text are replaced first).
function(frost_lines text out)
    string(REPLACE ";" "," text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    set(${out} "${lines}" PARENT_SCOPE)
endfunction()

# --- Rule 1 (and the .CRT$XCL part of rule 3): which objects have initialiser sections. ---
frost_objdump(headers -h)
frost_lines("${headers}" header_lines)
set(member "")
set(with_init "")
set(with_guard "")
foreach(line IN LISTS header_lines)
    if(line MATCHES "file format")
        frost_member_stem("${line}" member)
        continue()
    endif()
    if(FORMAT STREQUAL "COFF")
        if(line MATCHES "[ \t]\\.CRT\\$XC[A-Z]")
            list(APPEND with_init "${member}")
        endif()
        if(line MATCHES "[ \t]\\.CRT\\$XCL")
            list(APPEND with_guard "${member}")
        endif()
    elseif(line MATCHES "[ \t]\\.(init_array|ctors)")
        list(APPEND with_init "${member}")
    endif()
endforeach()
list(REMOVE_DUPLICATES with_init)
list(REMOVE_DUPLICATES with_guard)
list(SORT with_init)
message(STATUS "objects with initialisers: ${with_init}")
set(failures "")
foreach(object IN LISTS with_init)
    if(NOT object IN_LIST kAsioObjects)
        list(APPEND failures "${object} has a static initialiser but does not include asio (rule 1)")
    endif()
endforeach()
if(FORMAT STREQUAL "COFF")
    message(STATUS "objects with a .CRT$XCL entry: ${with_guard}")
    if(NOT with_guard STREQUAL "reactor")
        list(APPEND failures "exactly reactor.cpp must carry the .CRT$XCL Winsock guard, found: '${with_guard}' (rule 3)")
    endif()
endif()

# --- Rule 2: what the initialisers call. ---
set(symbols "")
if(FORMAT STREQUAL "COFF")
    frost_objdump(relocations -r)
    frost_lines("${relocations}" relocation_lines)
    set(section "")
    set(tu_initialisers "")
    set(guard_records "")
    set(guard_targets "")
    set(initializer_records "")
    set(member "")
    foreach(line IN LISTS relocation_lines)
        if(line MATCHES "file format")
            frost_member_stem("${line}" member)
            set(section "")
        elseif(line MATCHES "RELOCATION RECORDS FOR \\[([^]]*)\\]")
            set(section "${CMAKE_MATCH_1}")
        elseif(section MATCHES "^\\.CRT\\$XC[A-Z]$" AND line MATCHES "IMAGE_REL_[A-Z0-9_]+[ \t]+([^ \t]+)")
            set(target "${CMAKE_MATCH_1}")
            string(REGEX MATCH "^[ \t]*([0-9a-fA-F]+)[ \t]+(IMAGE_REL_[A-Z0-9_]+)" relocation "${line}")
            set(record "${member}|${section}|${CMAKE_MATCH_2}|${CMAKE_MATCH_1}|${target}")
            if(section STREQUAL ".CRT$XCL")
                list(APPEND guard_records "--guard=${record}")
                list(APPEND guard_targets "${target}")
            endif()
            if(target MATCHES "^_GLOBAL__sub_I_")
                list(APPEND tu_initialisers "${target}")
                list(APPEND initializer_records "--initializer=${record}")
            else()
                list(APPEND symbols "${target}")
            endif()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES tu_initialisers)
    # Release inlines the already permitted winsock_init_instance into the TU initializer.
    # Recognize that complete, pinned machine-code body only with its exact earlier manual guard;
    # never allow WSAStartup or atexit generally. Unknown instructions/relocations still reach rule 2.
    set(disassemble_targets ${tu_initialisers} ${guard_targets})
    if(disassemble_targets)
        list(JOIN disassemble_targets "," wanted)
        frost_objdump(disassembly -d -r "--disassemble-symbols=${wanted}")
        if(NOT PYTHON_EXECUTABLE)
            message(FATAL_ERROR "check_static_init: -DPYTHON_EXECUTABLE=... is required for COFF")
        endif()
        set(disassembly_file "${WORK_DIR}/check_static_init_coff_disassembly.txt")
        file(WRITE "${disassembly_file}" "${disassembly}")
        execute_process(
            COMMAND "${PYTHON_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/static_init_coff.py"
                "--disassembly=${disassembly_file}" ${guard_records} ${initializer_records}
            OUTPUT_VARIABLE recognized_json ERROR_VARIABLE recognition_errors RESULT_VARIABLE recognition_rc)
        if(NOT recognition_rc EQUAL 0)
            message(FATAL_ERROR "COFF initializer recognition failed: ${recognition_errors}")
        endif()
        string(JSON recognized_count ERROR_VARIABLE json_error LENGTH "${recognized_json}")
        if(json_error)
            message(FATAL_ERROR "COFF initializer recognizer returned invalid JSON: ${json_error}")
        endif()
        set(recognized "")
        if(recognized_count GREATER 0)
            math(EXPR recognized_last "${recognized_count} - 1")
            foreach(index RANGE ${recognized_last})
                string(JSON key GET "${recognized_json}" ${index})
                list(APPEND recognized "${key}")
                message(STATUS "  recognized guarded Asio Winsock initializer: ${key}")
            endforeach()
            list(APPEND symbols "??__Ewinsock_init_instance@detail@asio@@YAXXZ")
        endif()
        frost_lines("${disassembly}" disassembly_lines)
        set(member "")
        set(function "")
        foreach(line IN LISTS disassembly_lines)
            if(line MATCHES "file format")
                frost_member_stem("${line}" member)
                set(function "")
            elseif(line MATCHES "^[0-9a-fA-F]+ <([^>]*)>:$")
                set(function "${CMAKE_MATCH_1}")
            elseif(function IN_LIST tu_initialisers AND line MATCHES "IMAGE_REL_[A-Z0-9_]+[ \t]+([^ \t]+)")
                set(target "${CMAKE_MATCH_1}")
                set(key "${member}|${function}")
                if(NOT key IN_LIST recognized)
                    list(APPEND symbols "${target}")
                endif()
            endif()
        endforeach()
    endif()
else()
    frost_objdump(disassembly -d -r --section=.text.startup)
    frost_lines("${disassembly}" disassembly_lines)
    foreach(line IN LISTS disassembly_lines)
        if(line MATCHES "R_X86_64_[A-Z0-9_]+[ \t]+([^ \t]+)")
            string(REGEX REPLACE "[-+]0x[0-9a-f]+$" "" target "${CMAKE_MATCH_1}")
            # The initialisers' own bookkeeping: guard variables, zero-initialised storage, the DSO handle.
            if(target MATCHES "^(_ZGV|\\.bss|__dso_handle$)")
                continue()
            endif()
            list(APPEND symbols "${target}")
        endif()
    endforeach()
endif()
list(REMOVE_DUPLICATES symbols)

# Demangles every symbol at once (one name per line in, one per line out).
string(REPLACE ";" "\n" mangled_text "${symbols}")
set(mangled_file "${WORK_DIR}/check_static_init_symbols.txt")
file(WRITE "${mangled_file}" "${mangled_text}\n")
execute_process(COMMAND "${DEMANGLER}" INPUT_FILE "${mangled_file}"
    OUTPUT_VARIABLE demangled_text ERROR_VARIABLE demangle_errors RESULT_VARIABLE rc)
frost_lines("${demangled_text}" demangled_lines)
# llvm-undname echoes each input line, then prints its demangled form or "error: ..." (the raw name is
# then kept); llvm-cxxfilt prints one line per input line, unmangled names unchanged.
set(observed "")
set(pending "")
foreach(line IN LISTS demangled_lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    if(FORMAT STREQUAL "COFF")
        if(line MATCHES "^error:")
            if(pending)
                list(APPEND observed "${pending}")
            endif()
            set(pending "")
            continue()
        endif()
        if(line MATCHES "^[?]")
            set(pending "${line}")
            continue()
        endif()
        set(pending "")
    endif()
    string(REGEX REPLACE "(class|struct|enum|union) " "" line "${line}")
    string(REGEX REPLACE "[`']" "" line "${line}")
    string(REPLACE "void __cdecl " "" line "${line}")
    string(REPLACE "dynamic initializer for " "" line "${line}")
    string(REGEX REPLACE "(public|private|protected): static " "" line "${line}")
    list(APPEND observed "${line}")
endforeach()
if(pending)
    list(APPEND observed "${pending}")
endif()
list(REMOVE_DUPLICATES observed)
list(SORT observed)
list(LENGTH observed observed_count)
message(STATUS "initialiser callees (${observed_count}):")
foreach(name IN LISTS observed)
    set(matched FALSE)
    foreach(pattern IN LISTS kExpected)
        if(name MATCHES "${pattern}")
            set(matched TRUE)
            break()
        endif()
    endforeach()
    if(matched)
        message(STATUS "  expected: ${name}")
    else()
        message(STATUS "  UNEXPECTED: ${name}")
        list(APPEND failures "unexpected static initialiser: ${name} (rule 2; add it to kExpected only with an ADR 0020 entry)")
    endif()
endforeach()
if(observed_count EQUAL 0 AND with_init)
    list(APPEND failures "objects have initialiser sections but no callee was found: the parser is out of date")
endif()

if(failures)
    list(JOIN failures "\n  " text)
    message(FATAL_ERROR "io_no_static_init failed:\n  ${text}")
endif()
message(STATUS "io_no_static_init: ok")
