// SPDX-License-Identifier: Apache-2.0
//
// UndoProperty (ADR 0017): the identity properties of the undo tree and the LSP replay of history
// ChangeSets, on random histories from the differential's op mix (undo_ops.hpp).
/*
 * Each seed runs the UndoDriver (which checks every op against the UndoOracle) and stops at 16
 * checkpoints. At a checkpoint it settles open groups and compositions, records the state (text,
 * committed text, every anchor with its id and bias, stateId, current node) and then:
 *   UndoThenRedoIsIdentity: k undos (k random in [1, depth]) then k redos along redoChild return
 *     to exactly that state, and each redo returns the selections last recorded for its node;
 *   RedoThenUndoIsIdentity: from a state with a redo branch (made by 1..depth undos when the
 *     checkpoint has none), k redos (k in [1, redo chain]) then k undos do the same, and each undo
 *     returns the selections an earlier undo of the node returned;
 *   ChangesReplayAsLsp: every undo, redo and rollback ChangeSet of the run (random ops and the
 *     checkpoint chains) replays as LSP edits with the new bytes taken from the post-op text only.
 * Anchors are compared exactly: the anchorsBefore and anchorsAfter replacements make both round
 * trips the identity even for anchors created after the nodes were recorded. The generator is the
 * UndoDriver, which also checks each op against the UndoOracle; the identity checks themselves
 * compare only Document states.
 */

#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/document.hpp"

#include "text/support/text_ops.hpp"
#include "text/support/undo_ops.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace {

using frost::Result;
using frost::sched::detail::SplitMix64;
using frost::text::ResolvedAnchor;
using frost::text::SelectionSet;
using frost::text::test::SplitMixSource;
using frost::text::test::UndoDriver;
using frost::text::test::UndoDriverOptions;
using frost::text::test::undoSeedInitialText;
using frost::text::test::undoSeedOptions;

constexpr std::uint64_t kSeeds = 100;
constexpr std::uint32_t kOps = 400;
constexpr std::uint32_t kCheckpoints = 16;

struct State {
    std::string text;
    std::string committed;
    std::vector<ResolvedAnchor> anchors;
    std::uint64_t stateId = 0;
    std::uint64_t node = 0;
};

State capture(const UndoDriver& driver) {
    return State{driver.document().text().text(), driver.document().committedText().text(),
                 driver.document().anchors().all(), driver.document().undoState().stateId, driver.currentNodeId()};
}

// Empty when `got` equals `want`, else what differs.
std::string differences(const State& want, const State& got) {
    std::string out;
    if (got.text != want.text) {
        out += " text;";
    }
    if (got.committed != want.committed) {
        out += " committed text;";
    }
    if (got.anchors != want.anchors) {
        out += std::format(" anchors ({} vs {});", got.anchors.size(), want.anchors.size());
        for (std::size_t i = 0; i < std::min(got.anchors.size(), want.anchors.size()); ++i) {
            if (!(got.anchors[i] == want.anchors[i])) {
                out += std::format(" first at {}: id {} at {} vs id {} at {};", i, got.anchors[i].id.value,
                                   got.anchors[i].offset.value(), want.anchors[i].id.value,
                                   want.anchors[i].offset.value());
                break;
            }
        }
    }
    if (got.stateId != want.stateId) {
        out += std::format(" stateId {} vs {};", got.stateId, want.stateId);
    }
    if (got.node != want.node) {
        out += std::format(" node {} vs {};", got.node, want.node);
    }
    return out;
}

enum class Round { UndoThenRedo, RedoThenUndo };

