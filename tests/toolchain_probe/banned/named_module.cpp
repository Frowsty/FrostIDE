// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe fixture: named modules are banned (PLAN.md section 2.1). Clang 23 compiles
// this as a module unit, so frost-lint enforces the ban; this file is its fixture.
export module frost_probe;
export int probeModule() { return 1; }
