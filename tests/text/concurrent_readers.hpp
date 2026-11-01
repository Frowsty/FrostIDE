// SPDX-License-Identifier: Apache-2.0
//
// The concurrent-reader scenario shared by TextConcurrency.ReadersDuringEdits (unit scale) and
// TextStress.ConcurrentReaders (stress scale): one writer edits a TextBuffer and publishes each
// version's Snapshot while readers query, copy, swap and release snapshots on their own threads
// (ADR 0012, D7). TSan in the WSL -Full tree checks the reference counting and publication.
/*
 * The writer keeps a std::string mirror. Before publishing version v it stores the mirror's size
 * for every v and, for every 64th v, the oracle's summary and an FNV-1a hash of the mirror, all in
 * preallocated arrays; then it publishes the Snapshot into a mutex-guarded slot. The mutex orders
 * the array writes before any reader's copy of that snapshot, also when the snapshot reaches a
 * reader through the swap slot.
 *
 * Each reader iteration copies the published snapshot under the mutex and then, outside it:
 * checks size() against the array; for every 64th version recomputes summarize(text()), the hash
 * over chunks() and checkInvariants(), and compares them with the arrays and summary(); iterates
 * up to 32 lines with a LineCursor against lineInfo(); checks the round-trip rules at 16 random
 * offsets; keeps the copy in a ring of 8 so releases (and the last release of old nodes) run on
 * reader threads; and swaps one ring entry with the second slot, so copies cross threads. Loops are
 * bounded by the writer's edit count; there are no sleeps.
 */
#pragma once

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/text/text_summary.hpp"

#include "text/support/text_ops.hpp"
#include "text/support/text_oracle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace frost::text::test {

struct ConcurrentRun {
    std::uint32_t edits = 20'000;
    std::uint64_t maxDocBytes = 16 * 1024;
    std::uint64_t seed = 1;
};

struct ConcurrentResult {
    unsigned readers = 0;
    std::uint64_t readerIterations = 0;
    std::uint64_t deepChecks = 0; // Versions checked against the stored summary and hash.
    std::string failure;          // Empty on success.
};

inline std::uint64_t fnv1a(std::uint64_t hash, std::string_view bytes) noexcept {
    for (const char c : bytes) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 0x100'0000'01B3ULL;
    }
    return hash;
}
inline constexpr std::uint64_t kFnvOffset = 0xCBF2'9CE4'8422'2325ULL;

