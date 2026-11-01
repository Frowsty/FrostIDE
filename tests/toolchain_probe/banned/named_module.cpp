// SPDX-License-Identifier: Apache-2.0
//
// frost-lint fixture: named modules are banned (PLAN.md section 2.1). Clang 23 compiles
// this as a module unit, so frost-lint's cpp26 rule enforces the ban. Nothing builds
// this file, and this directory is exempt from that rule; the test in
// tests/tools/test_frost_lint.py lints a copy outside it and expects the rule to fire.
export module frost_probe;
export int probeModule() { return 1; }
