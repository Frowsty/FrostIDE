# SPDX-License-Identifier: Apache-2.0
#
# Test for frost_copy_runtime_dlls (cmake/FrostLayers.cmake), run with cmake -P from
# tests/cmake. Two build steps that copy the same DLL into one output directory race:
# in a fresh directory one copy fails with a sharing violation ("permission denied"),
# as s1_editor and s1_editor_tests did. The helper must therefore copy once per output
# directory and configuration. This script configures tests/cmake/runtime_dlls_fixture
# with Ninja Multi-Config and the build's own toolchain, then checks, on Windows:
#   1. structure: in the generated Ninja files of Debug and Release, each of the
#      fixture's three output directories is the destination of copy_if_different on
#      exactly one line (one build edge), so no two edges can write the same file;
#   2. dependency: in the fresh tree, building one consumer on its own puts its
#      directory's DLLs next to it, including a DLL that only its siblings link;
#   3. result: after parallel builds of both configurations every output directory
#      holds the union of its own consumers' DLLs and the ASan stand-in, nothing else,
#      and they come back after the DLLs and executables are deleted;
#   4. without the ASan stand-in, a directory with nothing to copy gets no DLLs and
#      the others get only theirs;
# on Linux, that nothing is copied at all. On both, a consumer with a
# per-configuration output directory must fail configure with the helper's message.
# Inputs (-D): FROST_SOURCE_DIR, WORK_DIR, MAKE_PROGRAM, TOOLCHAIN.
file(REMOVE_RECURSE "${WORK_DIR}")
set(fixture "${FROST_SOURCE_DIR}/tests/cmake/runtime_dlls_fixture")
set(tree "${WORK_DIR}/tree")
set(configs Debug Release)
# Enough parallel jobs that the consumers of one directory build at the same time.
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

# Sets <out> to the lines of every generated Ninja file in <dir> that run
# copy_if_different. Semicolons and brackets are replaced first, so each list element
# is exactly one line.
function(copy_lines dir out)
    file(GLOB ninja_files "${dir}/*.ninja" "${dir}/CMakeFiles/*.ninja")
    set(found "")
    foreach(file IN LISTS ninja_files)
        file(READ "${file}" content)
        string(REGEX REPLACE "[][;]" "_" content "${content}")
        string(REPLACE "\n" ";" lines "${content}")
        foreach(line IN LISTS lines)
            if(line MATCHES "copy_if_different")
                list(APPEND found "${line}")
            endif()
        endforeach()
    endforeach()
    set(${out} "${found}" PARENT_SCOPE)
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
copy_lines("${tree}" lines)

if(CMAKE_HOST_WIN32)
    set(both fake_qt.dll fake_asan_runtime.dll)

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

    # 2. One consumer on its own, in the fresh tree: shared_c links no Qt stand-in but
    # shares the directory with consumers that do, so it gets the union.
    build("${tree}" Debug --target shared_c)
    expect_files("${tree}/shared/Debug" shared_c.exe DLLS ${both})

    # 3. Everything, in parallel, in both configurations; then again after the DLLs
    # and executables of one directory are deleted.
    foreach(config IN LISTS configs)
        build("${tree}" ${config})
        expect_files("${tree}/shared/${config}" shared_a.exe shared_b.exe shared_c.exe DLLS ${both})
        expect_files("${tree}/bin/${config}" bin_a.exe bin_root.exe DLLS ${both})
        expect_files("${tree}/${config}" top_plain.exe DLLS fake_asan_runtime.dll)
    endforeach()
    file(GLOB built "${tree}/shared/Debug/*.dll" "${tree}/shared/Debug/*.exe")
    file(REMOVE ${built})
    build("${tree}" Debug)
    expect_files("${tree}/shared/Debug" shared_a.exe shared_b.exe shared_c.exe DLLS ${both})

    # 4. No ASan stand-in: top_plain's directory has nothing to copy.
    set(plain "${WORK_DIR}/no_asan")
    configure_ok("${plain}" -DFIXTURE_ASAN=OFF)
    build("${plain}" Debug)
    expect_files("${plain}/shared/Debug" shared_a.exe shared_b.exe shared_c.exe DLLS fake_qt.dll)
    expect_files("${plain}/Debug" top_plain.exe DLLS)
else()
    # No DLLs to copy: the helper must not generate any copy.
    if(lines)
        list(JOIN lines "\n" text)
        message(FATAL_ERROR "copy commands generated on a platform without DLLs:\n${text}")
    endif()
    build("${tree}" Debug)
    expect_files("${tree}/shared/Debug" shared_a shared_b shared_c DLLS)
    expect_files("${tree}/bin/Debug" bin_a bin_root DLLS)
    expect_files("${tree}/Debug" top_plain DLLS)
endif()

# A per-configuration output directory is rejected at configure time on every platform.
configure("${WORK_DIR}/per_config" -DFIXTURE_PER_CONFIG_OUTPUT=ON)
string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
string(FIND "${flat}" "frost_copy_runtime_dlls: per_config sets RUNTIME_OUTPUT_DIRECTORY_DEBUG" position)
if(result EQUAL 0 OR position EQUAL -1)
    message(FATAL_ERROR "a per-configuration output directory was not rejected (${result}):\n${output}")
endif()
message(STATUS "frost_copy_runtime_dlls: one copy step per output directory and configuration")
