// SPDX-License-Identifier: Apache-2.0
// Actual clang-tidy export YAML decoding stays in the pinned LLVM process, outside app CRT layers.
#pragma once
namespace frost::clang_engine {
[[nodiscard]] int runTidyFixesDecode();
}
