// SPDX-License-Identifier: Apache-2.0
//
// Compile-fail probe: an expansion statement, which toolchain_probe must reject.
void probe() {
    template for (auto x : {1, 2}) { (void)x; }
}
