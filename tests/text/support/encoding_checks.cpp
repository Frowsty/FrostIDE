// SPDX-License-Identifier: Apache-2.0
//
// The shared load/save property, its random inputs and the golden comparison (encoding_checks.hpp).

#include "text/support/encoding_checks.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/utf.hpp"
#include "frost/text/encoding_detection.hpp"
#include "frost/text/line_separator.hpp"

#include "support/test_env.hpp"
#include "text/support/encoding_oracle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <utility>

namespace frost::text::test {
namespace {

using sched::detail::SplitMix64;

std::unexpected<Error> broken(std::string what) { return fail(ErrorCode::Internal, std::move(what)); }

std::string describe(const Result<std::string>& r) {
    return r ? std::format("{} bytes", r->size()) : r.error().describe();
}

// Random well-formed UTF-8 text: code-like ASCII, line breaks of every kind and some multi-byte
// characters of every length.
std::string randomText(SplitMix64& rng, std::size_t maxBytes) {
    std::string out;
    const std::size_t target = static_cast<std::size_t>(rng.below(maxBytes + 1));
    while (out.size() < target) {
        const std::uint64_t pick = rng.below(20);
        if (pick < 12) {
            out.push_back(static_cast<char>(0x20 + rng.below(0x5F)));
        } else if (pick == 12) {
            out.append(rng.below(2) == 0 ? "\r\n" : rng.below(2) == 0 ? "\n" : "\r");
        } else if (pick == 13) {
            oracleAppendUtf8(static_cast<char32_t>(0x80 + rng.below(0x780)), out);
        } else if (pick == 14) {
            oracleAppendUtf8(static_cast<char32_t>(0x3000 + rng.below(0x6000)), out);
        } else if (pick == 15) {
            oracleAppendUtf8(static_cast<char32_t>(0x10000 + rng.below(0x100000)), out);
        } else if (pick == 16) {
            oracleAppendUtf8(static_cast<char32_t>(0xE000 + rng.below(0x2000)), out); // PUA, U+FFFD region.
        } else if (pick == 17) {
            out.push_back('\0');
        } else {
            out.append(rng.below(2) == 0 ? "int x = 0;" : "// comment");
        }
    }
    return out;
}

void spliceJunk(SplitMix64& rng, std::string& bytes) {
    const std::uint64_t count = rng.below(4);
    for (std::uint64_t k = 0; k < count && !bytes.empty(); ++k) {
        const std::size_t at = static_cast<std::size_t>(rng.below(bytes.size() + 1));
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), static_cast<char>(0x80 + rng.below(0x80)));
    }
}

void truncateTo(std::string& bytes, std::size_t maxBytes) {
    if (bytes.size() > maxBytes) {
        bytes.resize(maxBytes);
    }
}

bool sameAttempts(const std::vector<CandidateAttempt>& a, const std::vector<CandidateAttempt>& b) { return a == b; }

std::string attemptsText(const std::vector<CandidateAttempt>& tried) {
    std::string out;
    for (const CandidateAttempt& a : tried) {
        out += std::format("[{} bom={} conf={} {} outcome={} loss={}] ", encodingName(a.guess.encoding), a.guess.bom,
                           a.guess.confidence, detectionSourceName(a.guess.source), static_cast<int>(a.outcome),
                           lossKindName(a.fidelity.loss));
    }
    return out;
}

// encodeForSaveTo into one string, checking the piece sizes.
Result<std::string> encodeThroughSink(const Snapshot& text, const DocumentEncoding& state, SaveConsent consent,
                                      ITextCodecProvider& codecs) {
    std::string out;
    bool oversized = false;
    auto sink = [&](std::string_view piece) -> Result<void> {
        if (piece.size() > 64u * 1024u) {
            oversized = true;
        }
        out.append(piece);
        return {};
    };
    auto r = encodeForSaveTo(text, state, consent, sink, codecs);
    if (!r) {
        return std::unexpected(std::move(r).error());
    }
    if (oversized) {
        return broken("encodeForSaveTo delivered a piece larger than 64 KiB");
    }
    return out;
}

bool sameOutcome(const Result<std::string>& a, const Result<std::string>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    return a ? *a == *b : a.error().code() == b.error().code();
}

} // namespace

