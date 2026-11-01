// SPDX-License-Identifier: Apache-2.0
//
// The icon atlas fuzz harness: parses one input with IconAtlasView::parse and checks the
// parser's contract. Replayed over tests/fuzz/corpus/icon_atlas by ui_model_tests
// (UiFuzzReplay.IconAtlas).
// TODO(M1): after m1/piece-tree merges, add tests/fuzz/icon_atlas_harness.cpp forwarding
// frost::fuzz::runInput here, plus icon_atlas_fuzzer.cpp, registered with frost_add_fuzz_target.
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <span>

namespace frost::ui::fuzz {

/*
 * Runs one input. A rejection must be Corrupt or Unsupported. An accepted atlas must answer
 * find() for every entry with that entry, keep every page and cell inside the input, and
 * re-write (AtlasWriter over its own entries and cells) to exactly the input bytes.
 * Returns Internal describing the first broken property; keeps no state between inputs.
 */
[[nodiscard]] Result<void> runIconAtlasInput(std::span<const std::uint8_t> data);

} // namespace frost::ui::fuzz
