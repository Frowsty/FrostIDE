# SPDX-License-Identifier: Apache-2.0
#
# FrostCompilerCache.cmake: runs FrostIDE's own C and C++ compiles through the
# pinned sccache (PLAN.md section 6.4; budget B28 assumes "the Debug build uses
# sccache"). The root CMakeLists.txt includes it after project() and before any
# target exists, because CMAKE_<LANG>_COMPILER_LAUNCHER only initialises the
# COMPILER_LAUNCHER property of targets created afterwards. vcpkg port builds run
# in their own processes during project() and are not affected.
#
# FROST_SCCACHE (cache BOOL) selects it. Its default is ON when the sccache pinned
# for this host in sdk/sdk.lock.json is found: first in $ENV{FROST_SDK}/sccache-<version>,
# then on PATH (tools/env.ps1 puts the pinned one there), and only if its version
# matches the pin. Linux has no pin (tools/bootstrap-wsl.sh installs no sccache), so
# there the default is OFF and the build is unchanged.
#
# The path found is cached in FROST_SCCACHE_EXECUTABLE and checked again on every
# configure. A cached path that no longer runs or no longer reports the pin (the lock
# moved to a new version, say) is dropped and searched for once more, so an existing
# tree picks up a newly pinned sccache instead of quietly building without one.
# tests/cmake/check_compiler_cache.cmake covers these cases.
#
# On Windows the cache must stay off the system drive (D1). sccache's server reads
# SCCACHE_DIR when it starts, so this file requires SCCACHE_DIR in the configure
# environment (tools/env.ps1 sets E:\sdk\sccache-cache) and builds without sccache
# otherwise. Builds must run from an environment with the same SCCACHE_DIR, as
# tools/verify.ps1 does, because the first compile may be the one that starts the server.
#
# Each launcher invocation mixes a canonical source/build-root identity with the caller's
# current SCCACHE_C_CUSTOM_CACHE_BUSTER. This prevents reuse of /Z7 objects whose debug
# records name another worktree, while retaining same-tree hits and user cache separation.
#
# A CMAKE_C_COMPILER_LAUNCHER or CMAKE_CXX_COMPILER_LAUNCHER given by the user wins
# and is left as it is. Configure prints one "FrostIDE compiler cache:" status line
# saying whether sccache is used and, if not, why.
#
# Cacheability (checked with sccache 0.19.0 and clang-cl 23.1.4):
#  - Debug information lives in the object (/Z7, CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
#    in the root file), so every object is self-contained.
#  - sccache reads an unknown option that starts with "/", or the separate value of an
#    unknown option, as a second input file and then refuses to cache the compile
#    ("multiple input files" in `sccache --show-stats`). Options sccache does not know
#    must therefore be spelled "-name<value>", as cmake/toolchains/windows-clangcl.cmake
#    does for its MSVC and Windows SDK pins.
#  - Precompiled headers build but are not cached: sccache passes compiles with /Yc
#    or /Fp (CMake adds /Fp to every source that uses the PCH) through uncached.
#  - Unity builds are cached: each unity source is an ordinary translation unit.

include_guard(GLOBAL)

# The pinned version for this host, or "" when sdk.lock.json has no pin for it.
if(WIN32)
    set(_frost_lock_platform windows)
else()
    set(_frost_lock_platform linux)
endif()
file(READ "${CMAKE_CURRENT_LIST_DIR}/../sdk/sdk.lock.json" _frost_cache_lock)
string(JSON _frost_sccache_pin ERROR_VARIABLE _frost_sccache_pin_error
       GET "${_frost_cache_lock}" ${_frost_lock_platform} sccache version)
if(_frost_sccache_pin_error)
    set(_frost_sccache_pin "")
endif()
# How to get the pinned sccache, for the status line when it is missing. Only Windows
# has a pin today; the Linux text is for a future pin.
if(WIN32)
    set(_frost_sccache_fix "run tools/bootstrap.ps1, then tools/env.ps1")
else()
    set(_frost_sccache_fix "install sccache ${_frost_sccache_pin} in $FROST_SDK/sccache-${_frost_sccache_pin} or on PATH")
