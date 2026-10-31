// SPDX-License-Identifier: Apache-2.0
// Contract macros under the "observe" semantic (see assert_semantics.hpp).
#define FROST_CONTRACT_SEMANTIC 1
#include "frost/core/assert.hpp"

#include "assert_semantics.hpp"

namespace frost::test {

void checkObserved(bool condition) { FROST_PRE(condition); }

} // namespace frost::test
