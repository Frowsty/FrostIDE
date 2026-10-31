// SPDX-License-Identifier: Apache-2.0
// Contract macros under the "ignore" semantic (see assert_semantics.hpp).
#define FROST_CONTRACT_SEMANTIC 0
#include "frost/core/assert.hpp"

#include "assert_semantics.hpp"

namespace frost::test {

// The side effect below exists precisely to prove the condition is never
// evaluated, so Clang's warning about side effects in unevaluated contexts is
// expected here and only here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunevaluated-expression"
int checkIgnored(bool condition) {
    int evaluations = 0;
    // The comma expression would count an evaluation; "ignore" must not evaluate it.
    FROST_ASSERT((++evaluations, condition));
    return evaluations;
}
#pragma clang diagnostic pop

} // namespace frost::test