endif()

# Finds the pinned sccache. _frost_sccache_reason explains a miss for the status line.
set(_frost_sccache_found OFF)
set(_frost_sccache_reason "")
if(_frost_sccache_pin STREQUAL "")
    set(_frost_sccache_reason "no sccache pinned for ${_frost_lock_platform} in sdk/sdk.lock.json")
else()
    set(_frost_sccache_hints "")
    if(DEFINED ENV{FROST_SDK})
        file(TO_CMAKE_PATH "$ENV{FROST_SDK}/sccache-${_frost_sccache_pin}" _frost_sccache_hints)
    endif()
    # find_program() does not search while FROST_SCCACHE_EXECUTABLE holds a path, so in
    # a tree configured before the pin moved (or before that sccache was removed) the
    # hints and PATH would never be read again. A path that was already cached, or given
    # with -DFROST_SCCACHE_EXECUTABLE, and that fails the check below is therefore
    # dropped and the search runs a second time; the second result stands either way.
    set(_frost_sccache_retry OFF)
    if(DEFINED CACHE{FROST_SCCACHE_EXECUTABLE} AND NOT "$CACHE{FROST_SCCACHE_EXECUTABLE}" MATCHES "NOTFOUND$")
        set(_frost_sccache_retry ON)
    endif()
    set(_frost_sccache_dropped "")      # "the cached <path> was dropped because ...", for the reason
    set(_frost_sccache_dropped_path "")
    foreach(_frost_sccache_attempt RANGE 1)
        find_program(FROST_SCCACHE_EXECUTABLE sccache HINTS ${_frost_sccache_hints}
                     DOC "sccache used as the compiler launcher when FROST_SCCACHE is ON")
        set(_frost_sccache_problem "")
        if(NOT FROST_SCCACHE_EXECUTABLE)
            set(_frost_sccache_reason "sccache ${_frost_sccache_pin} not found")
        else()
            # `sccache --version` prints "sccache 0.19.0" and does not start the server.
            execute_process(COMMAND "${FROST_SCCACHE_EXECUTABLE}" --version
                            OUTPUT_VARIABLE _frost_sccache_version_text ERROR_QUIET
                            RESULT_VARIABLE _frost_sccache_version_status OUTPUT_STRIP_TRAILING_WHITESPACE)
            string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _frost_sccache_version "${_frost_sccache_version_text}")
            if(NOT _frost_sccache_version_status EQUAL 0)
                set(_frost_sccache_problem "failed to run --version (${_frost_sccache_version_status})")
            elseif(NOT _frost_sccache_version STREQUAL _frost_sccache_pin)
                set(_frost_sccache_problem
                    "reports '${_frost_sccache_version_text}', not the pinned ${_frost_sccache_pin}")
            else()
                set(_frost_sccache_found ON)
                break()
            endif()
            set(_frost_sccache_reason "${FROST_SCCACHE_EXECUTABLE} ${_frost_sccache_problem}")
        endif()
        if(NOT _frost_sccache_retry)
            break()
        endif()
        set(_frost_sccache_retry OFF)
        if(FROST_SCCACHE_EXECUTABLE)
            set(_frost_sccache_dropped_path "${FROST_SCCACHE_EXECUTABLE}")
            set(_frost_sccache_dropped
                "the cached ${FROST_SCCACHE_EXECUTABLE} was dropped because it ${_frost_sccache_problem}")
        endif()
        unset(FROST_SCCACHE_EXECUTABLE CACHE)
    endforeach()
    if(NOT _frost_sccache_found)
        # The drop is worth mentioning only when the second search ended somewhere else.
        if(NOT _frost_sccache_dropped STREQUAL ""
           AND NOT "${_frost_sccache_dropped_path}" STREQUAL "${FROST_SCCACHE_EXECUTABLE}")
            string(APPEND _frost_sccache_reason "; ${_frost_sccache_dropped}")
        endif()
        string(APPEND _frost_sccache_reason "; ${_frost_sccache_fix}")
    endif()
endif()

