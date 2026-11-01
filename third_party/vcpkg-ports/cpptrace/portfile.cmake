# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Microsoft Corporation and vcpkg contributors
#
# vcpkg portfile for cpptrace 1.0.4: an overlay of vcpkg's cpptrace port at 2026.10.16 (9eec002)
# (ADR 0022). FrostIDE uses cpptrace for raw in-process traces (addresses, then module and offset)
# and symbolizes them offline, so the overlay selects back-ends that load nothing and spawn
# nothing: no libdwarf (LGPL, and with it no zstd), no addr2line process, no dbghelp.
#   Windows: unwinding with CaptureStackBackTrace (kernel32), no symbols, no demangling.
#   Linux:   unwinding with _Unwind_Backtrace (libgcc), no symbols, demangling with cxxabi.
# The emscripten and libunwind branches of upstream are dropped (FrostIDE builds neither).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jeremy-rifkin/cpptrace
    REF "v${VERSION}"
    SHA512 e88edddbcdd423d49ed3adb02cf70580ee3a56065db4d81ca69d3f9f6d9b64ac27734842ca3b6d8ff45a548c25900a88f979e39d777af422a153e586d26ac5b5
    HEAD_REF main
)

if(VCPKG_TARGET_IS_WINDOWS)
    set(BACKEND_OPTIONS
        -DCPPTRACE_UNWIND_WITH_WINAPI=ON
        -DCPPTRACE_GET_SYMBOLS_WITH_NOTHING=ON
        -DCPPTRACE_DEMANGLE_WITH_NOTHING=ON
    )
else()
    set(BACKEND_OPTIONS
        -DCPPTRACE_UNWIND_WITH_UNWIND=ON
        -DCPPTRACE_GET_SYMBOLS_WITH_NOTHING=ON
        -DCPPTRACE_DEMANGLE_WITH_CXXABI=ON
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCPPTRACE_VCPKG=ON
        ${BACKEND_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME "cpptrace"
    CONFIG_PATH "lib/cmake/cpptrace"
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
