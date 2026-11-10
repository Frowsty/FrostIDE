# SPDX-License-Identifier: Apache-2.0
#
# vcpkg triplet for the Windows libFuzzer + ASan build tree (win-clangcl-fuzz, PLAN.md M2 and
# FND-14). On Windows, LLVM's libFuzzer runtime (clang_rt.fuzzer-x86_64.lib) is compiled against
# the static release CRT (/MT, _ITERATOR_DEBUG_LEVEL=0), so any port built for this triplet uses
# static CRT linkage and release-only configuration.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../cmake/toolchains/windows-clangcl.cmake")
