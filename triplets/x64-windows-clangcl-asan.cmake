# SPDX-License-Identifier: Apache-2.0
#
# vcpkg overlay triplet for the win-clangcl-asan preset: x64-windows-clangcl with
# every dependency compiled under AddressSanitizer. MSVC STL marks each object with
# `#pragma detect_mismatch("annotate_string"/"annotate_vector")`, "1" with ASan and
# "0" without, so linking an ASan test against a non-ASan gtest fails; building the
# ports with ASan keeps the container annotations on everywhere. Release only: the
# preset builds RelWithDebInfo, which maps to the ports' Release libraries, and
# clang-cl rejects ASan with the debug CRT (/MDd).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
# CMake's compiler checks build their test programs in Debug (/MDd) unless told
# otherwise, and clang-cl rejects /MDd together with -fsanitize=address.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_TRY_COMPILE_CONFIGURATION=Release)
set(VCPKG_C_FLAGS "-fsanitize=address")
set(VCPKG_CXX_FLAGS "-fsanitize=address")
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../cmake/toolchains/windows-clangcl.cmake")
set(VCPKG_ENV_PASSTHROUGH FROST_LLVM_ROOT VCPKG_ROOT)
