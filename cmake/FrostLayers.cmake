# SPDX-License-Identifier: Apache-2.0
#
# FrostLayers.cmake: target helpers that enforce the module layering at configure
# time and give executables their runtime DLLs. The layer ranks below are the source
# of truth for the dependency order of the modules listed in PLAN.md section 3.2; the
# Qt rule is PLAN.md section 2 ("Process model and IPC") and section 6.5.
#
# Every FrostIDE library belongs to a layer with a rank. A library may depend
# only on libraries of a strictly lower rank, or on libraries of its own layer.
# Only the "ui" and "app" layers may link Qt. A violation fails configure, so
# layering mistakes never reach a build.
#
# Two passes enforce this: each helper call checks its declared DEPS and
# THIRD_PARTY lists, and a deferred pass at the end of configure walks every
# layered target's final link graph, so links added later with a raw
# target_link_libraries, or Qt reached through a wrapper target, are caught too.
# Setting FROST_LAYERS_REPORT_ALL reports violations with SEND_ERROR instead of
# FATAL_ERROR, so tests/cmake can see every diagnostic of a fixture in one run.
#
# The file also makes executables runnable from the build tree on Windows:
# frost_copy_runtime_dlls registers an executable, and a second deferred pass
# (_frost_add_runtime_dll_copies) gives each output directory that holds a registered
# executable one target that copies the runtime DLLs of all its executables and orders
# every executable there after it. Its output-directory checks run on every platform.
#
#   frost_add_library(<name> LAYER <layer> [SOURCES ...] [DEPS frost libs...]
#                     [THIRD_PARTY imported targets...] [INTERFACE | SHARED])
#   frost_add_executable(<name> LAYER <layer> [WIN32] [NO_SANITIZE] [OUTPUT_NAME <file name>]
#                        SOURCES ... [DEPS ...] [THIRD_PARTY ...])
#   frost_add_test(<name> SOURCES ... [DEPS ...] [THIRD_PARTY ...] [LABELS ...] [QT])
#   frost_copy_runtime_dlls(<executable>)   (frost_add_executable and frost_add_test call it)

include_guard(GLOBAL)
include(FrostCompilerOptions)

# Layer ranks, lowest first. Libraries of equal rank but different layers may
# not depend on each other.
set(FROST_LAYER_RANK_compat 0)
set(FROST_LAYER_RANK_core 1)
set(FROST_LAYER_RANK_sched 2)
set(FROST_LAYER_RANK_io 3)
set(FROST_LAYER_RANK_text 4)
set(FROST_LAYER_RANK_storage 4)
set(FROST_LAYER_RANK_rpc 4)
# input (src/input, frost::input): not in PLAN.md section 3.2 yet; ADR 0016 decision 1 records it.
set(FROST_LAYER_RANK_input 4)
set(FROST_LAYER_RANK_syntax 5)
set(FROST_LAYER_RANK_settings 5)
set(FROST_LAYER_RANK_protocols 5)
set(FROST_LAYER_RANK_project 6)
# document (src/document, frost::document): the SaveManager and the document load service. Not in
# PLAN.md section 3.2 (which lists SaveManager under src/text); it links text, diff, storage and
# settings, so it sits above settings (rank 5). ADR 0028 decision 1 records it.
set(FROST_LAYER_RANK_document 6)
set(FROST_LAYER_RANK_lang 7)
set(FROST_LAYER_RANK_build 7)
set(FROST_LAYER_RANK_debug 7)
set(FROST_LAYER_RANK_vcs 7)
set(FROST_LAYER_RANK_term 7)
set(FROST_LAYER_RANK_search 7)
set(FROST_LAYER_RANK_test_runner 7)
set(FROST_LAYER_RANK_agent 8)
set(FROST_LAYER_RANK_diag 8)
set(FROST_LAYER_RANK_update 8)
set(FROST_LAYER_RANK_ui 9)
set(FROST_LAYER_RANK_app 10)

# Reports a layering violation: FATAL_ERROR normally, SEND_ERROR (configure
# continues, generation fails) when FROST_LAYERS_REPORT_ALL is set.
function(_frost_layer_error)
    if(FROST_LAYERS_REPORT_ALL)
        message(SEND_ERROR ${ARGN})
    else()
        message(FATAL_ERROR ${ARGN})
    endif()
