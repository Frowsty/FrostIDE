// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe fixture: expansion statements are banned. Clang 23 compiles this
// silently in C++26 mode, so frost-lint enforces the ban; this file is its fixture.
#include <tuple>
int probeExpansion() {
    int sum = 0;
    template for (auto value : std::tuple{1, 2}) {
        sum += value;
    }
    return sum;
}
