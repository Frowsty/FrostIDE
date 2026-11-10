// SPDX-License-Identifier: Apache-2.0
//
// libFuzzer entry point for json_parser (FND-14); the harness itself is in json_parser_harness.cpp.

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
        std::fprintf(stderr, "%s\n", result.error().describe().c_str());
        __builtin_trap();
    }
    return 0;
}