endfunction()

# Sets <out> to TRUE if a target of <layer> may depend on one of <dep_layer>.
function(_frost_layer_allows layer dep_layer out)
    set(allowed FALSE)
    if(dep_layer STREQUAL layer OR FROST_LAYER_RANK_${dep_layer} LESS FROST_LAYER_RANK_${layer})
        set(allowed TRUE)
    endif()
    set(${out} ${allowed} PARENT_SCOPE)
endfunction()

# Sets <out> to the real target behind <name> (resolving ALIAS targets), or to
# an empty string when <name> is not a target.
function(_frost_resolve_target name out)
    set(real "")
    if(TARGET "${name}")
        get_target_property(real "${name}" ALIASED_TARGET)
        if(NOT real)
            set(real "${name}")
        endif()
    endif()
    set(${out} "${real}" PARENT_SCOPE)
endfunction()

# Fails configure unless every dependency of <target> (in <layer>) is allowed.
function(_frost_check_layering target layer deps third_party)
    if(NOT DEFINED FROST_LAYER_RANK_${layer})
        message(FATAL_ERROR "${target}: unknown layer '${layer}' (see cmake/FrostLayers.cmake)")
    endif()
    set(rank ${FROST_LAYER_RANK_${layer}})
    foreach(dep IN LISTS deps)
        if(NOT TARGET ${dep})
            message(FATAL_ERROR "${target}: dependency '${dep}' is not a target (declare it before use)")
        endif()
        get_target_property(dep_layer ${dep} FROST_LAYER)
        if(NOT dep_layer)
            _frost_layer_error("${target}: '${dep}' is not a FrostIDE library; list it under THIRD_PARTY")
            continue()
        endif()
        _frost_layer_allows(${layer} ${dep_layer} allowed)
        if(NOT allowed)
            _frost_layer_error(
                "layering violation: ${target} (layer ${layer}, rank ${rank}) may not depend on "
                "${dep} (layer ${dep_layer}, rank ${FROST_LAYER_RANK_${dep_layer}})")
        endif()
    endforeach()
    foreach(lib IN LISTS third_party)
        # FrostIDE libraries listed as THIRD_PARTY would skip the rank check above.
        _frost_resolve_target("${lib}" real)
        set(lib_layer "")
        if(real)
            get_target_property(lib_layer "${real}" FROST_LAYER)
        endif()
        if(lib_layer OR lib MATCHES "^frost(::|_)")
            _frost_layer_error("${target}: '${lib}' is a FrostIDE library; list it under DEPS, not THIRD_PARTY")
        elseif(lib MATCHES "^Qt6?::" AND NOT layer MATCHES "^(ui|app)$")
            _frost_layer_error("layering violation: ${target} (layer ${layer}) may not link Qt (${lib})")
        endif()
    endforeach()
endfunction()

