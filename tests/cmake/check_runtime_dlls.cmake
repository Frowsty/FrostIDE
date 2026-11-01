# SPDX-License-Identifier: Apache-2.0
#
# Test for frost_copy_runtime_dlls (cmake/FrostLayers.cmake), run with cmake -P from
# tests/cmake. The rule under test: no build step may read or write a copied DLL in an
# output directory unless it is ordered after that directory's copy. Every executable
# there has such a step: vcpkg's z-applocal POST_BUILD step opens the DLLs it imports
# from beside it. In 51e9a05's fresh tests/perf/s1 directory, one executable's
# z-applocal opened Qt6Widgetsd.dll while the other's POST_BUILD copy was writing it, a
# reader against a writer, and vcpkg failed with "open_for_read(...): permission
# denied". The helper must therefore copy once per output directory and configuration,
# and order every executable in the directory after that copy, also one that was never
# registered, and every shared library built there (frost_app.dll beside frost.exe), whose
# z-applocal step reads the DLLs it imports too. This script configures
# tests/cmake/runtime_dlls_fixture with Ninja
# Multi-Config and the build's own toolchain (without vcpkg, so the fixture has no
# z-applocal step; the order check stands for it), then checks, on Windows:
#   1. structure: in the generated Ninja files of Debug and Release, each of the
#      fixture's three output directories is the destination of copy_if_different on
#      exactly one line (one build edge), so no two edges can write the same file;
#   2. order: in both configurations, every executable's and shared library's
#      object-order edge (which its compiles wait for) and its link edge list its
#      directory's copy target among their order-only inputs, including shared_raw and
#      shared_lib, which are not registered;
#   3. dependency: in the fresh tree, building one executable on its own puts its
#      directory's DLLs next to it, including DLLs that only its siblings link:
#      shared_c (registered, links no stand-in) in Debug, and shared_raw and shared_lib
#      in Release (shared_lib's imported DLL among them);
#   4. result: after parallel builds of both configurations every output directory
#      holds the union of its members' DLLs (shared_raw's and shared_lib's included),
#      the ASan stand-in and the DLLs built there, nothing else, and they come back
#      after the DLLs and executables are deleted;
#   5. without the ASan stand-in, a directory with nothing to copy gets no DLLs and
#      the others get only theirs;
# on Linux, that nothing is copied at all. On both, a registered and an unregistered
# executable with a per-configuration output directory must each fail configure with
# the helper's message.
# Inputs (-D): FROST_SOURCE_DIR, WORK_DIR, MAKE_PROGRAM, TOOLCHAIN.
file(REMOVE_RECURSE "${WORK_DIR}")
set(fixture "${FROST_SOURCE_DIR}/tests/cmake/runtime_dlls_fixture")
set(tree "${WORK_DIR}/tree")
set(configs Debug Release)
# Enough parallel jobs that the executables of one directory build at the same time.
set(jobs 8)

# Runs the command in ARGN; fails the test with its output unless it exits with 0.
function(run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${what} failed (${result}):\n${output}")
    endif()
endfunction()

# Configures the fixture into <dir>, with extra -D options in ARGN. Sets `result` and
# `output` in the caller.
function(configure dir)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${fixture}" -B "${dir}" -G "Ninja Multi-Config"
                "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}"
                "-DFROST_SOURCE_DIR=${FROST_SOURCE_DIR}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    set(result "${result}" PARENT_SCOPE)
    set(output "${output}" PARENT_SCOPE)
endfunction()

# Configures the fixture into <dir> like configure(), failing the test if that fails.
function(configure_ok dir)
    configure("${dir}" ${ARGN})
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "configure of ${dir} failed (${result}):\n${output}")
    endif()
endfunction()

# Builds <config> of the fixture in <dir> (extra build arguments in ARGN).
function(build dir config)
    run("build ${config} ${ARGN} in ${dir}" "${CMAKE_COMMAND}" --build "${dir}" --config ${config} -j ${jobs} ${ARGN})
endfunction()

# Sets <out> to the lines of every generated Ninja file in <dir>. Semicolons and
# brackets are replaced first, so each list element is exactly one line.
function(ninja_lines dir out)
    file(GLOB ninja_files "${dir}/*.ninja" "${dir}/CMakeFiles/*.ninja")
    set(found "")
    foreach(file IN LISTS ninja_files)
        file(READ "${file}" content)
        string(REGEX REPLACE "[][;]" "_" content "${content}")
        string(REPLACE "\n" ";" lines "${content}")
        list(APPEND found ${lines})
    endforeach()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# Fails the test unless target <name>'s object-order edge (the phony that its compiles