std::optional<Encoding> overrideByIndex(unsigned index) {
    switch (index) {
    case 1: return Encoding::utf8();
    case 2: return Encoding::unicode(EncodingForm::Utf16Le);
    case 3: return Encoding::unicode(EncodingForm::Utf16Be);
    case 4: return Encoding::unicode(EncodingForm::Utf32Le);
    case 5: return Encoding::unicode(EncodingForm::Utf32Be);
    case 6: return Encoding::legacy(1252);
    case 7: return Encoding::legacy(932);
    default: return std::nullopt;
    }
}

std::string randomEncodingInput(SplitMix64& rng, std::size_t maxBytes) {
    std::string bytes;
    switch (rng.below(8)) {
    case 0: // UTF-8, sometimes with a BOM.
        bytes = randomText(rng, maxBytes);
        if (rng.below(4) == 0) {
            bytes.insert(0, "\xEF\xBB\xBF");
        }
        break;
    case 1:
    case 2: { // UTF-16 in either order, with or without a BOM.
        const bool big = rng.below(2) == 0;
        bytes = oracleEncodeUtf16(randomText(rng, maxBytes / 2), big);
        if (rng.below(2) == 0) {
            bytes.insert(0, big ? "\xFE\xFF" : "\xFF\xFE");
        }
        break;
    }
    case 3: { // UTF-32 with a BOM.
        const bool big = rng.below(2) == 0;
        bytes = oracleEncodeUtf32(randomText(rng, maxBytes / 4), big);
        bytes.insert(0, big ? std::string_view("\x00\x00\xFE\xFF", 4) : std::string_view("\xFF\xFE\x00\x00", 4));
        break;
    }
    case 4: { // windows-1252-like: printable bytes of both halves.
        const std::size_t n = static_cast<std::size_t>(rng.below(maxBytes + 1));
        for (std::size_t i = 0; i < n; ++i) {
            bytes.push_back(rng.below(3) == 0 ? static_cast<char>(0x80 + rng.below(0x80))
                                              : static_cast<char>(0x20 + rng.below(0x5F)));
        }
        break;
    }
    case 5: { // Shift_JIS-like: ASCII, half-width katakana and lead/trail pairs.
        const std::size_t n = static_cast<std::size_t>(rng.below(maxBytes + 1));
        while (bytes.size() < n) {
            const std::uint64_t pick = rng.below(4);
            if (pick == 0) {
                bytes.push_back(static_cast<char>(0x20 + rng.below(0x5F)));
            } else if (pick == 1) {
                bytes.push_back(static_cast<char>(0xA1 + rng.below(0x3F)));
            } else {
                const std::uint64_t lead = rng.below(2) == 0 ? 0x81 + rng.below(0x1F) : 0xE0 + rng.below(0x1D);
                bytes.push_back(static_cast<char>(lead));
                bytes.push_back(static_cast<char>(0x40 + rng.below(0xBD)));
            }
        }
        break;
    }
    case 6: { // Junk, biased toward the bytes decoders care about.
        constexpr unsigned char kBiased[] = {0x00, 0x0A, 0x0D, 0x41, 0x80, 0x81, 0x9F, 0xA0, 0xC0,
                                             0xC2, 0xE0, 0xED, 0xEF, 0xF0, 0xF4, 0xFD, 0xFE, 0xFF};
        const std::size_t n = static_cast<std::size_t>(rng.below(maxBytes + 1));
        for (std::size_t i = 0; i < n; ++i) {
            bytes.push_back(rng.below(2) == 0 ? static_cast<char>(kBiased[rng.below(std::size(kBiased))])
                                              : static_cast<char>(rng.below(256)));
        }
        break;
    }
    default: // Short inputs around the BOM rules.
        bytes = std::string(
            std::string_view("\xFF\xFE\x00\x00\xFE\xFF\xEF\xBB\xBF", 9).substr(rng.below(7), 2 + rng.below(3)));
        bytes += randomText(rng, 8);
        break;
    }
    if (rng.below(3) == 0) {
        spliceJunk(rng, bytes);
    }
    truncateTo(bytes, maxBytes);
    return bytes;
}

