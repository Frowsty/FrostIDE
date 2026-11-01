# SPDX-License-Identifier: Apache-2.0
#
# Test for FrostCompilerCache.cmake (run with cmake -P from tests/cmake). It configures
# tests/cmake/compiler_cache_fixture six times in one build tree while the sccache pin
# and the installed copies change, the way a long-lived tree sees the lock move, and
# checks which sccache the module chose, what it printed and what CMakeCache.txt keeps
# in FROST_SCCACHE_EXECUTABLE. The case it guards: a cached path that no longer passes
# the version check must be searched for again rather than silently turning the cache off.
#
# The fake sccaches are copies of ninja and of cmake itself, since `--version` makes
# them print two different x.y.z versions (a copied cmake also complains on stderr that
# it cannot find its modules, which the module ignores). They stand in for an old and a
# new pin. The children see only the test's $FROST_SDK, SCCACHE_DIR and PATH, and
# CMake's system and environment prefixes are off, so a real sccache is never found.
# Nothing is compiled.
# Inputs (-D): FROST_SOURCE_DIR, WORK_DIR, MAKE_PROGRAM (ninja).

file(REMOVE_RECURSE "${WORK_DIR}")
set(repo "${WORK_DIR}/repo") # Stands in for the checkout: cmake/ and sdk/sdk.lock.json.
set(sdk "${WORK_DIR}/sdk")   # $FROST_SDK, holding sccache-<version> directories.
set(bin "${WORK_DIR}/bin")   # The children's only PATH entry.
set(tree "${WORK_DIR}/tree") # The build tree that is configured again and again.
file(MAKE_DIRECTORY "${repo}/cmake" "${repo}/sdk" "${sdk}" "${bin}" "${WORK_DIR}/sccache-dir")
file(COPY_FILE "${FROST_SOURCE_DIR}/cmake/FrostCompilerCache.cmake" "${repo}/cmake/FrostCompilerCache.cmake")

if(CMAKE_HOST_WIN32)
    set(exe ".exe")
else()
    set(exe "")
endif()
file(TO_NATIVE_PATH "${sdk}" native_sdk)
file(TO_NATIVE_PATH "${bin}" native_bin)
file(TO_NATIVE_PATH "${WORK_DIR}/sccache-dir" native_sccache_dir)
set(ENV{FROST_SDK} "${native_sdk}")
set(ENV{SCCACHE_DIR} "${native_sccache_dir}")
set(ENV{PATH} "${native_bin}")

# Sets <out> to the x.y.z that `<program> --version` prints, or fails the test.
function(version_of program out)
    execute_process(COMMAND "${program}" --version OUTPUT_VARIABLE text ERROR_QUIET RESULT_VARIABLE status)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" version "${text}")
    if(NOT status EQUAL 0 OR version STREQUAL "")
        message(FATAL_ERROR "test setup: '${program} --version' exited with '${status}' and printed '${text}'")
    endif()
    set(${out} "${version}" PARENT_SCOPE)
endfunction()

# Installs a copy of <program> as <dir>/sccache (sccache.exe on Windows).
function(install_fake program dir)
    file(MAKE_DIRECTORY "${dir}")
    file(COPY_FILE "${program}" "${dir}/sccache${exe}")
    if(NOT CMAKE_HOST_WIN32)
        file(CHMOD "${dir}/sccache${exe}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endif()
endfunction()

# Writes the fixture's lock file with <version> as the sccache pin of both platforms.
function(pin version)
    file(WRITE "${repo}/sdk/sdk.lock.json"
         "{\"windows\": {\"sccache\": {\"version\": \"${version}\"}},\n"
         " \"linux\": {\"sccache\": {\"version\": \"${version}\"}}}\n")
endfunction()

# Configures the fixture in ${tree}. Sets `output` (all output, whitespace runs collapsed
# to one space, since CMake wraps warnings) and `cached` (FROST_SCCACHE_EXECUTABLE in
# CMakeCache.txt) in the caller.
function(configure step)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${FROST_SOURCE_DIR}/tests/cmake/compiler_cache_fixture" -B "${tree}"
                -G Ninja "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-DFROST_FIXTURE_REPO=${repo}"
                -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=OFF -DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=OFF
        RESULT_VARIABLE result
        OUTPUT_VARIABLE raw
        ERROR_VARIABLE raw)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "step ${step}: configure failed (${result}):\n${raw}")
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${raw}")
    file(STRINGS "${tree}/CMakeCache.txt" line REGEX "^FROST_SCCACHE_EXECUTABLE:")
    string(REGEX REPLACE "^[^=]*=" "" value "${line}")
    set(output "${flat}" PARENT_SCOPE)
    set(raw_output "${raw}" PARENT_SCOPE)
    set(cached "${value}" PARENT_SCOPE)
