// SPDX-License-Identifier: Apache-2.0
//
// OpDriver: drives random operations against a TextBuffer and the TextOracle and compares them
// (ADR 0012, section 9.1). The differential test, the stress tests and the libFuzzer target all
// run this one driver; only their ByteSource differs. No gtest dependency.
/*
 * Op mix: insert 40 %, erase 25 %, replace 15 %, applyEdits batch 5 %, restore one of the last 16
 * snapshots 3 %, reload via fromUtf8(oracle text) 1 %, invalid op 6 %, explicit full check 5 %.
 * Texts come from a token alphabet with line breaks, NUL and 1- to 4-byte characters at the code
 * point boundaries; lengths are mostly 1-8 bytes (97.8 %), sometimes up to 4 KiB (2 %) and rarely
 * up to 64 KiB (0.2 %), clamped so the document stays within maxDocBytes; past three quarters of
 * the cap half the inserts become erases.
 *
 * Checks. After every op (O(1) or O(B log n + window)): size and version; for a failed op the
 * error code of the shared check order and an unchanged version and summary; for an edit the
 * returned TextChange's byte fields against the oracle's D8 normalisation, its coverage bounds
 * (start >= begin - 1, oldEnd <= end + 1) and that no bound is CRLF-interior; the text of the
 * window [start - 8, newEnd + 8). Every checkEvery ops, on an explicit check op and at the end
 * (verifyFull): text equality; summary() against the oracle's independent summary and against
 * text::summarize (consistency); TextBuffer::checkInvariants, which covers the buffer and its
 * current snapshot (P1-P6, S1-S4); queriesPerCheck
 * random queries of every conversion and the round-trip rules; summaryOf, chunks and chunkAt on
 * random ranges; a LineCursor window (every line with sweepAllLines); one ring snapshot's text;
 * and the TextChange points with the LSP-replay property, for the last text-changing op before
 * the full check only (every step of it when it was a batch).
 *
 * Buffer limit: with a small options.buffer.maxBufferBytes (limitBuffer), generated edits and
 * loads cross the limit and must fail with OutOfRange (check 5 of TextBuffer's order); the
 * invalid op's near-limit branch inserts one byte more than the room left.
 *
 * A mismatch returns ErrorCode::Internal naming the driver's label (the seed), the op index, what
 * differed and the last 32 ops. Deterministic for a given ByteSource; no time or thread input.
 * Not thread-safe.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"

#include "text/support/text_oracle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text::test {

// Random choices for the driver.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    // A value in [0, bound); 0 when bound <= 1.
    virtual std::uint64_t next(std::uint64_t bound) = 0;
    // True once the source has run out (fuzz inputs); it then returns 0.
    [[nodiscard]] virtual bool exhausted() const = 0;
};

// SplitMix64-driven choices (differential and stress tests). Never runs out.
class SplitMixSource final : public ByteSource {
public:
    explicit SplitMixSource(std::uint64_t seed) noexcept : m_rng(seed) {}
    std::uint64_t next(std::uint64_t bound) override { return bound <= 1 ? 0 : m_rng.below(bound); }
    [[nodiscard]] bool exhausted() const override { return false; }

private:
    sched::detail::SplitMix64 m_rng;
};

// Choices read from a fuzz input: each takes the fewest whole bytes that cover the bound (little
// endian, reduced modulo the bound). At the end of the input it sets exhausted() and returns 0.
class FuzzSource final : public ByteSource {
public:
    explicit FuzzSource(std::span<const std::uint8_t> data) noexcept : m_data(data) {}
    std::uint64_t next(std::uint64_t bound) override;
    [[nodiscard]] bool exhausted() const override { return m_exhausted; }

private:
    std::span<const std::uint8_t> m_data;
    std::size_t m_position = 0;
    bool m_exhausted = false;
};

struct DriverOptions {
    TextBufferOptions buffer;
    std::uint32_t checkEvery = 64;       // Full check every this many ops (0: only explicit ones).
    std::uint32_t queriesPerCheck = 16;  // Random queries of every conversion per full check.
    std::uint32_t maxOps = 0xFFFF'FFFFu; // done() after this many ops.
    // Generated texts keep documents within this size; an accepted raw-byte insert (an invalid op
    // whose random bytes happen to be well formed) may add up to 8 bytes more.
    std::uint64_t maxDocBytes = 64 * 1024;
};

// Per-kind op counts, for test reports.
struct DriverStats {
    std::uint64_t ops = 0;
    std::uint64_t inserts = 0;
    std::uint64_t erases = 0;
    std::uint64_t replaces = 0;
    std::uint64_t batches = 0;
    std::uint64_t restores = 0;
    std::uint64_t reloads = 0;
    std::uint64_t rejected = 0;  // Ops that returned an error, as the oracle expected.
    std::uint64_t overLimit = 0; // Of those, edits, batches and loads rejected by the buffer limit.
    std::uint64_t noOps = 0;
    std::uint64_t fullChecks = 0;
    std::uint64_t checkedBytes = 0; // Document bytes over all full checks (mean = / fullChecks).
    std::uint64_t maxBytes = 0;     // The largest document seen.
};

class OpDriver {
public:
    // `label` names the run in mismatch reports (for example "seed 17").
    OpDriver(DriverOptions options, std::string label);

    // Loads `initial` through TextBuffer::fromUtf8 and checks the result against the oracle: an
    // ill-formed text must fail exactly when the oracle's validator rejects it (InvalidArgument
    // naming the oracle's offset), and the run then starts empty.
    [[nodiscard]] Result<void> load(std::string initial);

    // Performs one op chosen by `source` and runs the per-op checks (and the periodic full check).
    [[nodiscard]] Result<void> step(ByteSource& source);

    // The full check described above. `sweepAllLines` iterates every line with a LineCursor.
    [[nodiscard]] Result<void> verifyFull(bool sweepAllLines);

    [[nodiscard]] bool done() const noexcept { return m_stats.ops >= m_options.maxOps; }
    [[nodiscard]] const DriverStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] const TextBuffer& buffer() const noexcept { return m_buffer; }
    [[nodiscard]] const TextOracle& oracle() const noexcept { return m_oracle; }

private:
    // One applied step of the last text-changing op: its change and the old bytes it replaced.
    struct Step {
        TextChange change;
        std::string removed;
    };
    // A compact record of an op for mismatch reports.
    struct OpRecord {
        std::uint64_t index = 0;
        const char* kind = "";
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        std::string text; // At most the first 24 bytes.
        std::uint64_t textSize = 0;
        std::string outcome;
    };
    struct RingEntry {
        Snapshot snapshot;
        std::string text;
    };

    [[nodiscard]] Error mismatch(const std::string& what) const;
    void record(const char* kind, std::uint64_t a, std::uint64_t b, std::string_view text);
    void note(std::string outcome);

    [[nodiscard]] std::uint64_t randomBoundary(ByteSource& source) const;
    [[nodiscard]] std::uint64_t randomLength(ByteSource& source) const;
    [[nodiscard]] std::string randomText(ByteSource& source, std::uint64_t maxBytes) const;

    [[nodiscard]] Result<void> edit(ByteSource& source, int api, std::uint64_t a, std::uint64_t b, std::string text);
    [[nodiscard]] Result<void> batch(ByteSource& source);
    [[nodiscard]] Result<void> restore(ByteSource& source);
    [[nodiscard]] Result<void> reload();
    [[nodiscard]] Result<void> invalid(ByteSource& source);
    [[nodiscard]] Result<void> checkRejected(const Result<TextChange>& result, ErrorCode expected,
                                             std::uint64_t versionBefore, const TextSummary& summaryBefore);
    [[nodiscard]] Result<void> checkChange(const TextChange& change, const ByteChange& expected, std::uint64_t a,
                                           std::uint64_t b, std::string_view before);
    [[nodiscard]] Result<void> checkWindow(const TextChange& change);
    [[nodiscard]] Result<void> checkSteps();
    [[nodiscard]] Result<void> checkQueries(sched::detail::SplitMix64& rng);
    [[nodiscard]] Result<void> checkLines(sched::detail::SplitMix64& rng, bool sweepAllLines);
    void pushSnapshot(ByteSource& source);

    DriverOptions m_options;
    std::string m_label;
    TextBuffer m_buffer;
    TextOracle m_oracle;
    std::uint64_t m_expectedVersion = 0;
    DriverStats m_stats;
    std::vector<Step> m_steps; // Of the last text-changing op, in application order.
    std::vector<RingEntry> m_ring;
    std::size_t m_ringNext = 0;
    std::array<OpRecord, 32> m_recent{};
    bool m_foreignChecked = false;
};

// `bytes` bytes (exactly) of text from the driver's token alphabet: well-formed UTF-8 with every
// line break kind, NUL and characters at the 1- to 4-byte boundaries. O(bytes).
[[nodiscard]] std::string tokenText(ByteSource& source, std::uint64_t bytes);

// Gives `options` a small buffer limit: maxBufferBytes = limit and maxDocBytes = limit + 32, so
// generated inserts, replaces and batches sometimes overshoot the limit and longer initial texts
// fail to load (both OutOfRange). Precondition: limit <= 0xFFFF'FFFE (TextBufferOptions).
void limitBuffer(DriverOptions& options, std::uint64_t limit) noexcept;

/*
 * One seeded differential run (TextDifferential, TextStress): the seed picks maxPieceBytes from
 * {4, 5, 7, 16, 64, 4096}[seed % 6] and an initial text of 0-8 KiB, empty when (seed / 6) % 4 == 0,
 * so every cap also starts empty; one seed in seven runs with a small buffer limit
 * (seedBufferLimit). Then `ops` driver steps from SplitMixSource(seed) and a final verifyFull(true).
 * Every field changes the run's choices or checks, so reproduceCommand names each of them.
 */