# wait for) and its link edge of <config> list <copy>, the copy target of its directory,
# among their order-only inputs (after `||`). <edges> holds the candidate Ninja lines.
# An optional fifth argument names the linker rule's kind: EXECUTABLE (the default) or
# SHARED_LIBRARY.
function(expect_ordered edges name copy config)
    set(linker EXECUTABLE)
    if(ARGC GREATER 4)
        set(linker "${ARGV4}")
    endif()
    string(TOUPPER "${config}" upper)
    foreach(kind IN ITEMS object-order link)
        if(kind STREQUAL "link")
            set(marker ": CXX_${linker}_LINKER__${name}_${config} ")
        else()
            set(marker "build cmake_object_order_depends_target_${name}_${upper}: ")
        endif()
        set(edge "")
        foreach(line IN LISTS edges)
            string(FIND "${line}" "${marker}" position)
            if(NOT position EQUAL -1)
                set(edge "${line}")
                break()
            endif()
        endforeach()
        if(edge STREQUAL "")
            message(FATAL_ERROR "no ${kind} edge of ${name} (${config}) in the generated Ninja files")
        endif()
        set(order_only "")
        string(FIND "${edge}" " || " bars)
        if(NOT bars EQUAL -1)
            string(SUBSTRING "${edge}" ${bars} -1 order_only)
        endif()
        # Ninja Multi-Config names the copy target <copy>:<config>, with `:` escaped.
        string(FIND "${order_only} " " ${copy}$:${config} " position)
        if(position EQUAL -1)
            message(FATAL_ERROR "the ${kind} edge of ${name} (${config}) is not ordered after ${copy}:\n${edge}")
        endif()
    endforeach()
endfunction()

# Fails the test unless <dir> holds exactly the DLLs named after DLLS and at least
# the other files named in ARGN.
function(expect_files dir)
    cmake_parse_arguments(ARG "" "" "DLLS" ${ARGN})
    file(GLOB present RELATIVE "${dir}" "${dir}/*")
    foreach(name IN LISTS ARG_UNPARSED_ARGUMENTS ARG_DLLS)
        if(NOT name IN_LIST present)
            message(FATAL_ERROR "${dir}: ${name} is missing (present: ${present})")
        endif()
    endforeach()
    file(GLOB dlls RELATIVE "${dir}" "${dir}/*.dll")
    foreach(name IN LISTS dlls)
        if(NOT name IN_LIST ARG_DLLS)
            message(FATAL_ERROR "${dir}: unexpected ${name} (expected DLLs: ${ARG_DLLS})")
        endif()
    endforeach()
endfunction()

configure_ok("${tree}")
ninja_lines("${tree}" ninja)
set(lines "${ninja}")
list(FILTER lines INCLUDE REGEX "copy_if_different")

