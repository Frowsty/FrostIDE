// SPDX-License-Identifier: Apache-2.0
//
// frost-lint fixture: expansion statements are banned (PLAN.md section 2.1). Clang 23
// compiles this silently in C++26 mode, so frost-lint's cpp26 rule enforces the ban.
// Nothing builds this file, and this directory is exempt from that rule; the test
// in tests/tools/test_frost_lint.py lints a copy outside it and expects the rule to fire.
#include <tuple>
int probeExpansion() {
    int sum = 0;
    template for (auto value : std::tuple{1, 2}) {
        sum += value;
    }
    return sum;
}
