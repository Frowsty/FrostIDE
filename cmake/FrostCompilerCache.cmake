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
# On Windows the cache must stay off the system drive (D1). sccache's server reads
# SCCACHE_DIR when it starts, so this file requires SCCACHE_DIR in the configure
# environment (tools/env.ps1 sets E:\sdk\sccache-cache) and builds without sccache
# otherwise. Builds must run from an environment with the same SCCACHE_DIR, as
# tools/verify.ps1 does, because the first compile may be the one that starts the server.
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
    find_program(FROST_SCCACHE_EXECUTABLE sccache HINTS ${_frost_sccache_hints}
                 DOC "sccache used as the compiler launcher when FROST_SCCACHE is ON")
    if(NOT FROST_SCCACHE_EXECUTABLE)
        set(_frost_sccache_reason "sccache ${_frost_sccache_pin} not found (run tools/bootstrap.ps1, then tools/env.ps1)")
    else()
        # `sccache --version` prints "sccache 0.19.0" and does not start the server.
        execute_process(COMMAND "${FROST_SCCACHE_EXECUTABLE}" --version
                        OUTPUT_VARIABLE _frost_sccache_version_text ERROR_QUIET
                        RESULT_VARIABLE _frost_sccache_version_status OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _frost_sccache_version "${_frost_sccache_version_text}")
        if(NOT _frost_sccache_version_status EQUAL 0 OR NOT _frost_sccache_version STREQUAL _frost_sccache_pin)
            set(_frost_sccache_reason
                "${FROST_SCCACHE_EXECUTABLE} reports '${_frost_sccache_version_text}', not the pinned ${_frost_sccache_pin}")
        else()
            set(_frost_sccache_found ON)
        endif()
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
    set(CMAKE_C_COMPILER_LAUNCHER "${FROST_SCCACHE_EXECUTABLE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${FROST_SCCACHE_EXECUTABLE}")
    if(DEFINED ENV{SCCACHE_DIR})
        set(_frost_sccache_dir "$ENV{SCCACHE_DIR}")
    else()
        set(_frost_sccache_dir "sccache's default")
    endif()
    message(STATUS "FrostIDE compiler cache: sccache ${_frost_sccache_version} "
                   "(${FROST_SCCACHE_EXECUTABLE}, SCCACHE_DIR ${_frost_sccache_dir})")
endif()
