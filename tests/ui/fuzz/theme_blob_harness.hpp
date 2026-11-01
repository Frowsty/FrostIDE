// SPDX-License-Identifier: Apache-2.0
//
// The theme blob fuzz harness: decodes one input with decodeThemeBlob and checks the decoder's
// contract. Replayed over tests/fuzz/corpus/theme_blob by ui_model_tests (UiFuzzReplay.ThemeBlob).
// TODO(M1): after m1/piece-tree merges, add tests/fuzz/theme_blob_harness.cpp forwarding
// frost::fuzz::runInput here, plus theme_blob_fuzzer.cpp, registered with frost_add_fuzz_target.
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <span>

namespace frost::ui::fuzz {

/*
 * Runs one input. A rejection must be Corrupt or Unsupported; an accepted blob must re-encode to
 * exactly the input bytes (the encoding is canonical) and its data must pass Theme::fromData.
 * Returns Internal describing the first broken property; keeps no state between inputs.
 */
[[nodiscard]] Result<void> runThemeBlobInput(std::span<const std::uint8_t> data);

} // namespace frost::ui::fuzz
