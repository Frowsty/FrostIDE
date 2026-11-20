// SPDX-License-Identifier: Apache-2.0
#include "OracleObject.h"
int32 UOracleObject::AddForBreakpoint(int32 left, int32 right) {
    const int32 sum = left + right;
    return sum; // Later native debugger oracle breakpoint: assert actual stopped frame and value.
}
