// SPDX-License-Identifier: Apache-2.0
//
// Checks and generators shared by the encoding unit, stress and fuzz tests (ADR 0018, "Tests"):
// the load/save property of TextFile.LosslessIffRoundTrip (which the fuzz target's check (c)
// extends), its random inputs, and the golden comparison with FROST_UPDATE_GOLDENS and the dump
// directory that EncodingDifferential and EncodingDifferentialStress share.
/*
 * checkLoadAndSave(bytes, options, codecs, limitMode) runs, on one input:
 *   - decodeTextFile succeeds (OutOfRange is accepted only in limit mode);
 *   - the buffer's invariants hold, the format is valid, and eol equals oracleEolStats(text);
 *   - decodeBytes gives the same text, encoding, eol and attempts;
 *   - exactly one attempt is Accepted; without an override, detection is deterministic, its
 *     candidates are non-empty and every confidence is at most 100;
 *   - lossless implies encodeForSave(None) == bytes; lossy implies encodeForSave(None) is Corrupt
 *     and encodeForSave(AcceptLoss) is either != bytes or InvalidArgument;
 *   - encodeForSaveTo's pieces (each at most 64 KiB) concatenate to encodeForSave's result, with
 *     both consents;
 *   - encodeForSave(AcceptLoss) either succeeds, and decoding its bytes with an override of the
 *     loaded encoding is lossless with the same text, or fails with InvalidArgument exactly when
 *     firstUnencodable finds a character.
 * Failures are Internal errors with a description. Not thread-safe beyond what `codecs` allows.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/encoding.hpp"
#include "frost/text/text_codec.hpp"
#include "frost/text/text_file.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text::test {

// The overrides the property and the fuzz target use: 0 none, 1 UTF-8, 2 UTF-16LE, 3 UTF-16BE,
// 4 UTF-32LE, 5 UTF-32BE, 6 windows-1252, 7 Shift_JIS. Precondition: index < 8.
[[nodiscard]] std::optional<Encoding> overrideByIndex(unsigned index);

// A random file of at most `maxBytes` bytes: valid UTF-8 with line breaks, UTF-16 or UTF-32 text
// with or without a BOM, windows-1252-like or Shift_JIS-like bytes, or junk, each sometimes with
// junk bytes spliced in.
[[nodiscard]] std::string randomEncodingInput(sched::detail::SplitMix64& rng, std::size_t maxBytes);

// The checks above. `limitMode`: options.buffer.maxBufferBytes is small and OutOfRange is allowed.
[[nodiscard]] Result<void> checkLoadAndSave(std::string_view bytes, const LoadOptions& options,
                                            ITextCodecProvider& codecs, bool limitMode);

// One canonical table behind a fingerprint line, dumped on a mismatch.
struct CanonicalTable {
    std::string name;    // "cp932.decode", for example.
    std::string content; // One line per unit or scalar.
};

/*
 * Compares `lines` with the golden file goldensDir/file, ignoring the golden's first line
 * ("generated-on <os> <date>").
 *   - FROST_UPDATE_GOLDENS=1: writes the golden (with a fresh generated-on line) and returns an
 *     error "goldens rewritten; rerun without FROST_UPDATE_GOLDENS", so an update run never passes.
 *   - On a mismatch: writes every table to dumpDir/<name>.<os>.txt and returns an error that lists
 *     the differing lines, the dump paths and how to diff them across OSes.
 */
[[nodiscard]] Result<void> checkGolden(std::string_view goldensDir, std::string_view dumpDir, std::string_view file,
                                       std::span<const std::string> lines, std::span<const CanonicalTable> tables);

// The seeds of a property test: [0, count), or only s when FROST_ENCODING_SEED=<s> is set.
struct SeedRange {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    bool single = false;
};
[[nodiscard]] SeedRange encodingSeeds(std::uint64_t count);
// The command that reproduces one failing seed: "FROST_ENCODING_SEED=7 <executable> --gtest_filter=<test>".
[[nodiscard]] std::string reproduceSeed(std::string_view executable, std::string_view test, std::uint64_t seed);

// "windows" or "linux".
[[nodiscard]] std::string_view osName();
// True when FROST_UPDATE_GOLDENS=1.
[[nodiscard]] bool updatingGoldens();

} // namespace frost::text::test
