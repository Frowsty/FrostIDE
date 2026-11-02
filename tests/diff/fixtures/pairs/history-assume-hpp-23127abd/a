// SPDX-License-Identifier: Apache-2.0
//
// FROST_ASSUME(condition): tells the optimizer that `condition` holds.
//
// Expands to the C++23 [[assume]] attribute, which every supported Clang
// implements. The condition is never evaluated at run time, so it must be free
// of side effects, and a false assumption is undefined behaviour: use it only on
// hot paths where a FROST_ASSERT already guards the same condition in Debug.
#pragma once

#define FROST_ASSUME(...) [[assume(__VA_ARGS__)]]