if(CMAKE_HOST_WIN32)
    set(both fake_qt.dll fake_asan_runtime.dll)
    # shared/ also gets the stand-ins that only the unregistered shared_raw and shared_lib
    # link; shared_lib.dll itself is there once shared_lib is built.
    set(shared_dlls ${both} fake_extra.dll fake_lib.dll)

    # 1. One copy edge per output directory and configuration.
    foreach(config IN LISTS configs)
        foreach(dir IN ITEMS "${tree}/shared/${config}" "${tree}/bin/${config}" "${tree}/${config}")
            # The destination is copy_if_different's last argument: followed by a quote
            # (the end of cmd /C "..."), a space, or the end of the line.
            string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" pattern "${dir}")
            set(matching "")
            foreach(line IN LISTS lines)
                if(line MATCHES "${pattern}(\"| |$)")
                    list(APPEND matching "${line}")
                endif()
            endforeach()
            list(LENGTH matching count)
            if(NOT count EQUAL 1)
                list(JOIN matching "\n" text)
                message(FATAL_ERROR "${dir} is the destination of ${count} copy command lines, expected 1:\n${text}")
            endif()
        endforeach()
    endforeach()

    # 2. Every executable and shared library is ordered after its directory's copy
    # target, registered or not. The copy targets are named after the directory relative
    # to the build tree.
    set(edges "${ninja}")
    list(FILTER edges INCLUDE REGEX
         "^build (cmake_object_order_depends_target_|.*: CXX_(EXECUTABLE|SHARED_LIBRARY)_LINKER__)")
    foreach(config IN LISTS configs)
        foreach(name IN ITEMS shared_a shared_b shared_c shared_raw)
            expect_ordered("${edges}" ${name} frost_runtime_dlls_shared ${config})
        endforeach()
        expect_ordered("${edges}" shared_lib frost_runtime_dlls_shared ${config} SHARED_LIBRARY)
        foreach(name IN ITEMS bin_a bin_root)
            expect_ordered("${edges}" ${name} frost_runtime_dlls_bin ${config})
        endforeach()
        expect_ordered("${edges}" top_plain frost_runtime_dlls ${config})
    endforeach()

    # 3. One executable on its own, in the fresh tree, in each configuration: it gets
    # the union of its directory's DLLs.
    build("${tree}" Debug --target shared_c)
    expect_files("${tree}/shared/Debug" shared_c.exe DLLS ${shared_dlls})
    build("${tree}" Release --target shared_raw)
    expect_files("${tree}/shared/Release" shared_raw.exe DLLS ${shared_dlls})
    build("${tree}" Release --target shared_lib)
    expect_files("${tree}/shared/Release" shared_raw.exe DLLS ${shared_dlls} shared_lib.dll)

    # 4. Everything, in parallel, in both configurations; then again after the DLLs
    # and executables of one directory are deleted.
    foreach(config IN LISTS configs)
        build("${tree}" ${config})
        expect_files("${tree}/shared/${config}" shared_a.exe shared_b.exe shared_c.exe shared_raw.exe
                     DLLS ${shared_dlls} shared_lib.dll)
        expect_files("${tree}/bin/${config}" bin_a.exe bin_root.exe DLLS ${both})
        expect_files("${tree}/${config}" top_plain.exe DLLS fake_asan_runtime.dll)
    endforeach()
    file(GLOB built "${tree}/shared/Debug/*.dll" "${tree}/shared/Debug/*.exe")
    file(REMOVE ${built})
    build("${tree}" Debug)
    expect_files("${tree}/shared/Debug" shared_a.exe shared_b.exe shared_c.exe shared_raw.exe
                 DLLS ${shared_dlls} shared_lib.dll)

    # 5. No ASan stand-in: top_plain's directory has nothing to copy.
    set(plain "${WORK_DIR}/no_asan")
    configure_ok("${plain}" -DFIXTURE_ASAN=OFF)
    build("${plain}" Debug)
    expect_files("${plain}/shared/Debug" shared_a.exe shared_b.exe shared_c.exe shared_raw.exe
                 DLLS fake_qt.dll fake_extra.dll fake_lib.dll shared_lib.dll)
    expect_files("${plain}/Debug" top_plain.exe DLLS)
else()
    # No DLLs to copy: the helper must not generate any copy.
    if(lines)
        list(JOIN lines "\n" text)
        message(FATAL_ERROR "copy commands generated on a platform without DLLs:\n${text}")
    endif()
    build("${tree}" Debug)
    expect_files("${tree}/shared/Debug" shared_a shared_b shared_c shared_raw DLLS)
    expect_files("${tree}/bin/Debug" bin_a bin_root DLLS)
    expect_files("${tree}/Debug" top_plain DLLS)
endif()

# A per-configuration output directory is rejected at configure time on every
# platform, also for an executable that is not registered: the helper could not tell
# whether it lands beside a copy.
foreach(kind IN ITEMS registered raw)
    configure("${WORK_DIR}/per_config_${kind}" -DFIXTURE_PER_CONFIG_OUTPUT=${kind})
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
    string(FIND "${flat}" "frost_copy_runtime_dlls: per_config sets RUNTIME_OUTPUT_DIRECTORY_DEBUG" position)
    if(result EQUAL 0 OR position EQUAL -1)
        message(FATAL_ERROR "a per-configuration output directory (${kind}) was not rejected (${result}):\n${output}")
    endif()
endforeach()
message(STATUS "frost_copy_runtime_dlls: one copy step per output directory and configuration, "
               "every executable and shared library ordered after it")
