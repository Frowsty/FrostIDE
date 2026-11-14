// SPDX-License-Identifier: Apache-2.0
//
// Regression tests for the Unit Tests results tree (RUN-10): a rerun that ends before it reports a
// result cannot leave the previous run's verdict visible.

#include "frost/ui/test_results_tree.hpp"

#include <gtest/gtest.h>

namespace frost::ui {
namespace {

TEST(TestResultsTree, AFailedOrStoppedRerunDoesNotRestoreThePreviousVerdict) {
    TestResultsTree tree;
    run::DiscoveredTest discovered;
    discovered.name = "Suite.Case";
    tree.setTests({discovered});

    run::TestResult previousCase;
    previousCase.name = "Suite.Case";
    previousCase.outcome = run::TestOutcome::Passed;
    previousCase.duration = std::chrono::milliseconds(3);
    run::TestResult previous;
    previous.name = "Suite.Case";
    previous.outcome = run::TestOutcome::Passed;
    previous.duration = std::chrono::milliseconds(3);
    previous.cases.push_back(previousCase);
    tree.applyResult(previous);
    ASSERT_EQ(tree.itemFor("Suite.Case")->text(1), QStringLiteral("Passed"));
    ASSERT_EQ(tree.itemFor("Suite.Case", "Suite.Case")->text(1), QStringLiteral("Passed"));

    tree.markPending({"Suite.Case"});
    ASSERT_EQ(tree.itemFor("Suite.Case")->text(1), QStringLiteral("Pending"));
    EXPECT_TRUE(tree.itemFor("Suite.Case", "Suite.Case")->text(1).isEmpty());
    EXPECT_EQ(tree.result("Suite.Case"), nullptr);

    run::TestRunReport interrupted;
    interrupted.stopped = true;
    tree.finishRun(interrupted);

    EXPECT_TRUE(tree.itemFor("Suite.Case")->text(1).isEmpty());
    EXPECT_TRUE(tree.itemFor("Suite.Case", "Suite.Case")->text(1).isEmpty());
    EXPECT_EQ(tree.result("Suite.Case"), nullptr);
    EXPECT_TRUE(tree.failedTestNames().empty());
    EXPECT_EQ(tree.counts().withoutResult, 1U);
}

} // namespace
TEST(TestResultsTree, ExplicitFrameworkListingsHaveNoInventedVerdictsAndRemainFilterable) {
    TestResultsTree tree;
    run::DiscoveredTest test;
    test.name = "actual-binary";
    tree.setTests({test});
    tree.setFrameworkCases(test.name,
                           {{"suite/leaf", run::TestLocation{"actual.cpp", 17}, false}, {"disabled", {}, true}});
    auto* item = tree.itemFor(test.name, "suite/leaf");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->text(1).isEmpty());
    EXPECT_TRUE(item->text(2).isEmpty());
    EXPECT_TRUE(item->toolTip(0).contains(QStringLiteral("actual.cpp:17")));
    EXPECT_TRUE(tree.itemFor(test.name, "disabled")->text(1).isEmpty());
    tree.setFilter(QStringLiteral("suite/leaf"));
    EXPECT_NE(tree.itemFor(test.name, "suite/leaf"), nullptr);
    tree.setTests({test});
    EXPECT_EQ(tree.frameworkCases(test.name), nullptr);
}

} // namespace frost::ui
