// SPDX-License-Identifier: Apache-2.0
//
// The concurrent-reader scenario for anchors shared by AnchorConcurrency.ReadersResolveDuringEdits
// (unit scale) and AnchorStress.ConcurrentReaders (stress scale): an UndoDriver edits a Document
// while readers resolve anchors in published DocumentSnapshots on their own threads (ADR 0017,
// decision 13). TSan in the WSL -Full tree checks the reference counting and publication.
/*
 * The writer is an UndoDriver with its oracle comparison on and 32 tracked anchors (created after
 * load, never released) besides its churn anchors; its op mix covers edits, undo and redo, groups,
 * composition and anchor creation and release. After every op that changed the text or the
 * anchors it stores, in preallocated arrays indexed by publication number, the tracked anchors'
 * offsets from the UndoOracle's anchor model (independent of AnchorSet), the text size and, for
 * every 64th publication, an FNV-1a hash of the oracle's text; then it publishes
 * {Document::snapshot(), number} into a mutex-guarded slot. The mutex orders the array writes
 * before any reader's copy of that publication, also when it reaches a reader through the swap
 * slot.
 *
 * Each reader iteration copies the publication under the mutex and then, outside it: checks the
 * size and that the anchor set's version and size match the text's; resolves every tracked anchor
 * against the arrays and checks each offset is within the text and a char boundary; for every 64th
 * publication runs both checkInvariants(), the hash over chunks() and a resolveRange over a random
 * range against resolve(); keeps the copy in a ring of 8 so releases run on reader threads; and
 * swaps one ring entry with the second slot. Loops are bounded by the writer; there are no sleeps.
 */
#pragma once

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/document.hpp"

#include "text/concurrent_readers.hpp"
#include "text/support/text_ops.hpp"
#include "text/support/undo_ops.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace frost::text::test {

struct DocumentConcurrentRun {
    std::uint32_t ops = 5'000;
    std::uint64_t seed = 1;
};

struct DocumentConcurrentResult {
    unsigned readers = 0;
    std::uint64_t publications = 0;
    std::uint64_t readerIterations = 0;
    std::uint64_t deepChecks = 0; // Publications checked against the stored hash and invariants.
    std::string failure;          // Empty on success.
};

