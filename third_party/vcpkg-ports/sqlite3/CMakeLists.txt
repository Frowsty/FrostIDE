# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Microsoft Corporation and vcpkg contributors
#
# Build of the SQLite amalgamation for vcpkg's sqlite3 port: an overlay of vcpkg's sqlite3 port at
# 2026.10.16 bumped to 3.54.1 (ADR 0014); delete it when the vcpkg baseline ships >= 3.54.1.
# The 3.54.1 tarball has the upstream layout (sqlite3.c, sqlite3.h, sqlite3ext.h, sqlite3.rc and
# shell.c at the top level), checked on 2026-11-01. One change from upstream: sqlite3.rc (the DLL's
# version resource) is compiled for shared builds only. FrostIDE's triplets link statically, where
# the linker never pulls a version resource out of a static library, and with the pinned llvm-rc
# the resource step fails: vcpkg's scripts/toolchains/windows.cmake puts /c65001 into
# CMAKE_RC_FLAGS, and CMake's llvm-rc wrapper also hands those flags to clang-cl's preprocessor,
# which rejects the option ("no such file or directory: '/c65001'"; CMake 4.4.5, LLVM 23.1.4).
cmake_minimum_required(VERSION 3.10)

project(sqlite3 C CXX)

option(WITH_ZLIB "Build sqlite3 with zlib support" OFF)
option(SQLITE3_SKIP_TOOLS "Disable build sqlite3 executable" OFF)

set(PKGCONFIG_LIBS_PRIVATE "")
set(PKGCONFIG_REQUIRES_PRIVATE "")

if(BUILD_SHARED_LIBS)
    add_library(sqlite3 sqlite3.c sqlite3.rc)
else()
    add_library(sqlite3 sqlite3.c)
endif()

target_include_directories(sqlite3 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}> $<INSTALL_INTERFACE:include>)

target_compile_definitions(
    sqlite3
    PRIVATE
        $<$<CONFIG:Debug>:SQLITE_DEBUG=1>
        $<$<CONFIG:Debug>:SQLITE_ENABLE_SELECTTRACE>
        $<$<CONFIG:Debug>:SQLITE_ENABLE_WHERETRACE>
        $<$<COMPILE_LANGUAGE:RC>:RC_VERONLY>
)

if (BUILD_SHARED_LIBS)
    if (WIN32)
        target_compile_definitions(sqlite3 PRIVATE "SQLITE_API=__declspec(dllexport)")
    else()
        target_compile_definitions(sqlite3 PRIVATE "SQLITE_API=__attribute__((visibility(\"default\")))")
    endif()
endif()

if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    string(APPEND PKGCONFIG_LIBS_PRIVATE " -pthread")
    foreach(LIB IN LISTS CMAKE_DL_LIBS)
        string(APPEND PKGCONFIG_LIBS_PRIVATE " -l${LIB}")
    endforeach()

    if(SQLITE_ENABLE_FTS5 OR SQLITE_ENABLE_MATH_FUNCTIONS)
        find_library(HAVE_LIBM m PATHS ${CMAKE_C_IMPLICIT_LINK_DIRECTORIES} ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES})
        if(HAVE_LIBM)
            target_link_libraries(sqlite3 PRIVATE m)
            string(APPEND PKGCONFIG_LIBS_PRIVATE " -lm")
        endif()
    endif()
endif()

if(SQLITE_ENABLE_ICU)
    find_package(ICU COMPONENTS uc i18n REQUIRED)
    target_link_libraries(sqlite3 PRIVATE ICU::uc ICU::i18n)

    string(APPEND PKGCONFIG_REQUIRES_PRIVATE " icu-uc icu-i18n")
endif()

if(NOT SQLITE3_SKIP_TOOLS)
    add_executable(sqlite3-bin shell.c)
    set_target_properties(sqlite3-bin PROPERTIES
        RUNTIME_OUTPUT_NAME sqlite3
    )

    target_link_libraries(sqlite3-bin PRIVATE sqlite3)
    if (WITH_ZLIB)
        find_package(ZLIB REQUIRED)
        target_link_libraries(sqlite3-bin PRIVATE ZLIB::ZLIB)
        target_compile_definitions(sqlite3-bin PRIVATE SQLITE_HAVE_ZLIB)
    endif()

    find_library(HAVE_LIBM m PATHS ${CMAKE_C_IMPLICIT_LINK_DIRECTORIES} ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES})
    if(HAVE_LIBM)
        target_link_libraries(sqlite3-bin PRIVATE m)
    endif()

    install(TARGETS sqlite3-bin
      RUNTIME DESTINATION bin
      LIBRARY DESTINATION lib
      ARCHIVE DESTINATION lib
    )
endif()

install(
    TARGETS sqlite3
    EXPORT unofficial-sqlite3-targets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES sqlite3.h sqlite3ext.h sqlite3-vcpkg-config.h DESTINATION include CONFIGURATIONS Release)
install(EXPORT unofficial-sqlite3-targets NAMESPACE unofficial::sqlite3:: FILE unofficial-sqlite3-targets.cmake DESTINATION share/unofficial-sqlite3)

configure_file(sqlite3.pc.in sqlite3.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sqlite3.pc" DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig")
