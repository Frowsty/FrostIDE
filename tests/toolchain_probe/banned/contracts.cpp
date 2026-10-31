// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe fixture: contracts syntax is banned (use FROST_PRE/FROST_POST). Must not compile.
int probeContracts(int x) pre(x > 0) { return x; }
