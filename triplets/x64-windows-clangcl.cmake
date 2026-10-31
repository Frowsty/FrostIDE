# SPDX-License-Identifier: Apache-2.0
#
# vcpkg overlay triplet: x64 Windows, dependencies built with the pinned
# clang-cl, dynamic CRT (/MD, matching Qt's prebuilt DLLs) and static libraries.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../cmake/toolchains/windows-clangcl.cmake")
# The toolchain reads these to find LLVM and vcpkg's standard Windows flags.
set(VCPKG_ENV_PASSTHROUGH FROST_LLVM_ROOT VCPKG_ROOT)
