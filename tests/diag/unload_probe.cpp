// SPDX-License-Identifier: Apache-2.0
//
// A privately loaded image for diagnostics lifetime and unwind-table tests. Frame-pointer and
// unwind metadata are kept in every configuration; the test never imports this DLL statically.
#include <cstdint>

extern "C" __declspec(dllexport) __declspec(noinline) std::uintptr_t frost_diag_unload_probe(std::uintptr_t value) {
    volatile std::uintptr_t frame[8] = {value};
    return frame[value % 8] + 1;
}