Result<void> checkLoadAndSave(std::string_view bytes, const LoadOptions& options, ITextCodecProvider& codecs,
                              bool limitMode) {
    auto loaded = decodeTextFile(std::string(bytes), options, codecs);
    if (!loaded) {
        if (limitMode && loaded.error().code() == ErrorCode::OutOfRange) {
            return {};
        }
        return broken("decodeTextFile failed: " + loaded.error().describe());
    }
    const Snapshot snapshot = loaded->buffer.snapshot();
    if (auto ok = loaded->buffer.checkInvariants(); !ok) {
        return broken("the loaded buffer breaks its invariants: " + ok.error().describe());
    }
    const DocumentEncoding& state = loaded->encoding;
    if (!state.format.isValid()) {
        return broken("the loaded format is not valid");
    }
    const std::string text = snapshot.text();
    if (loaded->eol != oracleEolStats(text)) {
        return broken("eol differs from oracleEolStats");
    }

    // decodeBytes agrees with decodeTextFile.
    auto decoded = decodeBytes(bytes, options, codecs);
    if (!decoded) {
        return broken("decodeBytes failed where decodeTextFile succeeded: " + decoded.error().describe());
    }
    const std::string_view decodedText =
        decoded->payloadIsText ? bytes.substr(decoded->payloadOffset) : std::string_view(decoded->text);
    if (decodedText != text || !(decoded->encoding == state) || decoded->eol != loaded->eol ||
        !sameAttempts(decoded->tried, loaded->tried) || decoded->binaryLikely != loaded->binaryLikely) {
        return broken("decodeBytes and decodeTextFile disagree");
    }
    if (decoded->payloadIsText && !decoded->text.empty()) {
        return broken("payloadIsText with a non-empty text");
    }

    // Attempts and detection.
    const auto accepted = std::count_if(loaded->tried.begin(), loaded->tried.end(), [](const CandidateAttempt& a) {
        return a.outcome == AttemptOutcome::Accepted;
    });
    if (accepted != 1) {
        return broken(std::format("{} attempts are Accepted: {}", accepted, attemptsText(loaded->tried)));
    }
    if (state.detection.confidence > 100) {
        return broken("confidence above 100");
    }
    if (!options.encoding) {
        const EncodingDetection first = detectEncoding(bytes, options.detect, codecs);
        const EncodingDetection second = detectEncoding(bytes, options.detect, codecs);
        if (first.candidates.empty() || first.candidates.size() > 4) {
            return broken(std::format("detection returned {} candidates", first.candidates.size()));
        }
        if (first.candidates != second.candidates || first.binaryLikely != second.binaryLikely) {
            return broken("detection is not deterministic");
        }
        for (const EncodingGuess& g : first.candidates) {
            if (g.confidence > 100) {
                return broken("a candidate's confidence is above 100");
            }
        }
    }

    // The save guard and the lossless implications.
    const Result<std::string> none = encodeForSave(snapshot, state, SaveConsent::None, codecs);
    const Result<std::string> accept = encodeForSave(snapshot, state, SaveConsent::AcceptLoss, codecs);
    if (state.fidelity.lossless()) {
        if (!none || *none != bytes) {
            return broken(std::format("a lossless load ({}) does not save back to its bytes: {}",
                                      encodingName(state.format.encoding), describe(none)));
        }
    } else {
        if (none || none.error().code() != ErrorCode::Corrupt) {
            return broken("a lossy load saves without consent: " + describe(none));
        }
        if (accept && *accept == bytes) {
            return broken("a lossy load saves back to its own bytes");
        }
        if (!accept && accept.error().code() != ErrorCode::InvalidArgument) {
            return broken("AcceptLoss failed with an unexpected error: " + accept.error().describe());
        }
    }

    // The sink path delivers the same bytes.
    if (!sameOutcome(encodeThroughSink(snapshot, state, SaveConsent::None, codecs), none) ||
        !sameOutcome(encodeThroughSink(snapshot, state, SaveConsent::AcceptLoss, codecs), accept)) {
        return broken("encodeForSaveTo and encodeForSave disagree");
    }

    // AcceptLoss either re-decodes losslessly or fails exactly at an unencodable character.
    auto unencodable = firstUnencodable(snapshot, state.format.encoding, codecs);
    if (!unencodable) {
        return broken("firstUnencodable failed: " + unencodable.error().describe());
    }
    if (accept) {
        if (*unencodable) {
            return broken("AcceptLoss succeeded although firstUnencodable found a character");
        }
        LoadOptions again = options;
        again.encoding = state.format.encoding;
        auto redecoded = decodeBytes(*accept, again, codecs);
        if (!redecoded) {
            return broken("re-decoding the saved bytes failed: " + redecoded.error().describe());
        }
        const std::string_view redecodedText = redecoded->payloadIsText
                                                   ? std::string_view(*accept).substr(redecoded->payloadOffset)
                                                   : std::string_view(redecoded->text);
        if (redecodedText != text || !redecoded->encoding.fidelity.lossless() ||
            redecoded->encoding.format.encoding != state.format.encoding ||
            redecoded->encoding.format.bom != state.format.bom) {
            return broken("the saved bytes do not re-decode losslessly to the same text");
        }
    } else if (accept.error().code() == ErrorCode::InvalidArgument) {
        if (!*unencodable) {
            return broken("AcceptLoss is InvalidArgument but firstUnencodable finds nothing: " +
                          accept.error().describe());
        }
    } else {
        return broken("AcceptLoss failed: " + accept.error().describe());
    }
    return {};
}

