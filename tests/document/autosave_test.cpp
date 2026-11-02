// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for frost::document's AutosaveController in virtual time (ADR 0028): Save All once
// the user has been idle for the idle delay (once per idle period), on frame deactivation, the
// hot-exit journal cadence, and the policies switching each of them off.

#include "frost/document/autosave.hpp"

#include "document_fixture.hpp"
#include <gtest/gtest.h>

#include <chrono>

namespace frost::document::test {
namespace {

using namespace std::chrono_literals;
using AutosaveTest = DocumentFixture;

TEST_F(AutosaveTest, SavesOnceAfterTheIdleDelay) {
    policies.autosave.idleDelay = 15s;
    writeExternally("a.txt", "a\n");
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    AutosaveController autosave(manager(), clock);

    edit(*document, 0, 1, "A");
    autosave.noteActivity();
    ASSERT_TRUE(autosave.nextDeadline().has_value());
    EXPECT_EQ(*autosave.nextDeadline(), clock.steadyNow() + AutosaveController::kJournalDelay);

    clock.advance(14s);
    EXPECT_FALSE(autosave.poll().autosave.has_value());
    EXPECT_EQ(read("a.txt"), "a\n");
    EXPECT_TRUE(document->dirty());

    clock.advance(1s);
    const AutosaveController::PollResult due = autosave.poll();
    ASSERT_TRUE(due.autosave.has_value());
    ASSERT_EQ(due.autosave->saved.size(), 1U);
    EXPECT_EQ(read("a.txt"), "A\n");
    EXPECT_FALSE(document->dirty());

    // Once per idle period: new edits without new activity wait for the next one.
    edit(*document, 0, 1, "B");
    clock.advance(1min);
    EXPECT_FALSE(autosave.poll().autosave.has_value());
    EXPECT_FALSE(autosave.nextDeadline().has_value());
    autosave.noteActivity();
    clock.advance(15s);
    ASSERT_TRUE(autosave.poll().autosave.has_value());
    EXPECT_EQ(read("a.txt"), "B\n");
}

TEST_F(AutosaveTest, ActivityPostponesTheIdleSave) {
    policies.autosave.idleDelay = 10s;
    writeExternally("a.txt", "a\n");
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    AutosaveController autosave(manager(), clock);
    for (int i = 0; i < 5; ++i) {
        edit(*document, 0, 1, i % 2 == 0 ? "X" : "Y");
        autosave.noteActivity();
        clock.advance(9s);
        EXPECT_FALSE(autosave.poll().autosave.has_value());
    }
    EXPECT_EQ(read("a.txt"), "a\n");
    clock.advance(1s);
    EXPECT_TRUE(autosave.poll().autosave.has_value());
    EXPECT_EQ(read("a.txt"), "X\n");
}

TEST_F(AutosaveTest, FrameDeactivationAndPolicies) {
    writeExternally("a.txt", "a\n");
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    AutosaveController autosave(manager(), clock);
    edit(*document, 0, 1, "A");
    const auto saved = autosave.frameDeactivated();
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->saved.size(), 1U);
    EXPECT_EQ(read("a.txt"), "A\n");

    // Both switched off (save.autosave.* false): nothing is saved, whatever the time.
    SavePolicies off = manager().policies();
    off.autosave.onFrameDeactivation = false;
    off.autosave.onIdle = false;
    off.hotExit = false;
    manager().setPolicies(off);
    edit(*document, 0, 1, "B");
    autosave.noteActivity();
    EXPECT_FALSE(autosave.frameDeactivated().has_value());
    EXPECT_FALSE(autosave.nextDeadline().has_value());
    clock.advance(1h);
    const AutosaveController::PollResult nothing = autosave.poll();
    EXPECT_FALSE(nothing.autosave.has_value());
    EXPECT_FALSE(nothing.journal.has_value());
    EXPECT_EQ(read("a.txt"), "A\n");
}

TEST_F(AutosaveTest, JournalRunsShortlyAfterTheLastInput) {
    writeExternally("a.txt", "a\n");
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    AutosaveController autosave(manager(), clock);
    edit(*document, 0, 1, "A");
    autosave.noteActivity();
    clock.advance(500ms);
    EXPECT_FALSE(autosave.poll().journal.has_value());
    clock.advance(500ms);
    const AutosaveController::PollResult due = autosave.poll();
    ASSERT_TRUE(due.journal.has_value()); // Without a journal service the report is empty.
    EXPECT_EQ(due.journal->written, 0U);
    EXPECT_FALSE(due.autosave.has_value());
    EXPECT_EQ(autosave.nextDeadline(), std::optional(clock.steadyNow() - 1s + policies.autosave.idleDelay));
}

} // namespace
} // namespace frost::document::test