inline DocumentConcurrentResult runDocumentConcurrentReaders(const DocumentConcurrentRun& run) {
    constexpr std::uint32_t kTracked = 32;
    const unsigned hardware = std::thread::hardware_concurrency();
    DocumentConcurrentResult result;
    result.readers = std::max(1u, std::min(4u, hardware > 1 ? hardware - 1 : 1u));

    UndoDriverOptions options = undoSeedOptions(run.seed);
    options.trackedAnchors = kTracked;
    options.maxOps = run.ops;
    UndoDriver driver(options, std::format("concurrent seed {}", run.seed), undoSeedInitialText(run.seed, options));
    SplitMixSource source(run.seed);

    // Arrays indexed by publication number, written before the publication.
    const std::size_t capacity = std::size_t{run.ops} + 1;
    std::vector<std::uint32_t> offsets(capacity * kTracked);
    std::vector<std::uint64_t> sizes(capacity);
    std::vector<std::uint64_t> hashes(capacity / 64 + 1);
    const std::span<const std::uint64_t> tracked = driver.trackedAnchors();
    if (tracked.size() != kTracked) {
        result.failure = std::format("the driver created {} tracked anchors, not {}", tracked.size(), kTracked);
        return result;
    }
    const auto store = [&](std::uint64_t number) {
        const std::string& text = driver.oracle().text();
        sizes[number] = text.size();
        for (std::uint32_t i = 0; i < kTracked; ++i) {
            offsets[number * kTracked + i] = static_cast<std::uint32_t>(driver.oracle().anchor(tracked[i])->offset);
        }
        if (number % 64 == 0) {
            hashes[number / 64] = fnv1a(kFnvOffset, text);
        }
    };

    struct Publication {
        DocumentSnapshot snapshot;
        std::uint64_t number = 0;
    };
    std::mutex publishMutex;
    Publication published;
    std::mutex swapMutex;
    Publication swapSlot;
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

    std::uint64_t publications = 0;
    store(0);
    published = Publication{driver.document().snapshot(), 0};
    swapSlot = published;
    // The rings start full of publication 0, so a swap never hands out a default (empty) value.
    const Publication initial = published;

    const auto reader = [&](unsigned index) {
        sched::detail::SplitMix64 rng(run.seed * 991 + index);
        std::array<Publication, 8> ring;
        ring.fill(initial);
        std::size_t ringNext = 0;
        bool last = false;
        while (!last) {
            last = writerDone.load(std::memory_order_acquire); // One more pass after the writer ends.
            Publication p;
            {
                const std::lock_guard lock(publishMutex);
                p = published;
            }
            if (rng.below(4) == 0) {
                const std::lock_guard lock(swapMutex);
                std::swap(p, swapSlot);
            }
            iterations.fetch_add(1, std::memory_order_relaxed);
            const Snapshot& text = p.snapshot.text;
            const AnchorSet& anchors = p.snapshot.anchors;
            const std::uint64_t n = text.size().value();
            if (p.number >= capacity || n != sizes[p.number] || anchors.textSize() != text.size() ||
                anchors.version() != text.version()) {
                reportFailure(std::format("reader {}: publication {} has size {} (anchors {} at version {}, text "
                                          "version {}), expected {}",
                                          index, p.number, n, anchors.textSize().value(), anchors.version(),
                                          text.version(), p.number < capacity ? sizes[p.number] : 0));
                return;
            }
            for (std::uint32_t i = 0; i < kTracked; ++i) {
                const Result<ByteOffset> at = anchors.resolve(AnchorId{tracked[i]});
                const std::uint32_t expected = offsets[p.number * kTracked + i];
                if (!at || at->value() != expected || at->value() > n || !text.isCharBoundary(*at)) {
                    reportFailure(std::format("reader {}: anchor {} in publication {} resolves to {}, expected {}",
                                              index, tracked[i], p.number,
                                              at ? std::to_string(at->value()) : at.error().describe(), expected));
                    return;
                }
            }
            if (p.number % 64 == 0) {
                std::uint64_t hash = kFnvOffset;
                for (const std::string_view chunk : text.chunks(ByteRange{ByteOffset{0}, text.size()})) {
                    hash = fnv1a(hash, chunk);
                }
                const std::uint64_t a = rng.below(n + 1);
                const std::uint64_t b = a + rng.below(n - a + 1);
                bool rangeOk = true;
                for (const ResolvedAnchor& r : anchors.resolveRange(ByteRange{ByteOffset{a}, ByteOffset{b}})) {
                    const Result<ByteOffset> again = anchors.resolve(r.id);
                    rangeOk = rangeOk && again && *again == r.offset && r.offset.value() >= a && r.offset.value() <= b;
                }
                if (hash != hashes[p.number / 64] || !text.checkInvariants().has_value() ||
                    !anchors.checkInvariants().has_value() || !rangeOk) {
                    reportFailure(std::format("reader {}: publication {} fails the deep check", index, p.number));
                    return;
                }
                deepChecks.fetch_add(1, std::memory_order_relaxed);
            }
            ring[ringNext] = std::move(p);
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
    while (!driver.done()) {
        {
            const std::lock_guard lock(failureMutex);
            if (!failure.empty()) {
                break;
            }
        }
        if (Result<void> stepped = driver.step(source); !stepped) {
            reportFailure(stepped.error().message());
            break;
        }
        if (driver.lastStep().changed) {
            ++publications;
            store(publications);
            Publication next{driver.document().snapshot(), publications};
            const std::lock_guard lock(publishMutex);
            published = std::move(next);
        }
    }
    if (Result<void> checked = driver.verifyFull(); !checked) {
        reportFailure(checked.error().message());
    }
    writerDone.store(true, std::memory_order_release);
    for (std::thread& r : readers) {
        r.join();
    }
    result.publications = publications;
    result.readerIterations = iterations.load();
    result.deepChecks = deepChecks.load();
    result.failure = std::move(failure);
    return result;
}

} // namespace frost::text::test
