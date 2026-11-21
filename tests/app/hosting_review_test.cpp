// SPDX-License-Identifier: Apache-2.0
// Deferred provider-format/admission fixtures for anchored reviews; no CLI/provider is contacted.
#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "../../src/app/src/hosting_cli.hpp"
#include "../../src/app/src/hosting_panel.hpp"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
namespace frost::app {
namespace {
std::string hash(char letter) { return std::string(40, letter); }
HostingRequest request(HostingProvider provider = HostingProvider::GitHub) {
    HostingRequest result;
    result.provider = provider;
#ifdef _WIN32
    result.executable = "C:/tools/provider.exe";
    result.root = "C:/project";
#else
    result.executable = "/tools/provider";
    result.root = "/project";
#endif
    result.host = "hosting.example";
    result.repository = "owner/repo";
    result.number = 42;
    result.head = hash('a');
    result.body = "literal `body` $(nothing)\nsecond line";
    return result;
}
std::string details(HostingProvider provider, char base = 'b', char head = 'a') {
    QJsonObject row{{"number", 42},
                    {"iid", 42},
                    {"title", "Review"},
                    {"state", provider == HostingProvider::GitHub ? "open" : "opened"},
                    {"html_url", "https://hosting.example/review/42"},
                    {"web_url", "https://hosting.example/review/42"},
                    {"user", QJsonObject{{"login", "author"}}},
                    {"author", QJsonObject{{"username", "author"}}},
                    {"source_branch", "topic"},
                    {"target_branch", "main"},
                    {"sha", QString::fromStdString(hash(head))},
                    {"head", QJsonObject{{"ref", "topic"}, {"sha", QString::fromStdString(hash(head))}}},
                    {"base", QJsonObject{{"ref", "main"}, {"sha", QString::fromStdString(hash(base))}}},
                    {"diff_refs", QJsonObject{{"head_sha", QString::fromStdString(hash(head))},
                                              {"base_sha", QString::fromStdString(hash(base))},
                                              {"start_sha", QString::fromStdString(hash('c'))}}}};
    return QJsonDocument(row).toJson().toStdString();
}
std::string files(HostingProvider provider, std::string patch = "@@ -10,3 +10,3 @@\n same\n-old\n+new\n final\n") {
    const QJsonObject row = provider == HostingProvider::GitHub ? QJsonObject{{"filename", "src/new name.cpp"},
                                                                              {"previous_filename", "old name.cpp"},
                                                                              {"status", "renamed"},
                                                                              {"additions", 1},
                                                                              {"deletions", 1},
                                                                              {"changes", 2},
                                                                              {"patch", QString::fromStdString(patch)}}
                                                                : QJsonObject{{"old_path", "old name.cpp"},
                                                                              {"new_path", "src/new name.cpp"},
                                                                              {"diff", QString::fromStdString(patch)}};
    return QJsonDocument(QJsonArray{row}).toJson().toStdString();
}
HostingRequest anchored(HostingProvider provider = HostingProvider::GitHub) {
    auto result = request(provider);
    auto identity = parseHostedDiffIdentity(provider, details(provider));
    EXPECT_TRUE(identity);
    auto diff = parseHostedDiffFiles(result, *identity, files(provider));
    EXPECT_TRUE(diff);
    result.diff = std::make_shared<const HostedDiffSnapshot>(std::move(*diff));
    result.operation = HostingOperation::InlineComment;
    result.lineIndex = 2;
    return result;
}
TEST(HostedReview, ActualHunkRowsPinRenamedPathSideAndCommitWithJsonStdin) {
    auto selected = anchored();
    ASSERT_EQ(selected.diff->files.front().lines.size(), 4u);
    auto command = hostingCommand(selected);
    ASSERT_TRUE(command);
    const auto body = QJsonDocument::fromJson(QByteArray::fromStdString(command->input)).object();
    EXPECT_EQ(body.value("path").toString(), "src/new name.cpp");
    EXPECT_EQ(body.value("line").toInteger(), 11);
    EXPECT_EQ(body.value("side").toString(), "RIGHT");
    EXPECT_EQ(body.value("commit_id").toString().toStdString(), hash('a'));
    EXPECT_EQ(body.value("body").toString().toStdString(), selected.body);
    EXPECT_EQ(command->process.stdinMode, io::Stdio::Pipe);
    selected.lineIndex = 1;
    selected.right = false;
    command = hostingCommand(selected);
    ASSERT_TRUE(command);
    EXPECT_EQ(QJsonDocument::fromJson(QByteArray::fromStdString(command->input)).object().value("side").toString(),
              "LEFT");
    selected.right = true;
    EXPECT_FALSE(hostingCommand(selected));
}
TEST(HostedReview, GitlabUsesFullDiffRefsAndBothContextCoordinates) {
    auto selected = anchored(HostingProvider::GitLab);
    selected.lineIndex = 0;
    auto command = hostingCommand(selected);
    ASSERT_TRUE(command);
    const auto position =
        QJsonDocument::fromJson(QByteArray::fromStdString(command->input)).object().value("position").toObject();
    EXPECT_EQ(position.value("head_sha").toString().toStdString(), hash('a'));
    EXPECT_EQ(position.value("base_sha").toString().toStdString(), hash('b'));
    EXPECT_EQ(position.value("start_sha").toString().toStdString(), hash('c'));
    EXPECT_EQ(position.value("old_path").toString(), "old name.cpp");
    EXPECT_EQ(position.value("new_path").toString(), "src/new name.cpp");
    EXPECT_EQ(position.value("old_line").toInteger(), 10);
    EXPECT_EQ(position.value("new_line").toInteger(), 10);
}
TEST(HostedReview, ProviderResponsesRetainOriginalHeadAndFullGitlabPosition) {
    auto github = anchored();
    QJsonObject response{{"id", 700},
                         {"body", QString::fromStdString(github.body)},
                         {"path", "src/new name.cpp"},
                         {"commit_id", QString::fromStdString(hash('b'))},
                         {"original_commit_id", QString::fromStdString(hash('a'))},
                         {"line", 20},
                         {"original_line", 11},
                         {"side", "RIGHT"}};
    EXPECT_TRUE(validateHostedCommentMutation(github, QJsonDocument(response).toJson().toStdString()));
    auto read = parseHostedComments(github, "[" + QJsonDocument(response).toJson().toStdString() + "]");
    ASSERT_TRUE(read);
    EXPECT_EQ(read->comments.front().originalHead, hash('a'));
    EXPECT_EQ(read->comments.front().newLine, 20u);
    EXPECT_EQ(read->comments.front().originalLine, 11u);
    auto gitlab = anchored(HostingProvider::GitLab);
    auto command = hostingCommand(gitlab);
    ASSERT_TRUE(command);
    auto position =
        QJsonDocument::fromJson(QByteArray::fromStdString(command->input)).object().value("position").toObject();
    QJsonObject note{{"id", 701},
                     {"body", QString::fromStdString(gitlab.body)},
                     {"author", QJsonObject{{"username", "reviewer"}}},
                     {"position", position},
                     {"resolved", false}};
    QJsonObject discussion{{"id", "abc123"}, {"notes", QJsonArray{note}}};
    EXPECT_TRUE(validateHostedCommentMutation(gitlab, QJsonDocument(discussion).toJson().toStdString()));
    position.insert("base_sha", QString::fromStdString(hash('c')));
    note.insert("position", position);
    discussion.insert("notes", QJsonArray{note});
    EXPECT_FALSE(validateHostedCommentMutation(gitlab, QJsonDocument(discussion).toJson().toStdString()));
}

TEST(HostedReview, MissingTruncatedOrForgedPatchCannotBecomeAnAnchor) {
    auto selected = anchored();
    const auto identity = selected.diff->identity;
    auto incomplete = parseHostedDiffFiles(selected, identity, files(selected.provider, "@@ -10,3 +10,3 @@\n short\n"));
    ASSERT_TRUE(incomplete);
    EXPECT_TRUE(incomplete->files.front().lines.empty());
    EXPECT_FALSE(incomplete->files.front().unavailable.empty());
    auto missingPath = QJsonDocument::fromJson(QByteArray::fromStdString(files(selected.provider))).array();
    auto renamed = missingPath[0].toObject();
    renamed.remove("previous_filename");
    missingPath[0] = renamed;
    EXPECT_FALSE(parseHostedDiffFiles(selected, identity, QJsonDocument(missingPath).toJson().toStdString()));
    auto totals = QJsonDocument::fromJson(QByteArray::fromStdString(files(selected.provider))).array();
    auto truncated = totals[0].toObject();
    truncated.insert("additions", 2);
    truncated.insert("changes", 3);
    totals[0] = truncated;
    auto prefix = parseHostedDiffFiles(selected, identity, QJsonDocument(totals).toJson().toStdString());
    ASSERT_TRUE(prefix);
    EXPECT_TRUE(prefix->files.front().lines.empty());
    auto forged = std::make_shared<HostedDiffSnapshot>(*selected.diff);
    forged->files[0].lines[2].newLine = 99;
    selected.diff = forged;
    EXPECT_FALSE(hostingCommand(selected));
    selected = anchored();
    selected.repository = "another/repository";
    EXPECT_FALSE(hostingCommand(selected));
}
TEST(HostedReview, RevokedAdmissionAfterNestedMetadataCheckNeverLaunchesPost) {
    io::ScriptedProcessLauncher launcher;
    auto selected = anchored();
    launcher.expect({.stdoutBytes = details(selected.provider)});
    bool metadataReturned = false;
    const auto admitted = [&] {
        if (launcher.launches().size() == 1) {
            if (metadataReturned)
                return false;
            metadataReturned = true;
        }
        return true;
    };
    auto result = runHosting(launcher, selected, admitted);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(launcher.launches().size(), 1u);
}

TEST(HostedReview, SameHeadButChangedBaseBeforePostSendsNoComment) {
    io::ScriptedProcessLauncher launcher;
    auto selected = anchored();
    launcher.expect({.stdoutBytes = details(selected.provider, 'c')});
    auto result = runHosting(launcher, selected, [] { return true; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Busy);
    ASSERT_EQ(launcher.launches().size(), 1u);
    EXPECT_EQ(launcher.launches()[0].spec.arguments[0], "api");
}
TEST(HostedReview, ChangedBaseDuringFileFetchDiscardsAllAnchors) {
    io::ScriptedProcessLauncher launcher;
    auto selected = request();
    launcher.expect({.stdoutBytes = details(selected.provider)});
    launcher.expect({.stdoutBytes = files(selected.provider)});
    launcher.expect({.stdoutBytes = details(selected.provider, 'c')});
    EXPECT_FALSE(loadHostedDiff(launcher, selected, [] { return true; }));
    EXPECT_EQ(launcher.launches().size(), 3u);
}
TEST(HostedReview, ExistingOriginalPositionsAndThreadRootsRemainProviderData) {
    auto selected = request();
    auto page = parseHostedComments(
        selected, "[{\"id\":11,\"in_reply_to_id\":10,\"user\":{\"login\":\"reviewer\"},\"body\":\"existing "
                  "reply\",\"path\":\"file.cpp\",\"line\":null,\"original_line\":8,\"commit_id\":\"" +
                      hash('a') + "\"}]");
    ASSERT_TRUE(page);
    ASSERT_EQ(page->comments.size(), 1u);
    EXPECT_FALSE(page->comments[0].newLine);
    EXPECT_EQ(page->comments[0].originalLine, 8u);
    selected.comments = std::make_shared<const HostedCommentPage>(*page);
    selected.operation = HostingOperation::Reply;
    auto command = hostingCommand(selected);
    ASSERT_TRUE(command);
    EXPECT_NE(command->process.arguments[1].find("/comments/10/replies"), std::string::npos);
    selected.body = "reply";
    EXPECT_TRUE(validateHostedCommentMutation(selected, R"({"id":12,"body":"reply","in_reply_to_id":10})"));
    EXPECT_FALSE(validateHostedCommentMutation(selected, R"({"id":12,"body":"reply","in_reply_to_id":99})"));
}
TEST(HostedReview, OversizedPostResponseRetainsUnconfirmedDispatchState) {
    io::ScriptedProcessLauncher launcher;
    auto selected = anchored();
    selected.dispatched = std::make_shared<std::atomic_bool>(false);
    launcher.expect({.stdoutBytes = details(selected.provider)});
    launcher.expect({.stdoutBytes = std::string(8U * 1024U * 1024U + 1U, 'x')});
    auto result = runHosting(launcher, selected, [] { return true; });
    ASSERT_FALSE(result);
    EXPECT_TRUE(selected.dispatched->load());
    EXPECT_NE(result.error().message().find("unconfirmed"), std::string::npos);
    EXPECT_EQ(launcher.launches().size(), 2u);
}

TEST(HostedReview, SuccessfulExitWithoutConfirmedAnchorRemainsUncertainAndNeverRetries) {
    io::ScriptedProcessLauncher launcher;
    auto selected = anchored();
    launcher.expect({.stdoutBytes = details(selected.provider)});
    launcher.expect(
        {.stdoutBytes =
             R"({"id":23,"body":"posted","path":"another.cpp","line":11,"original_line":11,"side":"RIGHT","commit_id":")" +
             hash('a') + "\"}"});
    auto result = runHosting(launcher, selected, [] { return true; });
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("unconfirmed"), std::string::npos);
    EXPECT_EQ(launcher.launches().size(), 2u);
}
bool waitFor(const std::function<bool()>& done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        QCoreApplication::processEvents();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < until);
    return false;
}
QPushButton* button(HostingPanel& panel, const QString& title) {
    for (auto* child : panel.findChildren<QPushButton*>())
        if (child->text() == title)
            return child;
    return nullptr;
}
void configure(HostingPanel& panel, const HostingRequest& selected) {
    panel.findChild<QLineEdit*>("hostingExecutable")->setText(QString::fromStdString(selected.executable));
    panel.findChild<QLineEdit*>("hostingHost")->setText(QString::fromStdString(selected.host));
    panel.findChild<QLineEdit*>("hostingRepository")->setText(QString::fromStdString(selected.repository));
}
void confirmNext() {
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            static_cast<QDialog*>(dialog)->done(QMessageBox::Yes);
    });
}
std::string comment(std::uint64_t id, std::uint64_t parent = 0, std::string body = "Actual provider comment fixture") {
    QJsonObject row{{"id", static_cast<qint64>(id)},
                    {"user", QJsonObject{{"login", "reviewer"}}},
                    {"body", QString::fromStdString(body)},
                    {"path", "src/new name.cpp"},
                    {"commit_id", QString::fromStdString(hash('a'))},
                    {"line", 11},
                    {"original_line", 11},
                    {"side", "RIGHT"}};
    if (parent)
        row.insert("in_reply_to_id", static_cast<qint64>(parent));
    return QJsonDocument(row).toJson(QJsonDocument::Compact).toStdString();
}
TEST(HostedReviewPanel, NormalDiffRowPostsExactAnchorThenRefreshesAndRepliesToActualThread) {
    io::ScriptedProcessLauncher launcher;
    const auto selected = request();
    HostingPanel panel({&launcher, [selected] { return selected.root; }, [](const auto&) { return true; },
                        [](const auto&) { return 1; }});
    configure(panel, selected);
    launcher.expect({.stdoutBytes = "[" + details(selected.provider) + "]"});
    button(panel, "Load Page")->click();
    auto* reviews = panel.findChild<QTreeWidget*>("hostingReviews");
    ASSERT_TRUE(waitFor([&] { return reviews->topLevelItemCount() == 1; }));
    reviews->setCurrentItem(reviews->topLevelItem(0));
    launcher.expect({.stdoutBytes = details(selected.provider)});
    launcher.expect({.stdoutBytes = files(selected.provider)});
    launcher.expect({.stdoutBytes = details(selected.provider)});
    button(panel, "Load Review Lines")->click();
    auto* lines = panel.findChild<QTreeWidget*>("hostingDiffLines");
    ASSERT_TRUE(waitFor([&] { return lines->topLevelItemCount() == 4; }));
    lines->setCurrentItem(lines->topLevelItem(2));
    auto* body = panel.findChild<QPlainTextEdit*>("hostingReviewBody");
    body->setPlainText("Review exact added line");
    ASSERT_TRUE(button(panel, "Send Line Comment...")->isEnabled());
    launcher.expect({.stdoutBytes = details(selected.provider)});
    launcher.expect({.stdoutBytes = comment(700, 0, "Review exact added line")});
    launcher.expect({.stdoutBytes = "[" + comment(700, 0, "Review exact added line") + "]"});
    confirmNext();
    button(panel, "Send Line Comment...")->click();
    auto* comments = panel.findChild<QTreeWidget*>("hostingComments");
    ASSERT_TRUE(waitFor([&] { return comments->topLevelItemCount() == 1 && body->toPlainText().isEmpty(); }));
    const auto sent = launcher.launches();
    ASSERT_EQ(sent.size(), 7u);
    const auto posted = QJsonDocument::fromJson(QByteArray::fromStdString(sent[5].stdinBytes)).object();
    EXPECT_EQ(posted.value("path").toString(), "src/new name.cpp");
    EXPECT_EQ(posted.value("line").toInteger(), 11);
    EXPECT_EQ(posted.value("commit_id").toString().toStdString(), hash('a'));
    comments->setCurrentItem(comments->topLevelItem(0));
    body->setPlainText("Thread reply");
    launcher.expect({.stdoutBytes = comment(701, 700, "Thread reply")});
    launcher.expect({.stdoutBytes = "[" + comment(700, 0, "Review exact added line") + "," +
                                    comment(701, 700, "Thread reply") + "]"});
    confirmNext();
    button(panel, "Reply to Thread...")->click();
    ASSERT_TRUE(waitFor([&] { return comments->topLevelItemCount() == 2 && body->toPlainText().isEmpty(); }));
    const auto replied = launcher.launches();
    ASSERT_EQ(replied.size(), 9u);
    EXPECT_NE(replied[7].spec.arguments[1].find("/comments/700/replies"), std::string::npos);
}
TEST(HostedReviewPanel, AnchorChangeAfterPostDispatchRequiresRefreshBeforeAnyRetry) {
    io::ScriptedProcessLauncher launcher;
    const auto selected = request();
    HostingPanel panel({&launcher, [selected] { return selected.root; }, [](const auto&) { return true; }, {}});
    configure(panel, selected);
    launcher.expect({.stdoutBytes = "[" + details(selected.provider) + "]"});
    button(panel, "Load Page")->click();
    auto* reviews = panel.findChild<QTreeWidget*>("hostingReviews");
    ASSERT_TRUE(waitFor([&] { return reviews->topLevelItemCount() == 1; }));
    reviews->setCurrentItem(reviews->topLevelItem(0));
    launcher.expect({.stdoutBytes = details(selected.provider)});
    launcher.expect({.stdoutBytes = files(selected.provider)});
    launcher.expect({.stdoutBytes = details(selected.provider)});
    button(panel, "Load Review Lines")->click();
    auto* lines = panel.findChild<QTreeWidget*>("hostingDiffLines");
    ASSERT_TRUE(waitFor([&] { return lines->topLevelItemCount() == 4; }));
    lines->setCurrentItem(lines->topLevelItem(2));
    auto* body = panel.findChild<QPlainTextEdit*>("hostingReviewBody");
    body->setPlainText("Retained possible submission");
    launcher.expect({.stdoutBytes = details(selected.provider)});
    launcher.expect({.neverExits = true});
    confirmNext();
    button(panel, "Send Line Comment...")->click();
    ASSERT_TRUE(waitFor([&] { return launcher.launches().size() == 6; }));
    lines->setCurrentItem(lines->topLevelItem(0));
    ASSERT_TRUE(waitFor([&] { return button(panel, "Refresh Comments")->isEnabled(); }));
    EXPECT_FALSE(button(panel, "Send Line Comment...")->isEnabled());
    EXPECT_EQ(body->toPlainText(), "Retained possible submission");
    EXPECT_EQ(launcher.launches().size(), 6u);
    launcher.expect({.stdoutBytes = "[]"});
    button(panel, "Refresh Comments")->click();
    ASSERT_TRUE(waitFor([&] { return button(panel, "Send Line Comment...")->isEnabled(); }));
    EXPECT_EQ(launcher.launches().size(), 7u);
}

