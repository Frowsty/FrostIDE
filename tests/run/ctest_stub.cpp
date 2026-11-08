// SPDX-License-Identifier: Apache-2.0
//
// A stub ctest for the RUN-7 acceptance "a stub ctest that exits 1 with no output yields the tool
// error and no results" (test_runner_test.cpp, tests/app/unit_tests_window_test.cpp). The build
// names it ctest(.exe) in a directory of its own. It reads no argument, prints nothing and exits 1.

int main() { return 1; }
