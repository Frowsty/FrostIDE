// SPDX-License-Identifier: Apache-2.0
//
// Fuzz harness for frost::text's piece tree (ADR 0012): the input drives the same OpDriver as the
// differential and stress tests (tests/text/support/text_ops.hpp), with choices read from the
// input bytes instead of a seed.
/*
 * Input format:
 *   byte 0          maxPieceBytes = {4, 5, 7, 16, 64, 4096}[byte % 6];
 *   byte 1          the initial text's length L (0..255);
 *   bytes 2..2+L    the initial text, loaded with TextBuffer::fromUtf8. Ill-formed text must be
 *                   rejected exactly when the oracle's validator rejects it, with the matching
 *                   error and offset; the run then starts empty;
 *   the rest        the OpDriver's choices (FuzzSource), until the input runs out (the driver
 *                   stops at the next op boundary) or after 512 ops.
 * Documents stay within 64 KiB; a full check runs every 32 ops (16 random queries) and a sweep of
 * every line ends each input. An empty input runs an empty buffer through the final check.
 */

#include "fuzz/fuzz_harness.hpp"
#include "support/crash_dialogs.hpp"
#include "text/support/text_ops.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>

namespace frost::fuzz {

void initialize() { frost::test::disableCrashDialogs(); }

Result<void> runInput(std::span<const std::uint8_t> data) {
    using frost::text::test::DriverOptions;
    using frost::text::test::FuzzSource;
    using frost::text::test::OpDriver;
    constexpr std::uint32_t kCaps[] = {4, 5, 7, 16, 64, 4096};
    DriverOptions options;
    options.buffer.maxPieceBytes = kCaps[(data.empty() ? 0 : data[0]) % std::size(kCaps)];
    options.checkEvery = 32;
    options.queriesPerCheck = 16;
    options.maxOps = 512;
    options.maxDocBytes = 64 * 1024;
    const std::size_t length = data.size() > 1 ? data[1] : 0;
    const std::span<const std::uint8_t> rest = data.size() > 2 ? data.subspan(2) : std::span<const std::uint8_t>();
    const std::span<const std::uint8_t> initial = rest.first(std::min(length, rest.size()));
    OpDriver driver(options, "fuzz input");
    if (auto loaded = driver.load(std::string(initial.begin(), initial.end())); !loaded) {
        return loaded;
    }
    FuzzSource source(rest.subspan(initial.size()));
    while (!driver.done() && !source.exhausted()) {
        if (auto stepped = driver.step(source); !stepped) {
            return stepped;
        }
    }
    return driver.verifyFull(true);
}

} // namespace frost::fuzz