// Runs `seed` with round trips of kind `round` at the checkpoints; returns the number performed.
Result<std::uint64_t> runSeed(std::uint64_t seed, Round round, bool replay, std::uint64_t* replayChecks) {
    UndoDriverOptions options = undoSeedOptions(seed);
    options.maxOps = kOps;
    options.replayHistoryOps = replay;
    UndoDriver driver(options, std::format("property seed {}", seed), undoSeedInitialText(seed, options));
    SplitMixSource source(seed ^ 0x5EED'0001u);
    SplitMix64 rng(seed);
    std::uint64_t trips = 0;
    const auto fail = [&](const std::string& what) {
        return frost::fail(frost::ErrorCode::Internal, std::format("seed {}: {}", seed, what));
    };
    for (std::uint32_t checkpoint = 0; checkpoint < kCheckpoints; ++checkpoint) {
        for (std::uint32_t i = 0; i < kOps / kCheckpoints; ++i) {
            if (Result<void> stepped = driver.step(source); !stepped) {
                return std::unexpected(stepped.error());
            }
        }
        if (Result<void> settled = driver.settle(source); !settled) {
            return std::unexpected(settled.error());
        }
        const auto undoChecked = [&]() -> Result<void> {
            const std::uint64_t node = driver.currentNodeId();
            const std::optional<SelectionSet> earlier = driver.undoSelectionsOf(node);
            if (Result<void> undone = driver.undoOnce(); !undone) {
                return undone;
            }
            if (!driver.lastStep().changeSet) {
                return fail(std::format("checkpoint {}: undo of node {} returned no ChangeSet", checkpoint, node));
            }
            if (earlier && !(driver.lastStep().changeSet->selections == *earlier)) {
                return fail(std::format("checkpoint {}: undo of node {} returned other selections than before",
                                        checkpoint, node));
            }
            return {};
        };
        const auto redoChecked = [&]() -> Result<void> {
            const std::uint64_t target = driver.redoChain().front();
            const std::optional<SelectionSet> recorded = driver.recordedSelectionsOf(target);
            if (Result<void> redone = driver.redoOnce(); !redone) {
                return redone;
            }
            if (driver.currentNodeId() != target) {
                return fail(std::format("checkpoint {}: redo went to {} instead of {}", checkpoint,
                                        driver.currentNodeId(), target));
            }
            if (recorded && !(driver.lastStep().changeSet->selections == *recorded)) {
                return fail(std::format("checkpoint {}: redo of node {} returned other selections than recorded",
                                        checkpoint, target));
            }
            return {};
        };
        // RedoThenUndo starts from a state with a redo branch: without one, undo 1..depth steps first.
        if (round == Round::RedoThenUndo && driver.redoChain().empty() && driver.currentDepth() > 0) {
            for (std::uint64_t j = 0, steps = 1 + rng.below(driver.currentDepth()); j < steps; ++j) {
                if (Result<void> r = undoChecked(); !r) {
                    return std::unexpected(r.error());
                }
            }
        }
        const State start = capture(driver);
        const std::uint64_t available =
            round == Round::UndoThenRedo ? driver.currentDepth() : driver.redoChain().size();
        if (available == 0) {
            continue;
        }
        const std::uint64_t k = 1 + rng.below(available); // In [1, depth] or [1, redo chain].
        ++trips;
        for (std::uint64_t i = 0; i < k; ++i) {
            if (Result<void> r = round == Round::UndoThenRedo ? undoChecked() : redoChecked(); !r) {
                return std::unexpected(r.error());
            }
        }
        for (std::uint64_t i = 0; i < k; ++i) {
            if (Result<void> r = round == Round::UndoThenRedo ? redoChecked() : undoChecked(); !r) {
                return std::unexpected(r.error());
            }
        }
        if (const std::string diff = differences(start, capture(driver)); !diff.empty()) {
            return fail(std::format("checkpoint {}: {} {} and back differs:{}", checkpoint, k,
                                    round == Round::UndoThenRedo ? "undos" : "redos", diff));
        }
        if (Result<void> checked = driver.verifyFull(); !checked) {
            return std::unexpected(checked.error());
        }
    }
    if (replayChecks != nullptr) {
        *replayChecks += driver.stats().replayChecks;
    }
    return trips;
}

TEST(UndoProperty, UndoThenRedoIsIdentity) {
    std::uint64_t trips = 0;
    for (std::uint64_t seed = 0; seed < kSeeds; ++seed) {
        const Result<std::uint64_t> result = runSeed(seed, Round::UndoThenRedo, false, nullptr);
        ASSERT_TRUE(result.has_value()) << result.error().message();
        trips += *result;
    }
    EXPECT_GT(trips, kSeeds * kCheckpoints / 4); // Most checkpoints have something to undo.
}

TEST(UndoProperty, RedoThenUndoIsIdentity) {
    std::uint64_t trips = 0;
    for (std::uint64_t seed = 0; seed < kSeeds; ++seed) {
        const Result<std::uint64_t> result = runSeed(seed, Round::RedoThenUndo, false, nullptr);
        ASSERT_TRUE(result.has_value()) << result.error().message();
        trips += *result;
    }
    EXPECT_GT(trips, kSeeds * kCheckpoints / 4); // Most checkpoints reach a redo branch.
}

TEST(UndoProperty, ChangesReplayAsLsp) {
    std::uint64_t replayed = 0;
    for (std::uint64_t seed = 0; seed < kSeeds / 2; ++seed) {
        for (const Round round : {Round::UndoThenRedo, Round::RedoThenUndo}) {
            const Result<std::uint64_t> result = runSeed(seed, round, true, &replayed);
            ASSERT_TRUE(result.has_value()) << result.error().message();
        }
    }
    EXPECT_GT(replayed, 1000u);
}

} // namespace
