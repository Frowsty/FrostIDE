// SPDX-License-Identifier: Apache-2.0
//
// FROST_ASSUME(condition): tells the optimizer that `condition` holds.
//
// Expands to the C++23 [[assume]] attribute, which every supported Clang
// implements. The condition is never evaluated at run time, so it must be free
// of side effects, and a false assumption is undefined behaviour: use it only where
// the condition is already established and a check is too costly. An enforced
// FROST_ASSERT (the default in every configuration) already lets the optimizer
// assume its condition afterwards, so FROST_ASSUME pays off only where the check
// is absent or compiled with the ignore or observe semantic.
#pragma once

#define FROST_ASSUME(...) [[assume(__VA_ARGS__)]]
