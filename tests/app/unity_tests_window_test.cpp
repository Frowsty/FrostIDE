// SPDX-License-Identifier: Apache-2.0
#include "../../src/app/src/unity_tests_window.hpp"
#include <QPushButton>
#include <QTableWidget>
#include <QWidget>
#include <gtest/gtest.h>
namespace frost::app {
namespace {
lang::Json state(std::string job, std::string phase = "discovered") {
    return lang::Json::object()
        .set("version", 1)
        .set("available", true)
        .set("ok", true)
        .set("owner", std::string(32, 'a'))
        .set("job", job)
        .set("catalog", job)
        .set("mode", "EditMode")
        .set("state", phase)
        .set("message", "Actual fixture adapter state")
        .set("execution", "");
}
lang::Json row() {
    return lang::Json::object()
        .set("id", "Tests.dll/Fixture/Actual")
        .set("name", "Actual")
        .set("fullName", "Fixture.Actual")
        .set("outcome", "Discovered")
        .set("message", "")
        .set("stack", "")
        .set("duration", 0.0)
        .set("truncated", false);
}
lang::Json page(std::string job) {
    return state(job)
        .set("snapshot", std::string(32, 'b'))
        .set("total", 1)
        .set("offset", 0)
        .set("next", 1)
        .set("cases", lang::Json::array().push(row()));
}
} // namespace
TEST(UnityTestsModel, RejectsChangedContinuationDuplicateIdentityAndInventedOutcome) {
    auto response = page(std::string(32, 'c'));
    ASSERT_TRUE(parseUnityTestPage(response, {}, 0));
    EXPECT_FALSE(parseUnityTestPage(response, std::string(32, 'd'), 0));
    EXPECT_FALSE(parseUnityTestPage(response, {}, 1));
    response.set("total", 2).set("next", 2).set("cases", lang::Json::array().push(row()).push(row()));
    EXPECT_FALSE(parseUnityTestPage(response, {}, 0));
    response = page(std::string(32, 'c'));
    auto bad = row();
    bad.set("outcome", "AssumedPass");
    response.set("cases", lang::Json::array().push(bad));
    EXPECT_FALSE(parseUnityTestPage(response, {}, 0));
}
TEST(UnityTestsWindow, MissingTrustedSelectedEditorNeverRequestsAProcessOrInventsTests) {
    int requests = 0;
    UnityTestsWindowContext context;
    context.selected = []() { return std::optional<UnityEditorIdentity>{}; };
    context.request = [&](const auto&, const auto&, auto) { ++requests; };
    std::unique_ptr<QWidget> widget(createUnityTestsWidget(nullptr, std::move(context)));
    auto* discover = widget->findChild<QPushButton*>("unityTestsDiscover");
    ASSERT_NE(discover, nullptr);
    discover->click();
    EXPECT_EQ(requests, 0);
    EXPECT_EQ(widget->findChild<QTableWidget*>()->rowCount(), 0);
}
TEST(UnityTestsWindow, ActualCatalogIdsDriveSelectionAndLateDomainRepliesCannotPublish) {
    auto epoch = std::make_shared<std::atomic_uint64_t>(0);
    UnityEditorIdentity identity;
    identity.root = "/fixture";
    identity.instance = std::string(32, 'd');
    identity.session = std::string(32, 'e');
    identity.generation = 1;
    identity.epoch = epoch;
    identity.revision = 0;
    identity.tests = true;
    struct Request {
        lang::Json payload;
        std::function<void(Result<lang::Json>)> done;
    };
    std::vector<Request> requests;
    UnityTestsWindowContext context;
    context.selected = [&]() { return std::optional<UnityEditorIdentity>{identity}; };
    context.request = [&](const auto&, const auto& payload, auto done) {
        requests.push_back({payload, std::move(done)});
    };
    std::unique_ptr<QWidget> widget(createUnityTestsWidget(nullptr, std::move(context)));
    widget->findChild<QPushButton*>("unityTestsDiscover")->click();
    ASSERT_EQ(requests.size(), 1u);
    const std::string job(requests[0].payload.stringAt("job"));
    auto discovered = std::move(requests[0].done);
    discovered(state(job));
    ASSERT_EQ(requests.size(), 2u);
    auto snapshot = std::move(requests[1].done);
    snapshot(page(job));
    auto* table = widget->findChild<QTableWidget*>();
    ASSERT_EQ(table->rowCount(), 1);
    table->selectRow(0);
    widget->findChild<QPushButton*>("unityTestsRunSelected")->click();
    ASSERT_EQ(requests.size(), 3u);
    const auto* ids = requests[2].payload.find("ids");
    ASSERT_NE(ids, nullptr);
    ASSERT_EQ(ids->size(), 1u);
    EXPECT_EQ(*ids->items().front().asString(), "Tests.dll/Fixture/Actual");
    epoch->store(1);
    identity.revision = 1;
    identity.generation = 2;
    identity.session = std::string(32, 'f');
    auto late = std::move(requests[2].done);
    late(state(std::string(requests[2].payload.stringAt("job")), "finished"));
    EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("Discovered"));
    EXPECT_EQ(requests.size(), 3u);
}
TEST(UnityTestsWindow, FirstSnapshotMustPreserveAcknowledgedExecutionBeforePublishing) {
    UnityEditorIdentity identity;
    identity.root = "/fixture";
    identity.instance = std::string(32, 'd');
    identity.session = std::string(32, 'e');
    identity.generation = 1;
    identity.epoch = std::make_shared<std::atomic_uint64_t>(0);
    identity.tests = true;
    struct Request {
        lang::Json payload;
        std::function<void(Result<lang::Json>)> done;
    };
    std::vector<Request> requests;
    UnityTestsWindowContext context;
    context.selected = [&]() { return std::optional<UnityEditorIdentity>{identity}; };
    context.request = [&](const auto&, const auto& payload, auto done) {
        requests.push_back({payload, std::move(done)});
    };
    std::unique_ptr<QWidget> widget(createUnityTestsWidget(nullptr, std::move(context)));
    widget->findChild<QPushButton*>("unityTestsDiscover")->click();
    ASSERT_EQ(requests.size(), 1u);
    const std::string discovery(requests[0].payload.stringAt("job"));
    auto deliver = [&](size_t index, lang::Json response) {
        auto done = std::move(requests.at(index).done);
        done(std::move(response));
    };
    deliver(0, state(discovery));
    ASSERT_EQ(requests.size(), 2u);
    deliver(1, page(discovery));
    auto* table = widget->findChild<QTableWidget*>();
    ASSERT_EQ(table->rowCount(), 1);
    table->selectRow(0);
    widget->findChild<QPushButton*>("unityTestsRunSelected")->click();
    ASSERT_EQ(requests.size(), 3u);
    const std::string job(requests[2].payload.stringAt("job"));
    const std::string executionA = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    const std::string executionB = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
    deliver(2, state(job, "running").set("catalog", discovery).set("execution", executionA));
    ASSERT_EQ(requests.size(), 4u);
    auto failed = row();
    failed.set("outcome", "Failed");
    deliver(
        3,
        page(job).set("catalog", discovery).set("state", "finished").set("execution", executionB).set("cases", lang::Json::array().push(failed)));
    EXPECT_EQ(table->rowCount(), 0);
    widget->findChild<QPushButton*>("unityTestsRunSelected")->click();
    widget->findChild<QPushButton*>("unityTestsRunFailed")->click();
    EXPECT_EQ(requests.size(), 4u);
    // Explicit observation can reacquire the actual original receipt; the rejected page did not.
    widget->findChild<QPushButton*>("unityTestsObserve")->click();
    ASSERT_EQ(requests.size(), 5u);
    deliver(4, state(job, "finished").set("catalog", discovery).set("execution", executionA));
    ASSERT_EQ(requests.size(), 6u);
    deliver(
        5,
        page(job).set("catalog", discovery).set("state", "finished").set("execution", executionA).set("cases", lang::Json::array().push(failed)));
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("Failed"));
}
TEST(UnityTestsWindow, RunReceiptMustMatchRequestedExecutionInReplyAndRecovery) {
    constexpr auto execution = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    for (bool recover : {false, true}) {
        for (const auto* variant : {"discovered", "empty execution", "nil execution", "owner", "catalog", "mode",
                                    "failed receipt", "valid"}) {
            SCOPED_TRACE(variant);
            SCOPED_TRACE(recover);
            UnityEditorIdentity identity;
            identity.root = "/fixture";
            identity.instance = std::string(32, 'd');
            identity.session = std::string(32, 'e');
            identity.generation = 1;
            identity.epoch = std::make_shared<std::atomic_uint64_t>(0);
            identity.tests = true;
            struct Request {
                lang::Json payload;
                std::function<void(Result<lang::Json>)> done;
            };
            std::vector<Request> requests;
            UnityTestsWindowContext context;
            context.selected = [&]() { return std::optional<UnityEditorIdentity>{identity}; };
            context.request = [&](const auto&, const auto& payload, auto done) {
                requests.push_back({payload, std::move(done)});
            };
            std::unique_ptr<QWidget> widget(createUnityTestsWidget(nullptr, std::move(context)));
            const auto deliver = [&](size_t index, lang::Json response) {
                auto done = std::move(requests.at(index).done);
                done(std::move(response));
            };
            widget->findChild<QPushButton*>("unityTestsDiscover")->click();
            ASSERT_EQ(requests.size(), 1u);
            const std::string discovery(requests[0].payload.stringAt("job"));
            deliver(0, state(discovery));
            ASSERT_EQ(requests.size(), 2u);
            deliver(1, page(discovery));
            auto* table = widget->findChild<QTableWidget*>();
            ASSERT_EQ(table->rowCount(), 1);
            auto* run = widget->findChild<QPushButton*>("unityTestsRunAll");
            run->click();
            ASSERT_EQ(requests.size(), 3u);
            const std::string job(requests[2].payload.stringAt("job"));
            auto receipt = state(job, "finished").set("catalog", discovery).set("execution", execution);
            const std::string_view scenario(variant);
            if (scenario == "discovered")
                receipt.set("state", "discovered").set("execution", "");
            else if (scenario == "empty execution")
                receipt.set("execution", "");
            else if (scenario == "nil execution")
                receipt.set("execution", "00000000-0000-0000-0000-000000000000");
            else if (scenario == "owner")
                receipt.set("owner", std::string(32, 'b'));
            else if (scenario == "catalog")
                receipt.set("catalog", std::string(32, 'c'));
            else if (scenario == "mode")
                receipt.set("mode", "PlayMode");
            else if (scenario == "failed receipt")
                receipt.set("ok", false);

            if (recover) {
                identity.epoch->store(1);
                identity.revision = 1;
                identity.generation = 2;
                identity.session = std::string(32, 'f');
                deliver(2, receipt);
                widget->findChild<QPushButton*>("unityTestsObserve")->click();
                ASSERT_EQ(requests.size(), 4u);
                EXPECT_EQ(requests[3].payload.stringAt("op"), "observe");
            }

            const size_t receiptIndex = recover ? 3 : 2;
            deliver(receiptIndex, std::move(receipt));
            if (scenario == "valid") {
                ASSERT_EQ(requests.size(), receiptIndex + 2);
                const auto& snapshot = requests.back().payload;
                EXPECT_EQ(snapshot.stringAt("op"), "snapshot");
                EXPECT_EQ(snapshot.stringAt("job"), job);
                auto failed = row().set("outcome", "Failed");
                deliver(receiptIndex + 1, page(job)
                                              .set("catalog", discovery)
                                              .set("state", "finished")
                                              .set("execution", execution)
                                              .set("cases", lang::Json::array().push(failed)));
                ASSERT_EQ(table->rowCount(), 1);
                EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("Failed"));
            } else {
                ASSERT_EQ(requests.size(), receiptIndex + 1);
                EXPECT_FALSE(run->isEnabled());
                ASSERT_EQ(table->rowCount(), 1);
                EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("Discovered"));
            }
        }
    }
}
} // namespace frost::app
