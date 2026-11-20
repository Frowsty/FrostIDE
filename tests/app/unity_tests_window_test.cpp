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
} // namespace frost::app