# Deferred whole-graph check (runs once, after every CMakeLists.txt has been
# processed). For each layered target it walks LINK_LIBRARIES and
# INTERFACE_LINK_LIBRARIES; non-FrostIDE targets (wrappers, imported targets)
# are followed through their INTERFACE_LINK_LIBRARIES, FrostIDE targets are
# rank-checked (their own edges are checked when they are the owner), and any
# Qt target reached from outside ui/app is a violation. Generator expressions
# other than $<LINK_ONLY:...> are not evaluated and are skipped.
function(_frost_check_layer_graph)
    get_property(targets GLOBAL PROPERTY FROST_LAYERED_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(layer ${target} FROST_LAYER)
        set(queue "")
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(libs ${target} ${property})
            if(libs)
                list(APPEND queue ${libs})
            endif()
        endforeach()
        set(seen "")
        while(queue)
            list(POP_FRONT queue item)
            if(item MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
                set(item "${CMAKE_MATCH_1}")
            endif()
            if(item MATCHES "^\\$<" OR item IN_LIST seen)
                continue()
            endif()
            list(APPEND seen "${item}")
            # Qt is recognised by name before resolving: this check runs in the
            # top-level scope, where an imported target created in a subdirectory
            # (find_package there without GLOBAL) cannot be resolved.
            if(item MATCHES "^Qt6?::")
                if(NOT layer MATCHES "^(ui|app)$")
                    _frost_layer_error("layering violation: ${target} (layer ${layer}) reaches Qt (${item}) "
                                       "through its link graph")
                endif()
                continue()
            endif()
            _frost_resolve_target("${item}" real)
            if(NOT real)
                # Plain names (shell32, pthread) are system libraries. A namespaced
                # name that does not resolve here hides its link graph from the check.
                if(item MATCHES "::")
                    _frost_layer_error("${target} links ${item}, which is not visible from the top-level directory, "
                                       "so its link graph cannot be checked; find the package in the root "
                                       "CMakeLists.txt or import it GLOBAL")
                endif()
                continue()
            endif()
            # `seen` already holds `item`; an alias or other name for a target visited
            # under its real name is skipped here.
            if(NOT real STREQUAL item)
                if(real IN_LIST seen)
                    continue()
                endif()
                list(APPEND seen "${real}")
            endif()
            if(real STREQUAL target)
                continue()
            endif()
            if(real MATCHES "^Qt6?::")
                if(NOT layer MATCHES "^(ui|app)$")
                    _frost_layer_error("layering violation: ${target} (layer ${layer}) reaches Qt (${item}) "
                                       "through its link graph")
                endif()
                continue()
            endif()
            get_target_property(dep_layer "${real}" FROST_LAYER)
            if(dep_layer)
                _frost_layer_allows(${layer} ${dep_layer} allowed)
                if(NOT allowed)
                    _frost_layer_error("layering violation: ${target} (layer ${layer}) links ${real} "
                                       "(layer ${dep_layer}) in its final link graph")
                endif()
                continue()
            endif()
            get_target_property(libs "${real}" INTERFACE_LINK_LIBRARIES)
            if(libs)
                list(APPEND queue ${libs})
            endif()
        endwhile()
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _frost_check_layer_graph)

# Creates FrostIDE library frost_<name> (alias frost::<name>) in <layer>: STATIC
# from SOURCES with PRODUCTION options, or with INTERFACE a header-only INTERFACE
# library (no compile options of its own), or with SHARED a shared library (a DLL on
# Windows; today only frost_app.dll, ADR 0012), which _frost_add_runtime_dll_copies orders
# after its output directory's DLL copy. DEPS (FrostIDE libraries, rank-checked)
# and THIRD_PARTY (imported targets) are linked PUBLIC/INTERFACE; the target is
# recorded in FROST_LAYERED_TARGETS for the deferred graph check.
function(frost_add_library name)
    cmake_parse_arguments(ARG "INTERFACE;SHARED" "LAYER" "SOURCES;DEPS;THIRD_PARTY" ${ARGN})
    set(target frost_${name})
    _frost_check_layering(${target} "${ARG_LAYER}" "${ARG_DEPS}" "${ARG_THIRD_PARTY}")
    if(ARG_INTERFACE AND ARG_SHARED)
        message(FATAL_ERROR "frost_add_library(${name}): INTERFACE and SHARED exclude each other")
    endif()
    if(ARG_INTERFACE)
        add_library(${target} INTERFACE)
        target_compile_features(${target} INTERFACE cxx_std_26)
        target_include_directories(${target} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(${target} INTERFACE ${ARG_DEPS} ${ARG_THIRD_PARTY})
    else()
        if(ARG_SHARED)
            add_library(${target} SHARED ${ARG_SOURCES})
        else()
            add_library(${target} STATIC ${ARG_SOURCES})
        endif()
        target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(${target} PUBLIC ${ARG_DEPS} ${ARG_THIRD_PARTY})
        frost_apply_compile_options(${target} PRODUCTION)
    endif()
    set_target_properties(${target} PROPERTIES FROST_LAYER ${ARG_LAYER})
    set_property(GLOBAL APPEND PROPERTY FROST_LAYERED_TARGETS ${target})
    add_library(frost::${name} ALIAS ${target})
endfunction()

# Creates executable <name> in <layer> with PRODUCTION options; DEPS and THIRD_PARTY
# are checked as for frost_add_library and linked PRIVATE. WIN32 selects the GUI
# subsystem; NO_SANITIZE leaves out FROST_SANITIZE's flags (frost.exe's Windows launcher
# only, see frost_apply_compile_options); OUTPUT_NAME renames the produced file. The
# executable gets its runtime DLLs through frost_copy_runtime_dlls, like every test of
# frost_add_test.
function(frost_add_executable name)
    cmake_parse_arguments(ARG "WIN32;NO_SANITIZE" "LAYER;OUTPUT_NAME" "SOURCES;DEPS;THIRD_PARTY" ${ARGN})
    _frost_check_layering(${name} "${ARG_LAYER}" "${ARG_DEPS}" "${ARG_THIRD_PARTY}")
    if(ARG_WIN32)
        add_executable(${name} WIN32 ${ARG_SOURCES})
    else()
        add_executable(${name} ${ARG_SOURCES})
    endif()
    target_link_libraries(${name} PRIVATE ${ARG_DEPS} ${ARG_THIRD_PARTY})
    if(ARG_NO_SANITIZE)
        frost_apply_compile_options(${name} PRODUCTION NO_SANITIZE)
    else()
        frost_apply_compile_options(${name} PRODUCTION)
    endif()
    set_target_properties(${name} PROPERTIES FROST_LAYER ${ARG_LAYER})
    set_property(GLOBAL APPEND PROPERTY FROST_LAYERED_TARGETS ${name})
    if(ARG_OUTPUT_NAME)
        set_target_properties(${name} PROPERTIES OUTPUT_NAME ${ARG_OUTPUT_NAME})
    endif()
    frost_copy_runtime_dlls(${name})
endfunction()

# Makes executable <target> run from the build tree without PATH edits. On Windows,
# the DLLs of the imported shared libraries it links (Qt; $<TARGET_RUNTIME_DLLS>) and,
# in ASan builds, clang's ASan runtime DLL (FROST_ASAN_RUNTIME_DLL, set by the root
# CMakeLists.txt; not on PATH) are copied into its output directory. Elsewhere nothing
# is copied, but the preconditions below are still checked. frost_add_executable and
# frost_add_test call it for every executable they create; an executable made with a
# plain add_executable (io_test_child) calls it itself.
#
# The rule: no build step may read or write a copied DLL in an output directory unless
# it is ordered after that directory's copy. Every executable in the directory has
# such a step: vcpkg's toolchain gives each executable a z-applocal POST_BUILD step
# (its add_executable override; VCPKG_APPLOCAL_DEPS) that opens every DLL the
# executable imports from beside it, recursively, and would copy missing ones from
# vcpkg's installed bin directory. That directory does not exist while the triplets
# link statically (VCPKG_LIBRARY_LINKAGE static), so z-applocal copies nothing today,
# but it reads the copied Qt and ASan DLLs on every run. That read is what failed in
# the first verify of the merged tree (87f376a, the merge of m0/s1), in its fresh
# tests/perf/s1 directory: vcpkg printed "open_for_read(...Qt6Widgetsd.dll): permission
# denied" when one executable's z-applocal opened the DLL while the other executable's
# POST_BUILD copy was writing it, a reader against a writer (51e9a05, the stopgap that
# ordered that pair, calls it two writers). So one custom target per output
# directory makes the copies (_frost_add_runtime_dll_copies, at the end of configure),
# never a step per executable, and every executable in the directory, registered here
# or not, is ordered after it: its compiles and its link wait for the copy, so the
# DLLs are in place before it links and before its z-applocal step runs, also when it
# is built on its own. Shared and module libraries built here (frost_app.dll, ADR 0012)
# are members of their DLL's directory in the same way: vcpkg's add_library override gives
# each one a z-applocal step too, which reads the Qt DLLs it imports from beside it, so each
# is ordered after its directory's copy, and its imported DLLs join that copy. The copy
# never writes a DLL built here; that DLL's only writer is its own link step, and no
# z-applocal step reads it unless an executable beside it imports it (frost.exe loads
# frost_app.dll at run time and depends on its target, so its own step runs after the link).
#
# Preconditions: <target> is an executable built here, not an IMPORTED or ALIAS
# target. Its runtime output directory, like that of every executable and shared or module
# library in the build, is the default one or a plain RUNTIME_OUTPUT_DIRECTORY: no
# generator expression, no RUNTIME_OUTPUT_DIRECTORY_<CONFIG> (checked at the end of
# configure). The copied DLLs come from imported targets: nothing links a shared library
# built here (frost.exe loads frost_app.dll at run time), because $<TARGET_RUNTIME_DLLS>
# would then list it and the copy, which waits for no link, would copy it.
function(frost_copy_runtime_dlls target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "frost_copy_runtime_dlls: '${target}' is not a target")
    endif()
    get_target_property(aliased ${target} ALIASED_TARGET)
    get_target_property(imported ${target} IMPORTED)
    if(aliased OR imported)
        message(FATAL_ERROR "frost_copy_runtime_dlls: ${target} is an ALIAS or IMPORTED target; "
                            "name the executable built here")
    endif()
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "frost_copy_runtime_dlls: ${target} is a ${type}, not an executable")
    endif()
    set_property(GLOBAL APPEND PROPERTY FROST_RUNTIME_DLL_TARGETS ${target})
endfunction()

# Deferred (end of configure, once the output-directory properties are final). Finds
# every executable and every shared or module library of the build through each
# directory's BUILDSYSTEM_TARGETS and groups them by runtime output directory (where a
# DLL goes on Windows). Each directory that holds a target registered by
# frost_copy_runtime_dlls gets one custom target, frost_runtime_dlls_<directory>
# (<directory> is the output directory relative to the build tree as a C identifier,
# e.g. frost_runtime_dlls_tests_perf_s1; the build tree's own is plain
# frost_runtime_dlls, and a clashing name gets a numeric suffix), that copies the
# union of the group's imported DLLs into the directory, and every member of the
# group, registered or not, depends on it. The search keeps the rule above when a
# registration is missing (s1_probe_validate and s1_first_frame had none and shared
# tests/perf/s1 with s1_editor), instead of relying on every call site. Multi-config
# generators run the copy once per configuration, into that configuration's
# subdirectory, so there is exactly one copy step per directory and configuration.
# The copy target always runs, so DLLs deleted from a directory come back with the next
# build. copy_if_different compares contents and leaves current DLLs alone; that costs
# an up-to-date build one cmake -E process per directory, in parallel: about 90 ms
# where the three Qt Debug DLLs are compared, 15 ms elsewhere (measured 2026-11-01).
# Budget: B28. The old POST_BUILD copy paid the same after every relink of each
# executable. copy_if_newer would be cheaper but would keep a newer DLL when Qt is
# downgraded. Windows only, after the checks.
function(_frost_add_runtime_dll_copies)
    get_property(registered GLOBAL PROPERTY FROST_RUNTIME_DLL_TARGETS)
    # Every executable and shared or module library built here, directory by directory
    # from the top. Imported and alias targets are not in BUILDSYSTEM_TARGETS, and
    # frost_copy_runtime_dlls rejects them, so every registered target is in this list.
    set(executables "")
    set(pending "${CMAKE_SOURCE_DIR}")
    while(pending)
        list(POP_FRONT pending source_dir)
        get_property(subdirs DIRECTORY "${source_dir}" PROPERTY SUBDIRECTORIES)
        list(APPEND pending ${subdirs})
        get_property(dir_targets DIRECTORY "${source_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS dir_targets)
            get_target_property(type ${target} TYPE)
            if(type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
                list(APPEND executables ${target})
            endif()
        endforeach()
    endwhile()
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        set(configs ${CMAKE_CONFIGURATION_TYPES})
    else()
        set(configs ${CMAKE_BUILD_TYPE})
    endif()
    # dirs[i] is a group's base output directory (before any per-configuration
    # subdirectory); group_<i> lists its executables, and copy_<i> is set when one of
    # them is registered. Every executable is checked, registered or not: one whose
    # directory cannot be told could land beside a copy without being ordered after it.
    set(dirs "")
    foreach(target IN LISTS executables)
        foreach(config IN LISTS configs)
            string(TOUPPER "${config}" config)
            get_target_property(per_config ${target} RUNTIME_OUTPUT_DIRECTORY_${config})
            if(per_config)
                message(FATAL_ERROR "frost_copy_runtime_dlls: ${target} sets RUNTIME_OUTPUT_DIRECTORY_${config}, "
                                    "which the per-directory DLL copy does not support (cmake/FrostLayers.cmake)")
            endif()
        endforeach()
        get_target_property(binary_dir ${target} BINARY_DIR)
        get_target_property(dir ${target} RUNTIME_OUTPUT_DIRECTORY)
        if(NOT dir)
            set(dir "${binary_dir}")
        elseif(dir MATCHES "\\$<")
            message(FATAL_ERROR "frost_copy_runtime_dlls: ${target}'s RUNTIME_OUTPUT_DIRECTORY uses a generator "
                                "expression, which the per-directory DLL copy does not support")
        endif()
        # CMake resolves a relative output directory against the target's binary
        # directory. A trailing slash is dropped so that both spellings share a group.
        cmake_path(ABSOLUTE_PATH dir BASE_DIRECTORY "${binary_dir}" NORMALIZE)
        string(REGEX REPLACE "([^:/])/$" "\\1" dir "${dir}")
        list(FIND dirs "${dir}" index)
        if(index EQUAL -1)
            list(LENGTH dirs index)
            list(APPEND dirs "${dir}")
        endif()
        list(APPEND group_${index} ${target})
        if(target IN_LIST registered)
            set(copy_${index} TRUE)
        endif()
    endforeach()
    if(NOT WIN32)
        return()
    endif()

    set(index 0)
    foreach(dir IN LISTS dirs)
        set(members ${group_${index}})
        set(copy ${copy_${index}})
        math(EXPR index "${index} + 1")
        if(NOT copy)
            continue()
        endif()
        file(RELATIVE_PATH rel "${CMAKE_BINARY_DIR}" "${dir}")
        set(name frost_runtime_dlls)
        if(rel)
            string(MAKE_C_IDENTIFIER "${name}_${rel}" name)
        else()
            set(rel ".")
        endif()
        if(TARGET ${name})
            set(name "${name}_${index}")
        endif()
        # The union of the DLLs of every executable in the directory, registered or not,
        # so that none of them needs a copy of its own, as one generator expression; it
        # holds `;`, so it stays quoted, and COMMAND_EXPAND_LISTS turns its value into
        # arguments.
        set(dlls "")
        foreach(target IN LISTS members)
            list(APPEND dlls "$<TARGET_RUNTIME_DLLS:${target}>")
        endforeach()
        set(dlls "$<REMOVE_DUPLICATES:${dlls}>")
        # All members share the directory; $<TARGET_FILE_DIR> adds its configuration
        # subdirectory. Target-file component expressions such as $<TARGET_FILE_DIR>
        # add no target dependency (CMP0112, NEW under cmake_minimum_required(VERSION
        # 4.0)), and $<TARGET_RUNTIME_DLLS> adds none either, so naming the members
        # here does not make the copy wait for them.
        list(GET members 0 first)
        if(FROST_ASAN_RUNTIME_DLL)
            set(verb copy_if_different)
        else()
            # `cmake -E true` swallows the arguments when there is nothing to copy,
            # because copy_if_different with only a destination is an error.
            set(verb "$<IF:$<BOOL:${dlls}>,copy_if_different,true>")
        endif()
        add_custom_target(${name}
            COMMAND ${CMAKE_COMMAND} -E "${verb}" ${FROST_ASAN_RUNTIME_DLL} "${dlls}" "$<TARGET_FILE_DIR:${first}>"
            COMMAND_EXPAND_LISTS
            VERBATIM
            COMMENT "Copying runtime DLLs for ${rel}")
        foreach(target IN LISTS members)
            add_dependencies(${target} ${name})
        endforeach()
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _frost_add_runtime_dll_copies)

# Test executables sit outside the layer graph (they may use any library) and
# are registered with CTest through gtest_discover_tests.
#
# QT marks tests that need Qt at runtime. They link tests/support/qt_test_main.cpp
# instead of gtest_main: it selects the offscreen platform and Qt's plugin
# directory before creating the QApplication, so ctest needs no environment setup.
# LABELS is the test kind, exactly one of unit, integration, stress or perf (default unit).
# The module label (mod.<directory>) comes from the test directory: tests/CMakeLists.txt adds
# each directory through frost_add_test_directory, which sets CMAKE_DIRECTORY_LABELS. A second
# label here cannot work: gtest_discover_tests writes every PROPERTIES list element as its own
# bracket argument (CMake 4.4.5's GoogleTestAddTests.cmake), so "unit;x" would become a property
# named x, which is why more than one label is a configure error.
# On Windows the test embeds the FrostIDE manifest (test_manifest.rc) and the linker's own
# manifest is disabled. Tests are built with TEST options and, like the executables of
# frost_add_executable, get their runtime DLLs through frost_copy_runtime_dlls.
#
# Scheduling for parallel ctest (verify.ps1 runs ctest -j; ADR 0027 decision 3). The kind label
# sets the default, so a suite added later is kept apart without further wiring:
#   integration  RESOURCE_LOCK frost_desktop: process-level tests start frost.exe or console helpers,
#                create private desktops and windows, or touch per-user state, and the executables
#                holding the lock never overlap;
#   perf         RUN_SERIAL: they measure wall-clock latency (asserted in optimised builds);
#   unit, stress no default.
# Keywords, applied to every test the executable's discovery adds (so they cover the executable):
#   RUN_SERIAL            nothing else runs beside these tests;
#   RESOURCE_LOCK <name>  tests holding the same name never overlap (replaces the kind's default);
#   PROCESSORS <n>        each test counts as n of ctest's -j slots (many threads, a nested build).
# A unit suite that needs the same isolation (a private desktop, MessageBoxW, a native window)
# says RESOURCE_LOCK frost_desktop; a test added with add_test or gtest_discover_tests directly
# sets these properties itself.
function(frost_add_test name)
    cmake_parse_arguments(ARG "QT;RUN_SERIAL" "RESOURCE_LOCK;PROCESSORS" "SOURCES;DEPS;THIRD_PARTY;LABELS" ${ARGN})
    list(LENGTH ARG_LABELS label_count)
    if(label_count GREATER 1)
        message(FATAL_ERROR "frost_add_test(${name}): LABELS takes one kind label (got '${ARG_LABELS}'); "
                            "the module label comes from frost_add_test_directory (tests/CMakeLists.txt)")
    endif()
    if(ARG_LABELS AND NOT ARG_LABELS MATCHES "^(unit|integration|stress|perf)$")
        message(FATAL_ERROR "frost_add_test(${name}): LABELS is unit, integration, stress or perf (got '${ARG_LABELS}')")
    endif()
    if(DEFINED ARG_PROCESSORS AND NOT ARG_PROCESSORS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "frost_add_test(${name}): PROCESSORS needs a positive integer (got '${ARG_PROCESSORS}')")
    endif()
    if(ARG_QT)
        add_executable(${name} ${ARG_SOURCES} ${FROST_SOURCE_DIR}/tests/support/qt_test_main.cpp)
        target_link_libraries(${name} PRIVATE ${ARG_DEPS} ${ARG_THIRD_PARTY} Qt6::Widgets GTest::gtest GTest::gmock)
        target_compile_definitions(${name} PRIVATE FROST_QT_PLUGIN_DIR="${FROST_QT_PLUGIN_DIR}")
    else()
        add_executable(${name} ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE ${ARG_DEPS} ${ARG_THIRD_PARTY} GTest::gtest GTest::gmock GTest::gtest_main)
    endif()
    frost_apply_compile_options(${name} TEST)
    frost_copy_runtime_dlls(${name})
    if(WIN32)
        # Same process settings as frost.exe (UTF-8 code page, long paths, DPI awareness).
        target_sources(${name} PRIVATE ${FROST_TEST_MANIFEST_RC})
        set_source_files_properties(${FROST_TEST_MANIFEST_RC} PROPERTIES OBJECT_DEPENDS ${FROST_MANIFEST_PATH})
        target_link_options(${name} PRIVATE /MANIFEST:NO)
    endif()
    if(NOT ARG_LABELS)
        set(ARG_LABELS unit)
    endif()
    # The kind's scheduling default (see above).
    if(ARG_LABELS STREQUAL "integration" AND NOT ARG_RESOURCE_LOCK)
        set(ARG_RESOURCE_LOCK frost_desktop)
    elseif(ARG_LABELS STREQUAL "perf")
        set(ARG_RUN_SERIAL TRUE)
    endif()
    set(properties LABELS ${ARG_LABELS})
    if(ARG_RUN_SERIAL)
        list(APPEND properties RUN_SERIAL TRUE)
    endif()
    if(ARG_RESOURCE_LOCK)
        list(APPEND properties RESOURCE_LOCK ${ARG_RESOURCE_LOCK})
    endif()
    if(ARG_PROCESSORS)
        list(APPEND properties PROCESSORS ${ARG_PROCESSORS})
    endif()
    gtest_discover_tests(${name}
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 60
        PROPERTIES ${properties})
endfunction()
