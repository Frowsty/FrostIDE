// SPDX-License-Identifier: Apache-2.0
//
// The byte-exact encoding fixtures (ADR 0018; tests/text/fixtures/encodings): each fixture's
// bytes as a hex array, its load options and every expectation of EncodingRoundTrip,
// FormatStatus.PerFixture, the integration tests and SaveManager's later fault-injection suite.
/*
 * Sources of truth. The UTF-16, UTF-32, windows-1252 and Shift_JIS bytes were produced with
 * CPython's codecs (a third implementation, besides Microsoft NLS and glibc) and are quoted in
 * encoding_fixtures.cpp; the expected texts are u8 literals. CPython serves only where it agrees
 * with Windows and glibc (ADR 0018, probe F7): the fixtures avoid 932's A0/FD/FE/FF, the 379 932
 * scalars CPython encodes differently and the 1252 C1 bytes, whose expectations (cp1252-c1,
 * cp932-nec-dup, invalid-utf8) come from the probe and the Unicode standard instead.
 *
 * Binary files cannot be typed into the repository, so these arrays are the source and
 * EncodingRoundTrip.FixturesMatchTable checks that the committed files equal them (which also
 * proves git's -text attribute and verify.sh's rsync kept them exact). With FROST_UPDATE_GOLDENS=1
 * that test writes the files and the fuzz seeds instead, and then fails.
 */
#pragma once

#include "frost/text/encoding.hpp"
#include "frost/text/encoding_detection.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/text/text_file.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text::test {

struct EncodingFixture {
    std::string name; // Also the case name; the file is name's "file" member.
    std::string file; // File name in tests/text/fixtures/encodings.
    std::string bytes;
    // Load options: legacyCodePage and defaultSeparator (Lf) are fixed per fixture.
    std::optional<Encoding> override;
    std::uint16_t legacyCodePage = 1252;
    // Expectations.
    Encoding encoding;
    bool bom = false;
    DetectionSource source = DetectionSource::Empty;
    std::uint8_t minConfidence = 0;
    EolStats eol;
    LineSeparator separator = LineSeparator::Lf;
    LossKind loss = LossKind::None;
    std::uint64_t replacements = 0;
    std::optional<std::uint64_t> firstLossFileOffset;
    std::optional<std::uint64_t> firstLossTextOffset;
    bool binaryLikely = false;
    std::string text;                // The decoded UTF-8.
    std::string consentBytes;        // Lossy fixtures: encodeForSave(AcceptLoss), or "" for InvalidArgument.
    bool consentUnencodable = false; // Lossy legacy fixtures: AcceptLoss is InvalidArgument.
};

// Every fixture, in a fixed order. Several entries may share one file (different options).
[[nodiscard]] const std::vector<EncodingFixture>& encodingFixtures();
// The fixture called `name`; aborts when there is none.
[[nodiscard]] const EncodingFixture& fixture(std::string_view name);
// The LoadOptions of a fixture: its override and legacy code page, defaultSeparator Lf.
[[nodiscard]] LoadOptions fixtureOptions(const EncodingFixture& fixture);

// The fuzz harness's three header bytes for a fixture seed: no override, the given codecs and
// legacy code page, no limit, the default sample (ADR 0018, the text_decode input format).
[[nodiscard]] std::string fuzzSeedHeader(const EncodingFixture& fixture);

} // namespace frost::text::test