endfunction()

# Fails step <step> unless the last configure's output contains <text>.
function(expect_output step text)
    string(FIND "${output}" "${text}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "step ${step}: expected '${text}' in the configure output:\n${raw_output}")
    endif()
endfunction()

# Fails step <step> if the last configure's output contains <text>.
function(expect_absent step text)
    string(FIND "${output}" "${text}" at)
    if(NOT at EQUAL -1)
        message(FATAL_ERROR "step ${step}: unexpected '${text}' in the configure output:\n${raw_output}")
    endif()
endfunction()

# Fails step <step> unless CMakeCache.txt holds <path> in FROST_SCCACHE_EXECUTABLE.
function(expect_cached step path)
    if(NOT cached STREQUAL path)
        message(FATAL_ERROR "step ${step}: FROST_SCCACHE_EXECUTABLE is '${cached}', expected '${path}'")
    endif()
endfunction()

version_of("${MAKE_PROGRAM}" old)   # The "old pin": ninja's version.
version_of("${CMAKE_COMMAND}" new)  # The "new pin": cmake's version.
if(old STREQUAL new)
    message(FATAL_ERROR "test setup: ninja and cmake both report ${old}; the test needs two versions")
endif()

# 1. First configure: the pinned sccache is installed under $FROST_SDK.
pin("${old}")
install_fake("${MAKE_PROGRAM}" "${sdk}/sccache-${old}")
configure(1)
expect_absent(1 "CMake Warning")
expect_output(1 "FrostIDE compiler cache: sccache ${old} (${sdk}/sccache-${old}/sccache${exe},")
expect_output(1 "fixture: launcher '${sdk}/sccache-${old}/sccache${exe}'")
expect_cached(1 "${sdk}/sccache-${old}/sccache${exe}")

# 2. The lock moves to a new version and the bootstrap installs it next to the old one.
#    The cached old path now reports the wrong version; the new one must be found.
pin("${new}")
install_fake("${CMAKE_COMMAND}" "${sdk}/sccache-${new}")
configure(2)
expect_absent(2 "CMake Warning")
expect_output(2 "FrostIDE compiler cache: sccache ${new} (${sdk}/sccache-${new}/sccache${exe},")
expect_output(2 "fixture: launcher '${sdk}/sccache-${new}/sccache${exe}'")
expect_cached(2 "${sdk}/sccache-${new}/sccache${exe}")

# 3. The cached executable disappears; the same version is still on PATH.
file(REMOVE_RECURSE "${sdk}/sccache-${new}")
install_fake("${CMAKE_COMMAND}" "${bin}")
configure(3)
expect_absent(3 "CMake Warning")
expect_output(3 "FrostIDE compiler cache: sccache ${new} (${bin}/sccache${exe},")
expect_cached(3 "${bin}/sccache${exe}")

# 4. No sccache is left at all (sccache-${old} is not searched, since the pin moved).
#    FROST_SCCACHE is cached ON, so this warns, names the dropped path, and caches the miss.
file(REMOVE "${bin}/sccache${exe}")
configure(4)
expect_output(4 "CMake Warning")
expect_output(4 "FROST_SCCACHE is ON, but sccache ${new} not found")
expect_output(4 "the cached ${bin}/sccache${exe} was dropped because it failed to run --version")
expect_output(4 "FrostIDE compiler cache: off (sccache ${new} not found")
expect_output(4 "fixture: launcher ''")
if(NOT cached MATCHES "NOTFOUND$")
    message(FATAL_ERROR "step 4: FROST_SCCACHE_EXECUTABLE is '${cached}', expected a NOTFOUND value")
endif()

# 5. Only an sccache of the wrong version is on PATH: off, with the version in the reason.
install_fake("${MAKE_PROGRAM}" "${bin}")
configure(5)
expect_output(5 "FROST_SCCACHE is ON, but ${bin}/sccache${exe} reports '${old}', not the pinned ${new}")
expect_output(5 "FrostIDE compiler cache: off (${bin}/sccache${exe} reports '${old}', not the pinned ${new}")
expect_output(5 "fixture: launcher ''")
expect_cached(5 "${bin}/sccache${exe}")

# 6. Unchanged, so the retry finds the same wrong one: the reason does not repeat it.
configure(6)
expect_output(6 "FrostIDE compiler cache: off (${bin}/sccache${exe} reports '${old}', not the pinned ${new}")
expect_absent(6 "was dropped")
expect_cached(6 "${bin}/sccache${exe}")

message(STATUS "FrostCompilerCache: a stale cached sccache is searched for again")
