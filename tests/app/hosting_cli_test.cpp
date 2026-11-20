// SPDX-License-Identifier: Apache-2.0
// Provider contract fixtures and admission/cancellation tests; no real provider or network is contacted.
#include "../../src/app/src/hosting_cli.hpp"
#include "../../src/app/src/hosting_panel.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <gtest/gtest.h>
#include <algorithm>
namespace frost::app {
namespace {
HostingRequest request() {
    HostingRequest result;
#ifdef _WIN32
    result.executable = "C:/tools/gh.exe"; result.root = "C:/project";
#else
    result.executable = "/tools/gh"; result.root = "/project";
#endif
    result.host = "github.example"; result.repository = "owner/repo"; result.number = 42;
    result.head = std::string(40, 'a');
    return result;
}
std::string githubRow(char head = 'a') {
    return "{\"number\":42,\"title\":\"Actual provider title\",\"state\":\"open\",\"html_url\":\"https://github.example/owner/repo/pull/42\",\"user\":{\"login\":\"author\"},\"head\":{\"ref\":\"topic\",\"sha\":\"" + std::string(40, head) + "\"},\"base\":{\"ref\":\"main\"}}";
}
TEST(HostingCli, ParsesBothProviderContractsAndRejectsIncompleteOrDuplicateRows) {
    const auto github = parseHostedReviews(HostingProvider::GitHub, "[" + githubRow() + "]");
    ASSERT_TRUE(github); ASSERT_EQ(github->size(), 1U);
    EXPECT_EQ(github->front().number, 42U); EXPECT_EQ(github->front().source, "topic");
    const auto gitlab = parseHostedReviews(HostingProvider::GitLab,
        "[{\"iid\":17,\"title\":\"MR title\",\"state\":\"opened\",\"web_url\":\"https://gitlab.example/group/repo/-/merge_requests/17\",\"author\":{\"username\":\"author\"},\"source_branch\":\"topic\",\"target_branch\":\"main\",\"sha\":\"" + std::string(40, 'b') + "\"}]");
    ASSERT_TRUE(gitlab); EXPECT_EQ(gitlab->front().number, 17U);
    EXPECT_FALSE(parseHostedReviews(HostingProvider::GitHub, "[{}]"));
    EXPECT_FALSE(parseHostedReviews(HostingProvider::GitHub, "[" + githubRow() + "," + githubRow() + "]"));
    EXPECT_FALSE(parseHostedReviews(HostingProvider::GitHub, "{\"message\":\"unauthorized\"}"));
}
TEST(HostingCli, ReviewBodyUsesJsonStdinAndCapturedCommitInsteadOfCommandArguments) {
    auto selected = request(); selected.operation = HostingOperation::RequestChanges;
    selected.body = "Line one\n`literal` $(literal) \"quotes\"";
    auto command = hostingCommand(selected);
    ASSERT_TRUE(command);
    EXPECT_EQ(command->process.stdinMode, io::Stdio::Pipe);
    EXPECT_EQ(std::ranges::find(command->process.arguments, selected.body), command->process.arguments.end());
    const auto body = QJsonDocument::fromJson(QByteArray::fromStdString(command->input)).object();
    EXPECT_EQ(body.value(QStringLiteral("body")).toString().toStdString(), selected.body);
    EXPECT_EQ(body.value(QStringLiteral("commit_id")).toString().toStdString(), selected.head);
    EXPECT_EQ(body.value(QStringLiteral("event")).toString(), QStringLiteral("REQUEST_CHANGES"));
    selected.provider = HostingProvider::GitLab; selected.operation = HostingOperation::Approve;
    selected.repository = "group/subgroup/repo";
    auto approved = hostingCommand(selected); ASSERT_TRUE(approved);
    EXPECT_TRUE(approved->process.arguments[1].starts_with("projects/group%2Fsubgroup%2Frepo/merge_requests/42/approve"));
    EXPECT_EQ(QJsonDocument::fromJson(QByteArray::fromStdString(approved->input)).object().value(QStringLiteral("sha")).toString().toStdString(), selected.head);
}
TEST(HostingCli, InvalidPathsHostsRepositoriesAndMissingHeadNeverPrepareACommand) {
    auto selected = request(); selected.executable = "gh"; EXPECT_FALSE(hostingCommand(selected));
    selected = request(); selected.host = "https://bad"; EXPECT_FALSE(hostingCommand(selected));
    selected = request(); selected.repository = "owner/../repo"; EXPECT_FALSE(hostingCommand(selected));
    selected = request(); selected.operation = HostingOperation::Approve; selected.head.clear(); EXPECT_FALSE(hostingCommand(selected));
}
TEST(HostingCli, RefusedOrCancelledAdmissionLaunchesNothingAndFailuresStayErrors) {
    io::ScriptedProcessLauncher launcher;
    auto selected = request();
    EXPECT_FALSE(runHosting(launcher, selected, [] { return false; }));
    std::stop_source stop; stop.request_stop();
    EXPECT_FALSE(runHosting(launcher, selected, [] { return true; }, stop.get_token()));
    EXPECT_TRUE(launcher.launches().empty());
    launcher.expect(io::ScriptedProcess{.exitCode = 1, .stderrBytes = "authentication required"});
    auto failed = runHosting(launcher, selected, [] { return true; });
    ASSERT_FALSE(failed); EXPECT_NE(failed.error().message().find("authentication required"), std::string::npos);
}
TEST(HostingCli, HeadChangeDuringDiffDiscardsTheEntireReturnedPatch) {
    io::ScriptedProcessLauncher launcher;
    launcher.expect(io::ScriptedProcess{.stdoutBytes = githubRow()});
    launcher.expect(io::ScriptedProcess{.stdoutBytes = "diff --git a/file b/file\nactual patch"});
    launcher.expect(io::ScriptedProcess{.stdoutBytes = githubRow('b')});
    auto selected = request(); selected.operation = HostingOperation::Diff;
    auto result = runHosting(launcher, selected, [] { return true; });
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code(), ErrorCode::Busy);
    EXPECT_EQ(launcher.launches().size(), 3U);
}
TEST(HostingCli, PanelCreationAndContextInvalidationNeverContactAProvider) {
    io::ScriptedProcessLauncher launcher;
    const auto selected = request();
    HostingPanel panel({&launcher, [selected] { return selected.root; }, [](const std::string&) { return true; }});
    panel.findChild<QLineEdit*>(QStringLiteral("hostingExecutable"))->setText(QString::fromStdString(selected.executable));
    panel.findChild<QLineEdit*>(QStringLiteral("hostingRepository"))->setText(QString::fromStdString(selected.repository));
    panel.invalidate();
    EXPECT_TRUE(launcher.launches().empty());
}
}
}
