// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe fixture: static reflection is banned (PLAN.md section 2.1). Must not compile.
constexpr auto kReflected = ^^int;
