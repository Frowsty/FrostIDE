// SPDX-License-Identifier: Apache-2.0
//
// Entry points of the assert_*_tu.cpp translation units, each compiled with a
// different FROST_CONTRACT_SEMANTIC, so one test binary can check all four.
#pragma once

namespace frost::test {

// Runs FROST_ASSERT on a condition under the "ignore" semantic; returns how
// many times the condition expression was evaluated (must be 0).
int checkIgnored(bool condition);

// Runs FROST_PRE(condition) under the "observe" semantic and returns normally.
void checkObserved(bool condition);

// Runs FROST_ASSERT(condition) under "quick_enforce": traps when false.
void checkQuickEnforced(bool condition);

} // namespace frost::test