// A hung ScriptedProcess normally advances the entire synchronous timeout immediately.
// Keep this test's second request in flight until the owner actually cancels its process.
class ProviderWaitingForCancellation final : public io::IProcess {
public:
    explicit ProviderWaitingForCancellation(std::unique_ptr<io::IProcess> child) : m_child(std::move(child)) {}
    io::ProcessId pid() const noexcept override { return m_child->pid(); }
    Result<void> writeStdin(std::string_view bytes) override { return m_child->writeStdin(bytes); }
    void closeStdin() noexcept override { m_child->closeStdin(); }
    Result<std::size_t> readStdout(std::span<char> bytes) override { return m_child->readStdout(bytes); }
    Result<std::size_t> readStderr(std::span<char> bytes) override { return m_child->readStderr(bytes); }
    Result<io::ExitStatus> wait(std::optional<IClock::Duration> timeout) override { return m_child->wait(timeout); }
    Result<void> terminate() override { return m_child->terminate(); }
    Result<io::ProcessResult> communicate(std::string_view bytes, IClock::Duration) override {
        return m_child->communicateUntil(bytes, std::nullopt);
    }

private:
    std::unique_ptr<io::IProcess> m_child;
};

class ProviderCancellationLauncher final : public io::IProcessLauncher {
public:
    io::ScriptedProcessLauncher scripts;
    std::atomic<bool> waitForCancellation = false;

    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        auto child = scripts.launch(spec);
        if (!child || !waitForCancellation.exchange(false))
            return child;
        return std::make_unique<ProviderWaitingForCancellation>(std::move(*child));
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                          const io::AsyncContext& context) override {
        return scripts.launchAsync(spec, context);
    }
};

