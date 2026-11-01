# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Microsoft Corporation and vcpkg contributors
#
# CMake package configuration (unofficial-sqlite3) installed by vcpkg's sqlite3 port: an overlay of
# vcpkg's sqlite3 port at 2026.10.16 bumped to 3.54.1 (ADR 0014); delete it when the vcpkg baseline
# ships >= 3.54.1.

include(CMakeFindDependencyMacro)
if(NOT WIN32)
    find_dependency(Threads)
endif()
if("@SQLITE_ENABLE_ICU@" AND "@VCPKG_LIBRARY_LINKAGE@" STREQUAL "static")
    find_dependency(ICU COMPONENTS uc i18n)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/unofficial-sqlite3-targets.cmake)