SeedRange encodingSeeds(std::uint64_t count) {
    const std::string value = frost::test::environment("FROST_ENCODING_SEED");
    if (!value.empty()) {
        std::uint64_t seed = 0;
        for (const char c : value) {
            if (c < '0' || c > '9') {
                std::fprintf(stderr, "FROST_ENCODING_SEED must be a decimal number, not '%s'\n", value.c_str());
                std::abort();
            }
            seed = seed * 10 + static_cast<std::uint64_t>(c - '0');
        }
        return {seed, seed + 1, true};
    }
    return {0, count, false};
}

std::string reproduceSeed(std::string_view executable, std::string_view test, std::uint64_t seed) {
    return std::format("reproduce with: FROST_ENCODING_SEED={} {} --gtest_filter={}", seed, executable, test);
}

std::string_view osName() {
#if defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

bool updatingGoldens() { return frost::test::environment("FROST_UPDATE_GOLDENS") == "1"; }

Result<void> checkGolden(std::string_view goldensDir, std::string_view dumpDir, std::string_view file,
                         std::span<const std::string> lines, std::span<const CanonicalTable> tables) {
    io::IFileSystem& fs = io::realFileSystem();
    const std::string path = std::format("{}/{}", goldensDir, file);
    if (updatingGoldens()) {
        const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
        std::string content = std::format("generated-on {} {:%F}\n", osName(), std::chrono::year_month_day{today});
        for (const std::string& line : lines) {
            content += line;
            content += '\n';
        }
        if (auto made = fs.createDirectories(goldensDir); !made) {
            return std::unexpected(std::move(made).error());
        }
        if (auto written = fs.writeFileAtomic(path, content); !written) {
            return std::unexpected(std::move(written).error());
        }
        return fail(ErrorCode::Internal,
                    std::format("goldens rewritten ({}); rerun without FROST_UPDATE_GOLDENS", path));
    }
    auto golden = fs.readFile(path);
    if (!golden) {
        return std::unexpected(std::move(golden).error().withContext("reading the golden " + path +
                                                                     " (generate it with FROST_UPDATE_GOLDENS=1)"));
    }
    std::vector<std::string> expected;
    std::string_view rest = *golden;
    bool first = true;
    while (!rest.empty()) {
        const std::size_t eol = rest.find('\n');
        std::string_view line = rest.substr(0, eol);
        rest = eol == std::string_view::npos ? std::string_view() : rest.substr(eol + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (first) {
            first = false;
            if (line.starts_with("generated-on ")) {
                continue;
            }
        }
        expected.emplace_back(line);
    }
    std::string differences;
    const std::size_t count = std::max(expected.size(), lines.size());
    for (std::size_t i = 0; i < count; ++i) {
        const std::string_view want = i < expected.size() ? std::string_view(expected[i]) : "<missing>";
        const std::string_view got = i < lines.size() ? std::string_view(lines[i]) : "<missing>";
        if (want != got) {
            differences += std::format("\n  golden: {}\n  actual: {}", want, got);
        }
    }
    if (differences.empty()) {
        return {};
    }
    std::string dumped;
    if (auto made = fs.createDirectories(dumpDir); made) {
        for (const CanonicalTable& table : tables) {
            const std::string dumpPath = std::format("{}/{}.{}.txt", dumpDir, table.name, osName());
            if (fs.writeFileAtomic(dumpPath, table.content)) {
                dumped += "\n  " + dumpPath;
            }
        }
    }
    return fail(
        ErrorCode::Corrupt,
        std::format("{} differs from the golden:{}\nCanonical tables written to:{}\nCompare them with the other "
                    "OS's dump, for example: diff <dir>/cp932.decode.windows.txt <dir>/cp932.decode.linux.txt",
                    file, differences, dumped));
}

} // namespace frost::text::test