TEST(HostedReviewPanel, RevokedAndRegrantedTrustCannotContinueAnOldProviderRequest) {
    ProviderCancellationLauncher launcher;
    const auto selected = request();
    ManualClock clock;
    io::MemFs fs(clock);
    auto trust = project::TrustStore::open(fs, io::joinPath(selected.root, "trusted-roots.txt").value());
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(selected.root));
    const auto original = (*trust)->snapshot(selected.root);
    HostingPanel panel({&launcher, [selected] { return selected.root; },
                        [&](const auto& root) { return (*trust)->isTrusted(root); },
                        [&](const auto& root) { return (*trust)->snapshot(root).revision; }});
    configure(panel, selected);
    launcher.scripts.expect({.stdoutBytes = "[" + details(selected.provider) + "]"});
    button(panel, "Load Page")->click();
    auto* reviews = panel.findChild<QTreeWidget*>("hostingReviews");
    ASSERT_TRUE(waitFor([&] { return reviews->topLevelItemCount() == 1; }));
    reviews->setCurrentItem(reviews->topLevelItem(0));
    launcher.scripts.expect({.neverExits = true});
    launcher.waitForCancellation = true;
    button(panel, "Load Review Lines")->click();
    ASSERT_TRUE(waitFor([&] { return launcher.scripts.launches().size() == 2; }));
    ASSERT_TRUE((*trust)->untrust(selected.root));
    EXPECT_FALSE((*trust)->snapshot(selected.root).trusted);
    ASSERT_TRUE((*trust)->trust(selected.root));
    const auto restored = (*trust)->snapshot(selected.root);
    EXPECT_TRUE(restored.trusted);
    EXPECT_NE(restored.revision, original.revision);
    ASSERT_TRUE(waitFor([&] {
        const auto children = launcher.scripts.launches();
        return children.size() == 2 && children.back().terminated && button(panel, "Load Review Lines")->isEnabled();
    }));
    EXPECT_EQ(launcher.scripts.launches().size(), 2u);
    EXPECT_EQ(panel.findChild<QTreeWidget*>("hostingDiffLines")->topLevelItemCount(), 0);
    const QString status = panel.findChild<QLabel*>("hostingStatus")->text();
    EXPECT_TRUE(status.contains("trust changed")) << status.toStdString();
}

