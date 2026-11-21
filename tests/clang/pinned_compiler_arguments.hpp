// SPDX-License-Identifier: Apache-2.0
// Keeps the test compilation databases on the same explicit toolchain as the build.
#pragma once

#include <vector>

namespace frost::test {

template<typename T>
void appendPinnedCompilerArguments(std::vector<T>& arguments) {
#ifdef _WIN32
    arguments.emplace_back("-vctoolsdir" FROST_TEST_VCTOOLS_DIR);
    arguments.emplace_back("-winsdkdir" FROST_TEST_WINSDK_DIR);
    arguments.emplace_back("-winsdkversion" FROST_TEST_WINSDK_VERSION);
#elif defined(FROST_TEST_GCC_INSTALL_DIR)
    arguments.emplace_back("--gcc-install-dir=" FROST_TEST_GCC_INSTALL_DIR);
#else
    (void)arguments;
#endif
}

} // namespace frost::test
