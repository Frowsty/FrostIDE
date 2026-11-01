// SPDX-License-Identifier: Apache-2.0
//
// libFuzzer entry points for the encoding harness (text_decode_harness.cpp), built only in
// the fuzz trees (FROST_FUZZ; presets win-clangcl-fuzz and linux-clang-fuzz).

#include "fuzz/fuzz_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    frost::fuzz::initialize();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const frost::Result<void> result = frost::fuzz::runInput({data, size});
    if (!result) {
        // libFuzzer saves the input as a crash artifact; the message says what differed.
        std::fprintf(stderr, "%s\n", result.error().describe().c_str());
        __builtin_trap();
    }
    return 0;
}