TEST(HostedReviewPanel, ReviewSelectionChangedInsideConfirmationCannotSendOldLineComment) {
    io::ScriptedProcessLauncher launcher;
    const auto selected = request();
    HostingPanel panel({&launcher, [selected] { return selected.root; }, [](const auto&) { return true; }, {}});
    configure(panel, selected);
    auto other = QJsonDocument::fromJson(QByteArray::fromStdString(details(selected.provider))).object();
    other.insert("number", 43);
    launcher.expect(
        {.stdoutBytes = "[" + details(selected.provider) + "," + QJsonDocument(other).toJson().toStdString() + "]"});
    button(panel, "Load Page")->click();
    auto* reviews = panel.findChild<QTreeWidget*>("hostingReviews");
    ASSERT_TRUE(waitFor([&] { return reviews->topLevelItemCount() == 2; }));
    reviews->setCurrentItem(reviews->topLevelItem(0));
    launcher.expect({.stdoutBytes = details(selected.provider)});
    launcher.expect({.stdoutBytes = files(selected.provider)});
    launcher.expect({.stdoutBytes = details(selected.provider)});
    button(panel, "Load Review Lines")->click();
    auto* lines = panel.findChild<QTreeWidget*>("hostingDiffLines");
    ASSERT_TRUE(waitFor([&] { return lines->topLevelItemCount() == 4; }));
    lines->setCurrentItem(lines->topLevelItem(2));
    auto* body = panel.findChild<QPlainTextEdit*>("hostingReviewBody");
    body->setPlainText("Retained draft");
    QTimer::singleShot(0, [&] {
        reviews->setCurrentItem(reviews->topLevelItem(1));
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            static_cast<QDialog*>(dialog)->done(QMessageBox::Yes);
    });
    button(panel, "Send Line Comment...")->click();
    EXPECT_EQ(launcher.launches().size(), 4u);
    EXPECT_EQ(body->toPlainText(), "Retained draft");
    EXPECT_EQ(lines->topLevelItemCount(), 0);
}
} // namespace
} // namespace frost::app
