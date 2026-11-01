// SPDX-License-Identifier: Apache-2.0
//
// The input_keymap fuzz target's core: one input through the keymap parser and the
// sequence syntax, with their round-trip properties. Shaped like frost::fuzz::runInput,
// so the libFuzzer harness only forwards; until that harness exists, KeymapFuzzReplay
// replays the seed corpus (tests/fuzz/corpus/input_keymap) in every normal tree.
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <span>

namespace frost::input::test {

/*
 * Runs parseKeymap on `data`; when it parses, checks that formatKeymap is a fixpoint and
 * that parse(format(k)) == k. Runs parseSequence (Lenient and Strict) on every line, and
 * for each Strict success checks parse(format(s)) == s. Returns Internal describing the
 * first property failure; parse errors are expected results, not failures. Keeps no
 * state between inputs; thread-compatible.
 */
[[nodiscard]] frost::Result<void> runKeymapFuzzInput(std::span<const std::uint8_t> data);

} // namespace frost::input::test
