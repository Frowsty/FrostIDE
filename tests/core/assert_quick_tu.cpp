// SPDX-License-Identifier: Apache-2.0
// Contract macros under the "quick_enforce" semantic (see assert_semantics.hpp).
#define FROST_CONTRACT_SEMANTIC 3
#include "frost/core/assert.hpp"

#include "assert_semantics.hpp"

namespace frost::test {

void checkQuickEnforced(bool condition) { FROST_ASSERT(condition); }

} // namespace frost::test
