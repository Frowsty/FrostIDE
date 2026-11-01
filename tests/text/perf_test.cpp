// SPDX-License-Identifier: Apache-2.0
//
// Micro-benchmarks for frost::text (label `perf`, modelled on sched_perf_tests). The perf label
// runs in every -Quick, so Debug and sanitizer builds use a fixed small scale (a 1 MiB document,
// 2k ops) and only report; optimised unsanitised builds run the full scale.
/*
 * TextPerf.EditLatency: a 16 MiB code-like document, half of its lines ending in CRLF. 100k typing
 * ops (local inserts and backspaces, with occasional jumps), each followed by what the editor does
 * per keystroke: snapshot(), pointAt(cursor) and a LineCursor over the 60 visible lines. Reports
 * p50/p99/max; asserts p99 <= 0.25 ms in optimised unsanitised builds. Budget: B5. That is the
 * text-side sub-budget of B5 recorded in ADR 0012: 1/32 of B5's p99 limit (8 ms) and 1/8 of its
 * p50 goal (2 ms), leaving the rest of the keystroke for layout and paint.
 *
 * TextPerf.LoadThroughput: fromUtf8 on 256 MiB (optimised unsanitised) or 16 MiB. Reports GB/s,
 * bytes per piece and tree memory, and projects them to F1 (a 2 GB file) for B15's +64 MB and 2 s
 * line-index limits (ADR 0012, D4 risk). Report only; m1/large-file gates B15. Budget: B15.
 *
 * TextPerf.SeekLatency: random pointAt, offsetAt, utf16PointAt and lineInfo queries on the 16 MiB
 * document. Reports p50/p99, nodes and bytes per piece.
 *
 * The tests include src/text/src/piece_tree.hpp (private) for detail::treeStats.
 */

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"

#include "piece_tree.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ByteColumn;
using frost::ByteOffset;
using frost::LineIndex;
using frost::Result;
using frost::Utf16Column;
using frost::sched::detail::SplitMix64;
using frost::test::kAssertBudgets;
using frost::text::ByteRange;
using frost::text::LineCursor;
using frost::text::LineInfo;
using frost::text::Point;
using frost::text::TextBuffer;
using frost::text::Utf16Point;
using Clock = std::chrono::steady_clock;

struct Percentiles {
    double p50us = 0;
    double p99us = 0;
    double maxUs = 0;
};

Percentiles percentiles(std::vector<std::int64_t> samplesNs) {
    std::sort(samplesNs.begin(), samplesNs.end());
    const auto at = [&](double q) {
        const auto index = static_cast<std::size_t>(q * static_cast<double>(samplesNs.size() - 1));
        return static_cast<double>(samplesNs[index]) / 1000.0;
    };
    return {at(0.50), at(0.99), static_cast<double>(samplesNs.back()) / 1000.0};
}

std::int64_t elapsedNs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

// A code-like document of about `bytes` bytes: indented lines of identifiers, punctuation and
// comments, half of them ending in CRLF, a few with non-ASCII characters.
std::string codeLike(std::size_t bytes) {
    constexpr std::string_view kWords[] = {
        "auto", "const", "return", "if", "for", "value", "index", "std::size_t", "result", "(",
        ")",    "{",     "}",      ";",  "=",   "+",     "->",    "// note",     "\"é\"",  "u8\"\xE2\x82\xAC\""};
    SplitMix64 rng(16);
    std::string text;
    text.reserve(bytes + 256);
    while (text.size() < bytes) {
        text.append(4 * static_cast<std::size_t>(rng.below(5)), ' ');
        for (std::uint64_t words = 2 + rng.below(10); words > 0; --words) {
            text += kWords[rng.below(std::size(kWords))];
            text += ' ';
        }
        text += rng.below(2) == 0 ? "\r\n" : "\n";
    }
    return text;
}

constexpr bool kFullScale = kAssertBudgets;
constexpr std::size_t kDocumentBytes = kFullScale ? std::size_t{16} << 20 : std::size_t{1} << 20;

TEST(TextPerf, EditLatency) {
    const int ops = kFullScale ? 100'000 : 2'000;
    Result<TextBuffer> loaded = TextBuffer::fromUtf8(codeLike(kDocumentBytes));
    ASSERT_TRUE(loaded.has_value());
    TextBuffer& buffer = *loaded;
    SplitMix64 rng(5);
    std::vector<std::int64_t> samples;
    samples.reserve(static_cast<std::size_t>(ops));
    // The cursor starts at a random line; typing inserts there, backspace erases the char before.
    const auto lineStart = [&](std::uint32_t line) {
        return buffer.current().lineInfo(LineIndex{line})->content.begin;
    };
    ByteOffset cursor = lineStart(static_cast<std::uint32_t>(rng.below(buffer.current().lineCount())));
    std::uint64_t checksum = 0;
    for (int op = 0; op < ops; ++op) {
        const std::uint64_t roll = rng.below(100);
        if (roll == 0) {
            cursor = lineStart(static_cast<std::uint32_t>(rng.below(buffer.current().lineCount())));
        }
        const bool backspace = roll >= 85 && cursor.value() > 0;
        const auto start = Clock::now();
        if (backspace) {
            const ByteOffset before = buffer.current().clip(ByteOffset{cursor.value() - 1});
            const auto change = buffer.erase(ByteRange{before, cursor});
            if (!change) {
                FAIL() << change.error().describe();
            }
            cursor = before;
        } else {
            const char typed[2] = {static_cast<char>('a' + rng.below(26)), 0};
            const auto change = buffer.insert(cursor, std::string_view(typed, 1));
            if (!change) {
                FAIL() << change.error().describe();
            }
            cursor = cursor + 1;
        }
        const frost::text::Snapshot snapshot = buffer.snapshot();
        const Point point = snapshot.pointAt(cursor);
        const std::uint32_t first = point.line.value() >= 30 ? point.line.value() - 30 : 0;
        Result<LineCursor> lines = snapshot.linesFrom(LineIndex{first});
        LineInfo line;
        for (int visible = 0; visible < 60 && lines->next(line); ++visible) {
            checksum += line.content.end.value();
        }
        samples.push_back(elapsedNs(start));
    }
    const Percentiles p = percentiles(samples);
    std::printf("[text perf] EditLatency (%s, %zu-byte document, %d ops): p50 %.1f us, p99 %.1f us, max %.1f us "
                "(checksum %llu)\n",
                kAssertBudgets ? "budgeted" : "report-only", kDocumentBytes, ops, p.p50us, p.p99us, p.maxUs,
                static_cast<unsigned long long>(checksum));
    ASSERT_TRUE(buffer.checkInvariants().has_value());
    if (kAssertBudgets) {
        // Budget: B5 (text-side sub-budget, ADR 0012).
        EXPECT_LE(p.p99us, 250.0) << "keystroke p99 above the 0.25 ms text sub-budget of B5";
    }
}