option(FROST_SCCACHE "Run C and C++ compiles through the pinned sccache (default: ON when it is found)"
       ${_frost_sccache_found})

if(DEFINED CMAKE_C_COMPILER_LAUNCHER OR DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
    message(STATUS "FrostIDE compiler cache: user launcher kept "
                   "(C '${CMAKE_C_COMPILER_LAUNCHER}', CXX '${CMAKE_CXX_COMPILER_LAUNCHER}')")
elseif(NOT FROST_SCCACHE)
    if(_frost_sccache_reason STREQUAL "")
        set(_frost_sccache_reason "FROST_SCCACHE is OFF")
    endif()
    message(STATUS "FrostIDE compiler cache: off (${_frost_sccache_reason})")
elseif(NOT _frost_sccache_found)
    # Explicitly ON, but the pinned sccache is missing: build without it rather than fail.
    message(WARNING "FROST_SCCACHE is ON, but ${_frost_sccache_reason}; building without a compiler cache")
    message(STATUS "FrostIDE compiler cache: off (${_frost_sccache_reason})")
elseif(WIN32 AND NOT DEFINED ENV{SCCACHE_DIR})
    message(WARNING "FROST_SCCACHE is ON, but SCCACHE_DIR is not set, so sccache would cache in the user "
                    "profile on the system drive (D1); building without it. Dot-source tools/env.ps1 first.")
    message(STATUS "FrostIDE compiler cache: off (SCCACHE_DIR not set)")
else()
    file(REAL_PATH "${CMAKE_SOURCE_DIR}" _frost_cache_source)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" _frost_cache_build)
    # The launcher must combine the user buster at build time, not configure time. A small
    # CMake script does that portably and preserves arbitrary argument boundaries, including
    # semicolons, before passing them to sccache. The buster is local to the launched process.
    string(LENGTH "${_frost_cache_source}" _frost_cache_source_len)
    string(LENGTH "${_frost_cache_build}" _frost_cache_build_len)
    string(SHA256 _frost_cache_salt
           "frost-source-v1:${_frost_cache_source_len}:${_frost_cache_source}:${_frost_cache_build_len}:${_frost_cache_build}")
    set(_frost_cache_script "${CMAKE_BINARY_DIR}/frost-sccache-launcher.cmake")
    file(CONFIGURE OUTPUT "${_frost_cache_script}" @ONLY CONTENT [=[
set(_user "$ENV{SCCACHE_C_CUSTOM_CACHE_BUSTER}")
string(LENGTH "${_user}" _user_len)
string(SHA256 _buster "@_frost_cache_salt@:${_user_len}:${_user}")
set(ENV{SCCACHE_C_CUSTOM_CACHE_BUSTER} "${_buster}")
# Expand each numbered argument as a quoted argument, without ever forming a CMake list.
# A list would merge or lose empty arguments and can reinterpret brackets and backslashes.
set(_invoke "execute_process(COMMAND")
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_index RANGE 4 ${_last})
    string(APPEND _invoke " \"\${CMAKE_ARGV${_index}}\"")
endforeach()
string(APPEND _invoke " RESULT_VARIABLE _status)")
cmake_language(EVAL CODE "${_invoke}")
if(NOT _status STREQUAL "0")
    message(FATAL_ERROR "sccache compiler invocation failed: ${_status}")
endif()
]=])
    set(_frost_cache_launcher "${CMAKE_COMMAND}" -P "${_frost_cache_script}" -- "${FROST_SCCACHE_EXECUTABLE}")
    set(CMAKE_C_COMPILER_LAUNCHER "${_frost_cache_launcher}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_frost_cache_launcher}")
    if(DEFINED ENV{SCCACHE_DIR})
        set(_frost_sccache_dir "$ENV{SCCACHE_DIR}")
    else()
        set(_frost_sccache_dir "sccache's default")
    endif()
    message(STATUS "FrostIDE compiler cache: sccache ${_frost_sccache_version} "
                   "(${FROST_SCCACHE_EXECUTABLE}, SCCACHE_DIR ${_frost_sccache_dir})")
endif()