inline ConcurrentResult runConcurrentReaders(const ConcurrentRun& run) {
    const unsigned hardware = std::thread::hardware_concurrency();
    ConcurrentResult result;
    result.readers = std::max(1u, std::min(4u, hardware > 1 ? hardware - 1 : 1u));

    // Arrays indexed by version, written before the version is published.
    std::vector<std::uint64_t> sizes(run.edits + 1);
    std::vector<TextSummary> summaries(run.edits / 64 + 1);
    std::vector<std::uint64_t> hashes(run.edits / 64 + 1);

    std::mutex publishMutex;
    Snapshot published;
    std::mutex swapMutex;
    Snapshot swapSlot;
    std::atomic<bool> writerDone{false};
    std::atomic<std::uint64_t> iterations{0};
    std::atomic<std::uint64_t> deepChecks{0};
    std::mutex failureMutex;
    std::string failure;
    const auto reportFailure = [&](std::string message) {
        const std::lock_guard lock(failureMutex);
        if (failure.empty()) {
            failure = std::move(message);
        }
    };

    TextBuffer buffer;
    std::string mirror;
    sizes[0] = 0;
    summaries[0] = TextSummary{};
    hashes[0] = kFnvOffset;
    published = buffer.snapshot();

    const auto reader = [&](unsigned index) {
        sched::detail::SplitMix64 rng(run.seed * 977 + index);
        std::array<Snapshot, 8> ring;
        std::size_t ringNext = 0;
        bool last = false;
        while (!last) {
            last = writerDone.load(std::memory_order_acquire); // One more pass after the writer ends.
            Snapshot s;
            {
                const std::lock_guard lock(publishMutex);
                s = published;
            }
            // Sometimes take the swap slot's snapshot instead, leaving ours there. A default snapshot
            // (the slot's initial value) passes every check as version 0.
            if (rng.below(4) == 0) {
                const std::lock_guard lock(swapMutex);
                std::swap(s, swapSlot);
            }
            iterations.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t v = s.version();
            const std::uint64_t n = s.size().value();
            if (v >= sizes.size() || n != sizes[v]) {
                reportFailure(std::format("reader {}: version {} has size {}, expected {}", index, v, n,
                                          v < sizes.size() ? sizes[v] : 0));
                return;
            }
            if (v % 64 == 0) {
                const std::string text = s.text();
                std::uint64_t hash = kFnvOffset;
                for (const std::string_view chunk : s.chunks(ByteRange{ByteOffset{0}, s.size()})) {
                    hash = fnv1a(hash, chunk);
                }
                const bool invariants = s.checkInvariants().has_value();
                if (!(summarize(text) == summaries[v / 64]) || !(s.summary() == summaries[v / 64]) ||
                    hash != hashes[v / 64] || fnv1a(kFnvOffset, text) != hash || !invariants) {
                    reportFailure(
                        std::format("reader {}: version {} differs from the writer's summary or hash", index, v));
                    return;
                }
                deepChecks.fetch_add(1, std::memory_order_relaxed);
            }
            // A window of up to 32 lines.
            const auto first = static_cast<std::uint32_t>(rng.below(s.lineCount()));
            Result<LineCursor> cursor = s.linesFrom(LineIndex{first});
            if (!cursor) {
                reportFailure(std::format("reader {}: linesFrom({}) failed", index, first));
                return;
            }
            LineInfo line;
            for (std::uint32_t i = first; i < std::min(first + 32, s.lineCount()); ++i) {
                const Result<LineInfo> expected = s.lineInfo(LineIndex{i});
                if (!cursor->next(line) || !expected || !(line == *expected)) {
                    reportFailure(std::format("reader {}: line {} of version {} differs", index, i, v));
                    return;
                }
            }
            // Round-trip rules at 16 offsets.
            for (int q = 0; q < 16; ++q) {
                const ByteOffset x{rng.below(n + 1)};
                const std::uint64_t clipped = s.clip(x).value();
                const bool interior = clipped > 0 && clipped < n && s.byteAt(ByteOffset{clipped - 1}) == '\r' &&
                                      s.byteAt(ByteOffset{clipped}) == '\n';
                const std::uint64_t floored = interior ? clipped - 1 : clipped;
                const Result<ByteOffset> back1 = s.offsetAt(s.pointAt(x));
                const Result<ByteOffset> back2 = s.offsetAt(s.utf16PointAt(x));
                const Result<ByteOffset> back3 = s.offsetAtUtf16(s.utf16At(x));
                if (!back1 || back1->value() != floored || !back2 || back2->value() != floored || !back3 ||
                    back3->value() != clipped) {
                    reportFailure(
                        std::format("reader {}: a round-trip rule fails at {} in version {}", index, x.value(), v));
                    return;
                }
            }
            // Keep the copy for a while, then swap one ring entry across threads.
            ring[ringNext] = std::move(s);
            ringNext = (ringNext + 1) % ring.size();
            if (rng.below(8) == 0) {
                const std::lock_guard lock(swapMutex);
                std::swap(ring[rng.below(ring.size())], swapSlot);
            }
        }
    };

    std::vector<std::thread> readers;
    readers.reserve(result.readers);
    for (unsigned i = 0; i < result.readers; ++i) {
        readers.emplace_back(reader, i);
    }

    // The writer.
    SplitMixSource source(run.seed);
    sched::detail::SplitMix64 rng(run.seed ^ 0xD1CE);
    const auto floorChar = [&](std::uint64_t x) {
        while (x > 0 && x < mirror.size() && (static_cast<unsigned char>(mirror[x]) & 0xC0u) == 0x80u) {
            --x;
        }
        return x;
    };
    for (std::uint32_t edit = 1; edit <= run.edits; ++edit) {
        {
            const std::lock_guard lock(failureMutex);
            if (!failure.empty()) {
                break;
            }
        }
        const std::uint64_t n = mirror.size();
        std::uint64_t a = floorChar(rng.below(n + 1));
        std::uint64_t b = floorChar(std::min(n, a + rng.below(16)));
        std::string text;
        if (n >= run.maxDocBytes / 2 && rng.below(2) == 0) {
            b = floorChar(std::min(n, a + 1 + rng.below(64))); // Past half the cap, erase more.
        } else {
            const std::uint64_t kept = n - (b - a);
            const std::uint64_t room = run.maxDocBytes > kept ? run.maxDocBytes - kept : 0;
            text = tokenText(source, std::min<std::uint64_t>(room, 1 + rng.below(rng.below(16) == 0 ? 512 : 12)));
        }
        // Every edit changes the text, so the version equals the edit count.
        if (a == b && text.empty()) {
            if (n < run.maxDocBytes) {
                text = "a";
            } else {
                a = 0;
                b = floorChar(std::min<std::uint64_t>(n, 4)); // At least 1: the first char is at most 4 bytes.
            }
        }
        const Result<TextChange> change = buffer.replace(ByteRange{ByteOffset{a}, ByteOffset{b}}, text);
        if (!change) {
            reportFailure("writer: edit failed: " + change.error().describe());
            break;
        }
        mirror.replace(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a), text);
        const std::uint64_t v = buffer.version();
        if (v != edit) {
            reportFailure(std::format("writer: version {} after edit {}", v, edit));
            break;
        }
        sizes[v] = mirror.size();
        if (v % 64 == 0) {
            summaries[v / 64] = oracleSummary(mirror);
            hashes[v / 64] = fnv1a(kFnvOffset, mirror);
        }
        const std::lock_guard lock(publishMutex);
        published = buffer.snapshot();
    }
    writerDone.store(true, std::memory_order_release);
    for (std::thread& t : readers) {
        t.join();
    }
    result.readerIterations = iterations.load();
    result.deepChecks = deepChecks.load();
    if (failure.empty() && buffer.current().text() != mirror) {
        failure = "writer: the final text differs from the mirror";
    }
    result.failure = std::move(failure);
    return result;
}

} // namespace frost::text::test