TEST(TextPerf, LoadThroughput) {
    const std::size_t bytes = kFullScale ? std::size_t{256} << 20 : std::size_t{16} << 20;
    std::string text = codeLike(bytes);
    const std::size_t size = text.size();
    const auto start = Clock::now();
    Result<TextBuffer> loaded = TextBuffer::fromUtf8(std::move(text));
    const double seconds = static_cast<double>(elapsedNs(start)) / 1e9;
    ASSERT_TRUE(loaded.has_value());
    const frost::text::detail::TreeStats stats = frost::text::detail::treeStats(loaded->current().pieceTree());
    const double gbPerSecond = static_cast<double>(size) / seconds / 1e9;
    const double nodeBytesPerByte = static_cast<double>(stats.nodeBytes) / static_cast<double>(size);
    constexpr double kF1Bytes = 2e9;
    std::printf("[text perf] LoadThroughput (%s): %zu bytes in %.3f s = %.2f GB/s; %zu pieces (%.0f bytes per piece), "
                "%zu leaves, %zu internal nodes, %.1f MB of nodes (%.4f bytes per text byte)\n",
                kFullScale ? "full scale" : "small scale", size, seconds, gbPerSecond, stats.pieces,
                static_cast<double>(size) / static_cast<double>(stats.pieces), stats.leaves, stats.internals,
                static_cast<double>(stats.nodeBytes) / 1e6, nodeBytesPerByte);
    std::printf("[text perf] F1 projection (2 GB): load %.2f s at this rate, %.1f MB of tree nodes (B15 allows +64 MB "
                "of private bytes and 2 s for the line index; m1/large-file gates it)\n",
                kF1Bytes / (gbPerSecond * 1e9), kF1Bytes * nodeBytesPerByte / 1e6);
}

TEST(TextPerf, SeekLatency) {
    const int queries = kFullScale ? 200'000 : 4'000;
    Result<TextBuffer> loaded = TextBuffer::fromUtf8(codeLike(kDocumentBytes));
    ASSERT_TRUE(loaded.has_value());
    const frost::text::Snapshot s = loaded->snapshot();
    SplitMix64 rng(17);
    std::vector<std::int64_t> pointSamples;
    std::vector<std::int64_t> offsetSamples;
    std::vector<std::int64_t> utf16Samples;
    std::vector<std::int64_t> lineSamples;
    std::uint64_t checksum = 0;
    for (int q = 0; q < queries; ++q) {
        const ByteOffset x{rng.below(s.size().value() + 1)};
        const auto line = static_cast<std::uint32_t>(rng.below(s.lineCount()));
        auto start = Clock::now();
        checksum += s.pointAt(x).column.value();
        pointSamples.push_back(elapsedNs(start));
        start = Clock::now();
        checksum += s.offsetAt(Point{LineIndex{line}, ByteColumn{7}})->value();
        offsetSamples.push_back(elapsedNs(start));
        start = Clock::now();
        checksum += s.offsetAt(Utf16Point{LineIndex{line}, Utf16Column{7}})->value();
        checksum += s.utf16PointAt(x).column.value();
        utf16Samples.push_back(elapsedNs(start));
        start = Clock::now();
        checksum += s.lineInfo(LineIndex{line})->content.end.value();
        lineSamples.push_back(elapsedNs(start));
    }
    const frost::text::detail::TreeStats stats = frost::text::detail::treeStats(s.pieceTree());
    const auto report = [](const char* name, std::vector<std::int64_t> samples) {
        const Percentiles p = percentiles(std::move(samples));
        std::printf("[text perf] SeekLatency %-28s p50 %.2f us, p99 %.2f us, max %.1f us\n", name, p.p50us, p.p99us,
                    p.maxUs);
    };
    report("pointAt:", pointSamples);
    report("offsetAt(Point):", offsetSamples);
    report("offsetAt(Utf16Point)+utf16PointAt:", utf16Samples);
    report("lineInfo:", lineSamples);
    std::printf("[text perf] SeekLatency on %llu bytes, %u lines: %zu pieces (%.0f bytes per piece), height %u, "
                "%zu nodes (checksum %llu)\n",
                static_cast<unsigned long long>(s.size().value()), s.lineCount(), stats.pieces,
                static_cast<double>(s.size().value()) / static_cast<double>(stats.pieces), s.pieceTree().height(),
                stats.leaves + stats.internals, static_cast<unsigned long long>(checksum));
}

} // namespace