struct SeedRun {
    std::uint64_t seed = 0;
    std::uint32_t ops = 1000;
    std::uint32_t checkEvery = 64;
    std::uint32_t queriesPerCheck = 16;

    friend constexpr bool operator==(const SeedRun&, const SeedRun&) noexcept = default;
};
[[nodiscard]] std::uint32_t seedPieceCap(std::uint64_t seed) noexcept;
[[nodiscard]] std::string seedInitialText(std::uint64_t seed);
// The small buffer limit of seed `seed` (passed to limitBuffer), or none (the default limit). One
// seed in seven, seed % 7 == 3: any 7 consecutive seeds include one, so every stress scale has
// some, and 7 is coprime to the 6 caps, so every cap and initial-text choice occurs with a limit.
// The limit is 256 + 1024 * ((seed / 7) % 9) bytes, from below to above the largest initial text.
[[nodiscard]] std::optional<std::uint64_t> seedBufferLimit(std::uint64_t seed) noexcept;
// The driver's stats, or its first mismatch (ErrorCode::Internal, labelled "seed <s>").
[[nodiscard]] Result<DriverStats> runSeed(const SeedRun& run);

/*
 * TextDifferential.Quick's settings: the seeds 0..199, or only FROST_TEXT_SEED, each run as
 * SeedRun{seed, FROST_TEXT_OPS (default 1000), FROST_TEXT_CHECK_EVERY (64), FROST_TEXT_QUERIES
 * (16)} on FROST_TEXT_THREADS worker threads (default min(4, hardware threads / 2); 1 with
 * FROST_TEXT_SEED). `lookup(name)` returns a variable's value, empty when unset.
 * Errors: InvalidArgument naming the variable when a value is not a decimal number in range
 * (ops, checks and queries 32-bit, threads 1..1024).
 */
struct QuickSettings {
    std::optional<std::uint64_t> onlySeed;
    SeedRun run; // run.seed is *onlySeed when that is set, else 0 (unused).
    std::optional<unsigned> threads;
};
using EnvironmentLookup = std::function<std::string(std::string_view name)>;
[[nodiscard]] Result<QuickSettings> quickSettings(const EnvironmentLookup& lookup);

// The command that re-runs exactly `run` through TextDifferential.Quick: it sets every variable
// quickSettings reads for a SeedRun field, so quickSettings on its assignments yields `run` again
// (TextDifferential.ReproduceCommandRoundTrips). The differential and stress tests print it for
// their first failing seed.
[[nodiscard]] std::string reproduceCommand(const SeedRun& run);

} // namespace frost::text::test
