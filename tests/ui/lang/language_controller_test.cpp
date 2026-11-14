// SPDX-License-Identifier: Apache-2.0
//
// LanguageController offscreen against the scripted language server (tests/lang/feature_server)
// and a fake editor host over a TextBuffer: completion from Ctrl+Space and from typing '.',
// Enter commits and Esc cancels, parameter info on '(', go to declaration into another file,
// related files, find usages into the panel, highlight usages after the caret debounce, the
// file structure popup, rename and quick fixes through the edit applier (a refusal reaches the
// status bar), reformat, and the registered IntelliJ action ids; and (FND-3, ADR 0078) metadata
// sources only from the server and only into the metadata cache, honest empty-result messages,
// Refactor This and the named commands through clangd tweaks (or disabled with their epic), the
// include cleaner, and the language capability table keeping C++/C# features out of Lua files.

#include "frost/input/action.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "../../../src/ui/lang/src/language_controller_internal.hpp"
#include "lang/feature_server.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QShortcut>
#include <QThread>
#include <QTimer>
#include <QInputDialog>
#include <QTreeWidget>
#include <QWidget>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace frost::ui::lang {

struct LanguageControllerTestAccess {
    static void startInteger(LanguageController& controller, sched::LatestWins& slot,
                             std::function<sched::Task<Result<int>>(std::stop_token)> factory,
                             std::function<void(Result<int>)> done) {
        controller.start<int>(slot, std::move(factory), std::move(done));
    }
};

} // namespace frost::ui::lang

namespace {

namespace fl = frost::lang;
TEST(LanguageControllerRefactoring, ModuleAndCudaPathsUseCppSemanticAdmission) {
    for (const auto path : {"module.cppm", "module.ixx", "kernel.cu", "MODULE.CPPM"})
        EXPECT_EQ(frost::ui::lang::languageIdForPath(path), "cpp");
}
using frost::ByteOffset;
using frost::lang::Json;
using frost::test::FeatureServer;
using frost::test::workPath;
using frost::ui::lang::LanguageController;

// Processes events until `condition` holds (the controller posts results to the UI thread).
bool pumpUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

Json rangeJson(std::uint32_t line, std::uint32_t from, std::uint32_t to) {
    return fl::toJson(fl::LspRange{{line, from}, {line, to}});
}

std::string uriOf(const std::string& path) { return fl::pathToUri(path).value(); }

// The directory of the test documents (workPath's parent).
std::string workDir() { return std::string(frost::io::parentPath(workPath("x"))); }

// The active editor: one document in a TextBuffer.
class FakeHost final : public frost::ui::lang::IEditorLanguageHost {
public:
    explicit FakeHost(std::string text, std::string path = workPath("main.cpp"))
        : m_path(std::move(path)), m_buffer(frost::text::TextBuffer::fromUtf8(std::move(text)).value()) {}

    // Puts the caret just after the first occurrence of `marker`.
    void caretAfter(std::string_view marker) {
        const std::string text = m_buffer.current().text();
        const std::size_t at = text.find(marker);
        ASSERT_NE(at, std::string::npos) << marker;
        m_caret = ByteOffset{at + marker.size()};
    }
    // Types `text` at the caret (one undo step per call, as an editor would).
    void type(std::string_view typed) {
        const std::string text(typed); // text::Edit views its text.
        std::vector<frost::text::Edit> edits{{frost::text::ByteRange{m_caret, m_caret}, text}};
        ASSERT_TRUE(m_buffer.applyEdits(edits).has_value());
        m_caret = ByteOffset{m_caret.value() + text.size()};
        ++m_version;
    }
    [[nodiscard]] std::string text() const { return m_buffer.current().text(); }

    std::string documentPath() const override { return m_path; }
    std::optional<frost::text::Snapshot> snapshot() const override { return m_buffer.current(); }
    std::int64_t documentVersion() const override { return m_version; }
    std::string structureIdentity() const override { return std::to_string(identity); }
    void reopenSamePath() {
        ++identity;
        m_version = 1;
    }
    ByteOffset caretOffset() const override { return m_caret; }
    std::optional<frost::text::ByteRange> selection() const override { return selectionRange; }
    frost::ui::lang::FormattingSettings formattingOptions() const override { return formatOptions; }
    QRect caretScreenRect() const override { return QRect(200, 200, 2, 16); }
    QRect screenRectAt(ByteOffset) const override { return QRect(190, 200, 2, 16); }
    frost::Result<void> applyEdits(std::span<const fl::ByteEdit> edits, std::string_view label,
                                   std::optional<ByteOffset> caret) override {
        std::vector<frost::text::Edit> plain;
        for (const fl::ByteEdit& edit : edits) {
            plain.push_back({edit.range, edit.text});
        }
        auto applied = m_buffer.applyEdits(plain);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        ++m_version;
        ++undoSteps;
        lastLabel = std::string(label);
        if (caret) {
            m_caret = *caret;
        }
        return {};
    }
    void select(frost::text::ByteRange range) override {
        selected = range;
        m_caret = range.end;
    }
    void openLocation(std::string_view path, fl::LspPosition position) override {
        opened.emplace_back(std::string(path), position);
    }
    void setUsageHighlights(std::vector<fl::DocumentHighlight> list) override { highlights = std::move(list); }
    std::vector<std::pair<std::string, frost::text::Snapshot>> openDocuments() const override {
        return {{m_path, m_buffer.current()}};
    }
    QWidget* editorWidget() const override { return nullptr; }

    std::vector<std::pair<std::string, fl::LspPosition>> opened;
    std::vector<fl::DocumentHighlight> highlights;
    std::optional<frost::text::ByteRange> selected;
    std::optional<frost::text::ByteRange> selectionRange;
    frost::ui::lang::FormattingSettings formatOptions;
    int undoSteps = 0;
    int identity = 1;
    std::string lastLabel;

private:
    std::string m_path = workPath("main.cpp");
    frost::text::TextBuffer m_buffer;
    ByteOffset m_caret{0};
    std::int64_t m_version = 1;
};

// Records workspace edits; refuses them with Busy when `refuse` is set.
class RecordingApplier final : public fl::IWorkspaceEditApplier {
public:
    frost::Result<void> apply(const fl::WorkspaceEdit& edit, std::string_view label) override {
        if (refuse) {
            return frost::fail(frost::ErrorCode::Busy, "a.h has unsaved changes and also changed on disk; save or "
                                                       "reload it before applying this edit");
        }
        edits.push_back(edit);
        labels.emplace_back(label);
        return {};
    }
    std::vector<fl::WorkspaceEdit> edits;
    std::vector<std::string> labels;
    bool refuse = false;
};

// Models renegotiating encoding after a server restart while retaining real scripted requests.
class RestartableEncodingRequester final : public fl::ILspRequester {
public:
    explicit RestartableEncodingRequester(fl::ILspRequester& server) : m_server(server) {}
    frost::sched::Task<frost::Result<Json>> request(std::string method, Json params,
                                                    std::stop_token token = {}) override {
        return m_server.request(std::move(method), std::move(params), token);
    }
    fl::PositionEncoding encoding() const override { return current; }
    std::optional<fl::ServerCapabilities> serverCapabilities() const override { return m_server.serverCapabilities(); }
    fl::PositionEncoding current = fl::PositionEncoding::Utf8;

private:
    fl::ILspRequester& m_server;
};

TEST(LanguageControllerRefactoring, EncodingChangeWhilePreviewOpenCannotApplyOldRanges) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    server.on("textDocument/prepareRename", [](const Json&) { return rangeJson(0, 3, 6); });
    server.on("textDocument/rename", [&](const Json&) {
        return Json::object().set(
            "changes",
            Json::object().set(
                uriOf(path), Json::array({Json::object().set("range", rangeJson(0, 3, 6)).set("newText", "renamed")})));
    });
    FakeHost host("\xc3\xa9 old\n");
    host.caretAfter("o");
    frost::io::MemFs fs(frost::systemClock());
    RecordingApplier applier;
    RestartableEncodingRequester requester(server.client());
    LanguageController controller({.server = requester,
                                   .scheduler = server.scheduler(),
                                   .host = host,
                                   .applier = applier,
                                   .fs = fs,
                                   .diagnostics = {}});
    controller.rename();
    auto& input = controller.renameInput();
    ASSERT_TRUE(pumpUntil([&] { return input.isVisible(); }));
    input.previewUsagesCheckbox()->setChecked(true);
    emit input.accepted(QStringLiteral("renamed"));
    QPointer<QDialog> preview;
    ASSERT_TRUE(pumpUntil([&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QStringLiteral("refactoringPreview")) {
                preview = qobject_cast<QDialog*>(widget);
                return !preview.isNull();
            }
        }
        return false;
    }));
    const auto close = qScopeGuard([&] {
        if (preview)
            preview->reject();
    });
    requester.current = fl::PositionEncoding::Utf16;
    preview->accept();
    EXPECT_TRUE(applier.edits.empty());
    EXPECT_EQ(host.text(), "\xc3\xa9 old\n");
    EXPECT_TRUE(preview->isVisible());
}

// A controller over `server`, `host` and `applier`.
struct Rig {
    Rig(FeatureServer& server, FakeHost& host)
        : fs(frost::systemClock()), controller(frost::ui::lang::LanguageServices{
                                        .server = server.client(),
                                        .scheduler = server.scheduler(),
                                        .host = host,
                                        .applier = applier,
                                        .fs = fs,
                                        .diagnostics = [this](std::string_view) { return diagnostics; }}) {}

    frost::io::MemFs fs;
    RecordingApplier applier;
    std::vector<fl::LiveDiagnostic> diagnostics;
    LanguageController controller;
};

TEST(LanguageControllerRefactoring, PreviewAppearsBeforeSemanticCheckAndCancelStopsIt) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    server.on("textDocument/prepareRename", [](const Json&) { return rangeJson(0, 4, 7); });
    server.on("textDocument/rename", [&](const Json&) {
        return Json::object().set(
            "changes",
            Json::object().set(
                uriOf(path), Json::array({Json::object().set("range", rangeJson(0, 4, 7)).set("newText", "newName")})));
    });
    FakeHost host("int old;\n");
    host.caretAfter("int o");
    Rig rig(server, host);
    std::stop_token cancellation;
    std::function<void(frost::Result<void>)> complete;
    rig.controller.setCppRenameCheck([&](std::string selectedPath, fl::LspPosition, std::string name,
                                         fl::WorkspaceEdit required, fl::WorkspaceEdit offered, std::stop_token stop,
                                         std::function<void(frost::Result<void>)> done) {
        EXPECT_EQ(selectedPath, path);
        EXPECT_EQ(name, "newName");
        EXPECT_EQ(required, offered);
        cancellation = stop;
        complete = std::move(done);
    });
    rig.controller.rename();
    auto& input = rig.controller.renameInput();
    ASSERT_TRUE(pumpUntil([&] { return input.isVisible(); }));
    input.previewUsagesCheckbox()->setChecked(false); // C++ still gets a review while checking.
    emit input.accepted(QStringLiteral("newName"));
    QPointer<QDialog> preview;
    ASSERT_TRUE(pumpUntil([&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QStringLiteral("refactoringPreview")) {
                preview = qobject_cast<QDialog*>(widget);
                return preview && preview->isVisible() && static_cast<bool>(complete);
            }
        }
        return false;
    }));
    const auto close = qScopeGuard([&] {
        if (preview)
            preview->reject();
    });
    const auto* apply = preview->findChild<QPushButton*>(QStringLiteral("applyRefactoring"));
    ASSERT_NE(apply, nullptr);
    EXPECT_FALSE(apply->isEnabled());
    EXPECT_TRUE(rig.applier.edits.empty());
    preview->reject();
    EXPECT_TRUE(cancellation.stop_requested());
    complete(frost::fail(frost::ErrorCode::Cancelled, "cancelled"));
    EXPECT_TRUE(rig.applier.edits.empty());
    EXPECT_EQ(host.text(), "int old;\n");
}

TEST(LanguageControllerRequests, CapturingFactorySurvivesDeferredStartSuspensionAndCancellation) {
    FeatureServer server;
    FakeHost host("int value;");
    frost::io::MemFs fs(frost::systemClock());
    RecordingApplier applier;
    const std::string expected = "document snapshot held by the request factory";
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel ? "cancel suspended request" : "complete suspended request");
        struct Observations {
            std::string beforeSuspension;
            std::string afterSuspension;
            bool sawCancellation = false;
        } observed;
        std::optional<frost::Result<int>> delivered;
        int finished = 0;
        std::weak_ptr<std::string> payloadLifetime;
        frost::sched::LatestWins slot;
        frost::sched::SimExecutor executor;
        LanguageController controller(frost::ui::lang::LanguageServices{.server = server.client(),
                                                                        .scheduler = executor,
                                                                        .host = host,
                                                                        .applier = applier,
                                                                        .fs = fs,
                                                                        .diagnostics = {}});
        const auto drain = qScopeGuard([&] {
            slot.cancel();
            executor.shutdown();
            QCoreApplication::sendPostedEvents(QCoreApplication::instance(), QEvent::MetaCall);
        });
        QObject::connect(&controller, &LanguageController::requestFinished, [&] { ++finished; });
        {
            auto payload = std::make_shared<std::string>(expected);
            payloadLifetime = payload;
            auto coroutine = [payload =
                                  std::move(payload)](std::weak_ptr<std::string> lifetime,
                                                      frost::sched::IScheduler& scheduler, Observations& observations,
                                                      std::stop_token token) -> frost::sched::Task<frost::Result<int>> {
                // Frame-owned arguments guard every capture read, so a destroyed factory fails safely.
                if (lifetime.expired()) {
                    co_return frost::fail(frost::ErrorCode::Internal, "factory expired before task start");
                }
                observations.beforeSuspension = *payload;
                const bool elapsed = co_await scheduler.sleepFor(std::chrono::seconds(1), token);
                if (lifetime.expired()) {
                    co_return frost::fail(frost::ErrorCode::Internal, "factory expired while task was suspended");
                }
                observations.afterSuspension = *payload;
                observations.sawCancellation = token.stop_requested();
                if (!elapsed) {
                    co_return frost::fail(frost::ErrorCode::Cancelled, "request cancelled while suspended");
                }
                co_return static_cast<int>(payload->size());
            };
            auto factory =
                std::bind_front(std::move(coroutine), payloadLifetime, std::ref(executor), std::ref(observed));
            frost::ui::lang::LanguageControllerTestAccess::startInteger(
                controller, slot, std::move(factory),
                [&](frost::Result<int> result) { delivered.emplace(std::move(result)); });
        }
        // The caller's closure is gone. Only the production request path can retain its payload.
        EXPECT_FALSE(payloadLifetime.expired());
        EXPECT_EQ(executor.dispatches(), 0U);
        EXPECT_TRUE(observed.beforeSuspension.empty());
        ASSERT_TRUE(executor.step());
        ASSERT_EQ(observed.beforeSuspension, expected);
        EXPECT_EQ(executor.timerCount(), 1U);
        EXPECT_FALSE(payloadLifetime.expired());
        EXPECT_TRUE(observed.afterSuspension.empty());
        EXPECT_FALSE(delivered.has_value());
        EXPECT_EQ(finished, 0);
        if (cancel) {
            slot.cancel();
            EXPECT_FALSE(payloadLifetime.expired());
            EXPECT_TRUE(observed.afterSuspension.empty());
        }
        EXPECT_GT(executor.runUntilIdle(), 0U);
        EXPECT_EQ(observed.afterSuspension, expected);
        EXPECT_EQ(observed.sawCancellation, cancel);
        EXPECT_TRUE(payloadLifetime.expired());
        // Task completion is queued to Qt. The cancelled ticket must never publish its result.
        EXPECT_FALSE(delivered.has_value());
        QCoreApplication::sendPostedEvents(QCoreApplication::instance(), QEvent::MetaCall);
        if (cancel) {
            EXPECT_FALSE(delivered.has_value());
            EXPECT_EQ(finished, 0);
        } else {
            ASSERT_TRUE(delivered.has_value());
            ASSERT_TRUE(delivered->has_value());
            EXPECT_EQ(delivered->value(), static_cast<int>(expected.size()));
            EXPECT_EQ(finished, 1);
        }
    }
}

Json completionItem(const std::string& label, int kind, std::string sort, const std::string& insert = {},
                    bool snippet = false) {
    return Json::object()
        .set("label", " " + label)
        .set("filterText", label)
        .set("sortText", std::move(sort))
        .set("kind", kind)
        .set("insertTextFormat", snippet ? 2 : 1)
        .set("insertText", insert.empty() ? label : insert);
}

TEST(LanguageControllerCompletion, RestartDropsDelayedPreviousGenerationResponse) {
    FeatureServer server;
    std::atomic<int> calls{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    server.on("textDocument/completion", [&](const Json&) {
        const int call = calls.fetch_add(1);
        if (call == 0) {
            entered.store(true);
            while (!release.load())
                std::this_thread::yield();
        }
        return Json::object()
            .set("isIncomplete", false)
            .set("items", Json::array({completionItem(call == 0 ? "valueOld" : "valueNew", 6, "1")}));
    });
    FakeHost host("return va");
    host.caretAfter("return va");
    Rig rig(server, host);
    const auto unblock = qScopeGuard([&] { release.store(true); });
    rig.controller.completeCode();
    ASSERT_TRUE(pumpUntil([&] { return entered.load(); }));
    // Workbench calls editorChanged when retiring the previous managed generation.
    rig.controller.editorChanged();
    rig.controller.completeCode();
    release.store(true);
    ASSERT_TRUE(pumpUntil([&] { return calls.load() >= 2 && rig.controller.completionPopup().isVisible(); }));
    ASSERT_EQ(rig.controller.completionPopup().rowCount(), 1);
    EXPECT_TRUE(rig.controller.keyPress(Qt::Key_Return, Qt::NoModifier));
    EXPECT_EQ(host.text(), "return valueNew");
}

TEST(LanguageControllerCompletion, CtrlSpaceShowsTheLookupAndEnterCommits) {
    FeatureServer server;
    server.on("textDocument/completion", [](const Json&) {
        return Json::object()
            .set("isIncomplete", false)
            .set("items", Json::array({completionItem("value", 6, "1"),
                                       completionItem("valueOf", 3, "2", "valueOf(${1:int x})$0", true),
                                       completionItem("other", 7, "3")}));
    });
    FakeHost host("int main() {\n    int value = 1;\n    return va\n}\n");
    host.caretAfter("return va");
    Rig rig(server, host);
    rig.controller.completeCode();
    auto& popup = rig.controller.completionPopup();
    ASSERT_TRUE(pumpUntil([&] { return popup.isVisible(); }));
    EXPECT_EQ(popup.rowCount(), 2); // "other" does not match "va".
    const auto sent = server.paramsOf("textDocument/completion");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].find("position")->integerAt("line"), 2);
    EXPECT_EQ(sent[0].find("position")->integerAt("character"), 13);

    // Typing filters locally without a new request.
    host.type("l");
    rig.controller.textTyped("l");
    EXPECT_EQ(popup.rowCount(), 2);
    EXPECT_EQ(server.paramsOf("textDocument/completion").size(), 1u);

    // Down to valueOf, Enter: the snippet becomes plain text and its placeholder is selected.
    EXPECT_TRUE(rig.controller.keyPress(Qt::Key_Down, Qt::NoModifier));
    EXPECT_TRUE(rig.controller.keyPress(Qt::Key_Return, Qt::NoModifier));
    EXPECT_FALSE(popup.isVisible());
    EXPECT_EQ(host.text(), "int main() {\n    int value = 1;\n    return valueOf(int x)\n}\n");
    EXPECT_EQ(host.undoSteps, 1);
    ASSERT_TRUE(host.selected.has_value());
    EXPECT_EQ(host.text().substr(host.selected->begin.value(), host.selected->size()), "int x");
    // Tab goes to the final stop after the call; then Tab is the editor's again.
    EXPECT_TRUE(rig.controller.keyPress(Qt::Key_Tab, Qt::NoModifier));
    EXPECT_EQ(host.text().substr(host.selected->begin.value(), 2), "\n}");
    EXPECT_FALSE(rig.controller.keyPress(Qt::Key_Tab, Qt::NoModifier));
}

TEST(LanguageControllerCompletion, CallSnippetRequestsParameterHintsAfterSelectingArgument) {
    FeatureServer server;
    server.on("textDocument/completion", [](const Json&) {
        auto item = completionItem("Wake(entity)", 15, "1", "Wake(${1:entity})", true);
        item.set("command", Json::object().set("command", "editor.action.triggerParameterHints"));
        return Json::array({std::move(item)});
    });
    server.on("textDocument/signatureHelp", [](const Json&) {
        return Json::object().set("signatures", Json::array({Json::object().set("label", "Wake(entity)")}));
    });
    FakeHost host("AI.Wa");
    host.caretAfter("Wa");
    Rig rig(server, host);
    rig.controller.completeCode();
    ASSERT_TRUE(pumpUntil([&] { return rig.controller.completionPopup().isVisible(); }));
    ASSERT_TRUE(rig.controller.keyPress(Qt::Key_Return, Qt::NoModifier));
    EXPECT_EQ(host.text(), "AI.Wake(entity)");
    ASSERT_TRUE(host.selected.has_value());
    EXPECT_EQ(host.text().substr(host.selected->begin.value(), host.selected->size()), "entity");
    ASSERT_TRUE(pumpUntil([&] { return rig.controller.parameterInfoPopup().isVisible(); }));
    const auto requests = server.paramsOf("textDocument/signatureHelp");
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().find("position")->integerAt("character"), 14);
}

TEST(LanguageControllerCompletion, DotTriggersAndEscCancels) {
    FeatureServer server;
    server.on("textDocument/completion", [](const Json&) {
        return Json::array({completionItem("size", 2, "1"), completionItem("swap", 2, "2")});
    });
    FakeHost host("void f(Vec v) {\n    v\n}\n");
    host.caretAfter("    v");
    Rig rig(server, host);
    host.type(".");
    rig.controller.textTyped(".");
    auto& popup = rig.controller.completionPopup();
    ASSERT_TRUE(pumpUntil([&] { return popup.isVisible(); }));
    EXPECT_EQ(popup.rowCount(), 2);
    const auto sent = server.paramsOf("textDocument/completion");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].find("context")->stringAt("triggerCharacter"), ".");
    EXPECT_TRUE(rig.controller.keyPress(Qt::Key_Escape, Qt::NoModifier));
    EXPECT_FALSE(popup.isVisible());
    const std::string before = host.text();
    EXPECT_FALSE(rig.controller.keyPress(Qt::Key_Return, Qt::NoModifier)); // Back to the editor.
    EXPECT_EQ(host.text(), before);
}

TEST(LanguageControllerAssist, ParameterInfoOnParenAndQuickDoc) {
    FeatureServer server;
    server.on("textDocument/signatureHelp", [](const Json&) {
        return Json::object()
            .set("signatures",
                 Json::array({Json::object()
                                  .set("label", "int add(int a, int b)")
                                  .set("parameters", Json::array({Json::object().set("label", "int a"),
                                                                  Json::object().set("label", "int b")}))}))
            .set("activeParameter", 0);
    });
    server.on("textDocument/hover", [](const Json&) {
        return Json::object().set("contents",
                                  Json::object().set("kind", "markdown").set("value", "Adds **two** numbers."));
    });
    FakeHost host("int x = add\n");
    host.caretAfter("add");
    Rig rig(server, host);
    host.type("(");
    rig.controller.textTyped("(");
    auto& parameters = rig.controller.parameterInfoPopup();
    ASSERT_TRUE(pumpUntil([&] { return parameters.isVisible(); }));
    EXPECT_TRUE(parameters.html().contains(QStringLiteral("<b>int a</b>")));
    EXPECT_EQ(server.paramsOf("textDocument/signatureHelp")[0].find("context")->stringAt("triggerCharacter"), "(");
    EXPECT_TRUE(rig.controller.keyPress(Qt::Key_Escape, Qt::NoModifier));
    EXPECT_FALSE(parameters.isVisible());

    rig.controller.showQuickDocumentation();
    auto& doc = rig.controller.quickDocPopup();
    ASSERT_TRUE(pumpUntil([&] { return doc.isVisible(); }));
    EXPECT_TRUE(doc.plainText().contains(QStringLiteral("Adds two numbers.")));
}

TEST(LanguageControllerNavigation, DeclarationRelatedFilesUsagesAndHighlights) {
    FeatureServer server;
    const std::string header = workPath("tiny.h");
    server.on("textDocument/definition", [&](const Json&) {
        return Json::array({Json::object().set("uri", uriOf(header)).set("range", rangeJson(4, 4, 13))});
    });
    server.on("textDocument/switchSourceHeader", [&](const Json&) { return Json(uriOf(header)); });
    server.on("textDocument/references", [&](const Json&) {
        return Json::array({Json::object().set("uri", uriOf(workPath("main.cpp"))).set("range", rangeJson(1, 12, 21)),
                            Json::object().set("uri", uriOf(header)).set("range", rangeJson(4, 4, 13))});
    });
    server.on("textDocument/documentHighlight", [](const Json&) {
        return Json::array({Json::object().set("range", rangeJson(1, 12, 21)).set("kind", 2)});
    });
    FakeHost host("int main() {\n    int v = tinyValue();\n}\n");
    host.caretAfter("tinyV");
    Rig rig(server, host);
    std::vector<QString> status;
    QObject::connect(&rig.controller, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });

    rig.controller.gotoDeclaration();
    ASSERT_TRUE(pumpUntil([&] { return host.opened.size() == 1; }));
    EXPECT_EQ(fl::documentKey(host.opened[0].first), fl::documentKey(header));
    EXPECT_EQ(host.opened[0].second, (fl::LspPosition{4, 4}));

    rig.controller.gotoRelatedFile();
    ASSERT_TRUE(pumpUntil([&] { return host.opened.size() == 2; }));
    EXPECT_EQ(fl::documentKey(host.opened[1].first), fl::documentKey(header));

    // tiny.h is not open and not on the (memory) disk: its usage has no preview but is listed.
    int usagesReady = 0;
    QObject::connect(&rig.controller, &LanguageController::usagesReady, [&] { ++usagesReady; });
    rig.controller.findUsages();
    ASSERT_TRUE(pumpUntil([&] { return usagesReady == 1; }));
    auto& panel = rig.controller.usagesPanel();
    EXPECT_EQ(panel.usageCount(), 2u);
    EXPECT_EQ(panel.title(), QStringLiteral("Usages of 'tinyValue': 2 usages"));
    panel.activate(0, 0);
    ASSERT_EQ(host.opened.size(), 3u);

    rig.controller.setHighlightDelay(std::chrono::milliseconds(5));
    rig.controller.caretMoved();
    ASSERT_TRUE(pumpUntil([&] { return !host.highlights.empty(); }));
    EXPECT_EQ(host.highlights[0].kind, fl::HighlightKind::Read);
    EXPECT_TRUE(status.empty());
}

TEST(LanguageControllerNavigation, FileStructurePopupOpensTheSymbol) {
    FeatureServer server;
    server.on("textDocument/documentSymbol", [](const Json&) {
        return Json::array({Json::object()
                                .set("name", "main")
                                .set("kind", 12)
                                .set("range", rangeJson(0, 0, 20))
                                .set("selectionRange", rangeJson(0, 4, 8))});
    });
    FakeHost host("int main() {}\n");
    Rig rig(server, host);
    rig.controller.showFileStructure();
    auto& popup = rig.controller.symbolPopup();
    ASSERT_TRUE(pumpUntil([&] { return popup.isVisible(); }));
    EXPECT_EQ(popup.visibleRowCount(), 1);
    popup.activateCurrent();
    ASSERT_EQ(host.opened.size(), 1u);
    EXPECT_EQ(host.opened[0].second, (fl::LspPosition{0, 4}));
}

TEST(LanguageControllerNavigation, StructureRejectsOldRepliesAndRefreshesAfterEditAndReopen) {
    FeatureServer server;
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<int> requests = 0;
    server.on("textDocument/documentSymbol", [&](const Json&) {
        const int n = ++requests;
        if (n == 1)
            gate.wait_for(std::chrono::seconds(5));
        return Json::array({Json::object()
                                .set("name", n == 1 ? "old" : "current")
                                .set("kind", 12)
                                .set("range", rangeJson(0, 0, 20))
                                .set("selectionRange", rangeJson(0, 4, 8))});
    });
    bool released = false;
    const auto unblock = qScopeGuard([&] {
        if (!released)
            release.set_value();
    });
    FakeHost host("int main() {}\n");
    Rig rig(server, host);
    auto* tree = rig.controller.structurePanel().findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    ASSERT_NE(tree, nullptr);
    rig.controller.setStructureVisible(true);
    ASSERT_TRUE(pumpUntil([&] { return requests.load() == 1; }));
    host.type("x");
    rig.controller.structureChanged();
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    release.set_value();
    released = true;
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1; }));
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("current"));
    emit tree->itemActivated(tree->topLevelItem(0), 0);
    ASSERT_EQ(host.opened.size(), 1u);
    EXPECT_EQ(host.opened.front().second, (fl::LspPosition{0, 4}));
    ++host.identity; // Same path and version, different document/server generation.
    emit tree->itemActivated(tree->topLevelItem(0), 0);
    EXPECT_EQ(host.opened.size(), 1u);
    rig.controller.editorChanged();
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    rig.controller.setStructureVisible(false);
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    rig.controller.setStructureVisible(true);
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1; }));
}

TEST(LanguageControllerNavigation, StructureFilterAndPopupUseIndependentRequests) {
    FeatureServer server;
    server.on("textDocument/documentSymbol", [](const Json&) {
        return Json::array({Json::object()
                                .set("name", "Widget")
                                .set("kind", 5)
                                .set("range", rangeJson(0, 0, 20))
                                .set("selectionRange", rangeJson(0, 0, 6))
                                .set("children", Json::array({Json::object()
                                                                  .set("name", "draw")
                                                                  .set("kind", 6)
                                                                  .set("range", rangeJson(1, 0, 10))
                                                                  .set("selectionRange", rangeJson(1, 2, 6))}))});
    });
    FakeHost host("class Widget {};\n");
    Rig rig(server, host);
    auto& panel = rig.controller.structurePanel();
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("structureSearch"));
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(search, nullptr);
    rig.controller.setStructureVisible(true);
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1; }));
    search->setText(QStringLiteral("draw"));
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    ASSERT_EQ(tree->topLevelItem(0)->childCount(), 1);
    EXPECT_EQ(tree->topLevelItem(0)->child(0)->text(0), QStringLiteral("draw"));
    auto* shortcut = panel.findChild<QShortcut*>();
    ASSERT_NE(shortcut, nullptr);
    EXPECT_EQ(shortcut->key(), QKeySequence(Qt::Key_F4));
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    emit shortcut->activated();
    ASSERT_EQ(host.opened.size(), 1u);
    EXPECT_EQ(host.opened[0].second, (fl::LspPosition{1, 2}));
    rig.controller.showFileStructure();
    ASSERT_TRUE(pumpUntil([&] { return rig.controller.symbolPopup().isVisible(); }));
    EXPECT_EQ(tree->topLevelItemCount(), 1);
    search->setText(QStringLiteral("missing"));
    EXPECT_EQ(tree->topLevelItemCount(), 0);
}

TEST(LanguageControllerNavigation, StructureDropsReplyFromClosedAndReopenedDocument) {
    FeatureServer server;
    std::atomic<int> requests = 0;
    std::atomic<bool> release = false;
    server.on("textDocument/documentSymbol", [&](const Json&) {
        const int request = ++requests;
        if (request == 1) {
            while (!release.load())
                std::this_thread::yield();
        }
        return Json::array({Json::object()
                                .set("name", request == 1 ? "old instance" : "new instance")
                                .set("kind", 12)
                                .set("range", rangeJson(0, 0, 12))
                                .set("selectionRange", rangeJson(0, 0, 4))});
    });
    const auto unblock = qScopeGuard([&] { release.store(true); });
    FakeHost host("int main() {}\n");
    Rig rig(server, host);
    auto* tree = rig.controller.structurePanel().findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    ASSERT_NE(tree, nullptr);
    rig.controller.setStructureVisible(true);
    ASSERT_TRUE(pumpUntil([&] { return requests.load() == 1; }));
    host.reopenSamePath();
    rig.controller.editorChanged();
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    release.store(true);
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1; }));
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("new instance"));
}

TEST(LanguageControllerNavigation, PopupDoesNotCancelPendingStructureRequest) {
    FeatureServer server;
    std::atomic<int> requests = 0;
    std::atomic<bool> release = false;
    server.on("textDocument/documentSymbol", [&](const Json&) {
        const int request = ++requests;
        if (request == 1) {
            while (!release.load())
                std::this_thread::yield();
        }
        return Json::array({Json::object()
                                .set("name", "main")
                                .set("kind", 12)
                                .set("range", rangeJson(0, 0, 12))
                                .set("selectionRange", rangeJson(0, 0, 4))});
    });
    const auto unblock = qScopeGuard([&] { release.store(true); });
    FakeHost host("int main() {}\n");
    Rig rig(server, host);
    auto* tree = rig.controller.structurePanel().findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    ASSERT_NE(tree, nullptr);
    rig.controller.setStructureVisible(true);
    ASSERT_TRUE(pumpUntil([&] { return requests.load() == 1; }));
    rig.controller.showFileStructure();
    release.store(true);
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1 && rig.controller.symbolPopup().isVisible(); }));
    EXPECT_GE(requests.load(), 2);
}

TEST(LanguageControllerRefactoring, RenameAndQuickFixGoThroughTheApplier) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    server.on("textDocument/prepareRename", [](const Json&) { return rangeJson(0, 4, 9); });
    server.on("textDocument/rename", [&](const Json& params) {
        return Json::object().set(
            "changes",
            Json::object().set(
                uriOf(path),
                Json::array(
                    {Json::object().set("range", rangeJson(0, 4, 9)).set("newText", params.stringAt("newName"))})));
    });
    server.on("textDocument/codeAction", [&](const Json&) {
        return Json::array(
            {Json::object()
                 .set("title", "Insert ';'")
                 .set("kind", "quickfix")
                 .set("isPreferred", true)
                 .set("edit",
                      Json::object().set(
                          "changes",
                          Json::object().set(
                              uriOf(path),
                              Json::array({Json::object().set("range", rangeJson(0, 10, 10)).set("newText", ";")}))))});
    });
    FakeHost host("int value\n");
    host.caretAfter("int va");
    Rig rig(server, host);
    std::vector<QString> status;
    QObject::connect(&rig.controller, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });

    rig.controller.rename();
    auto& input = rig.controller.renameInput();
    ASSERT_TRUE(pumpUntil([&] { return input.isVisible(); }));
    EXPECT_EQ(input.field()->text(), QStringLiteral("value")); // From the prepared range.
    input.field()->setText(QStringLiteral("amount"));
    emit input.accepted(QStringLiteral("amount"));
    ASSERT_TRUE(pumpUntil([&] { return rig.applier.edits.size() == 1; }));
    EXPECT_EQ(rig.applier.labels[0], "Rename");
    EXPECT_EQ(rig.applier.edits[0].files.at(0).edits.at(0).newText, "amount");

    fl::LiveDiagnostic missingSemi;
    missingSemi.message = "expected ';' after top level declarator";
    missingSemi.lspRange = fl::LspRange{{0, 9}, {0, 9}};
    rig.diagnostics = {missingSemi};
    rig.controller.showIntentions();
    auto& choice = rig.controller.choicePopup();
    ASSERT_TRUE(pumpUntil([&] { return choice.isVisible(); }));
    ASSERT_GE(choice.list()->count(), 10);
    EXPECT_EQ(choice.list()->item(0)->text(), QStringLiteral("Insert ';'"));
    EXPECT_EQ(choice.list()->item(1)->text(), QStringLiteral("Go to Declaration or Usages"));
    const auto sentActions = server.paramsOf("textDocument/codeAction");
    ASSERT_EQ(sentActions.size(), 1u);
    const Json* sentDiagnostics = sentActions[0].find("context")->find("diagnostics");
    ASSERT_EQ(sentDiagnostics->size(), 1u); // The line's diagnostic, though the caret is elsewhere.
    rig.applier.refuse = true;
    emit choice.chosen(0);
    ASSERT_EQ(status.size(), 1u);
    EXPECT_TRUE(status[0].contains(QStringLiteral("save or reload")));
}

TEST(LanguageControllerRefactoring, ShowIntentionsOpensRiderContextMenuAndNavigatesWithoutQuickFixes) {
    FeatureServer server;
    const std::string target = workPath("decl.hpp");
    server.on("textDocument/codeAction", [](const Json&) { return Json::array(); });
    server.on("textDocument/definition", [&](const Json&) {
        return Json::array({Json::object().set("uri", uriOf(target)).set("range", rangeJson(4, 2, 8))});
    });
    FakeHost host("int value = 1;\n");
    host.caretAfter("val");
    Rig rig(server, host);

    rig.controller.showIntentions();
    auto& choice = rig.controller.choicePopup();
    ASSERT_TRUE(pumpUntil([&] { return choice.isVisible(); }));
    ASSERT_EQ(choice.list()->count(), 9);
    EXPECT_EQ(choice.list()->item(0)->text(), QStringLiteral("Go to Declaration or Usages"));
    EXPECT_EQ(choice.list()->item(1)->text(), QStringLiteral("Go to Implementation"));
    EXPECT_EQ(choice.list()->item(2)->text(), QStringLiteral("Find Usages"));
    EXPECT_EQ(choice.list()->item(5)->text(), QStringLiteral("Rename..."));
    emit choice.chosen(0);
    ASSERT_TRUE(pumpUntil([&] { return host.opened.size() == 1; }));
    EXPECT_EQ(fl::documentKey(host.opened[0].first), fl::documentKey(target));
    EXPECT_EQ(host.opened[0].second, (fl::LspPosition{4, 2}));
}

TEST(LanguageControllerRefactoring, ReformatAppliesTheServerEditsAsOneStep) {
    FeatureServer server;
    server.on("textDocument/formatting", [](const Json&) {
        return Json::array({Json::object().set("range", rangeJson(0, 3, 6)).set("newText", " ")});
    });
    FakeHost host("int   x = 1;\n");
    host.formatOptions = {.tabSize = 8, .indentSize = 2, .insertSpaces = true};
    Rig rig(server, host);
    rig.controller.reformat();
    ASSERT_TRUE(pumpUntil([&] { return host.undoSteps == 1; }));
    EXPECT_EQ(host.text(), "int x = 1;\n");
    EXPECT_EQ(host.lastLabel, "Reformat Code");
    const auto sent = server.paramsOf("textDocument/formatting");
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].find("options")->integerAt("tabSize"), 2);
    EXPECT_EQ(sent[0].find("options")->boolAt("insertSpaces"), true);
}

TEST(LanguageControllerRefactoring, RangeFormattingUsesEffectiveFileOptions) {
    FeatureServer server;
    server.on("textDocument/rangeFormatting", [](const Json&) {
        return Json::array({Json::object().set("range", rangeJson(0, 3, 6)).set("newText", " ")});
    });
    FakeHost host("int   x = 1;\n");
    host.selectionRange = frost::text::ByteRange{ByteOffset{0}, ByteOffset{9}};
    host.formatOptions = {.tabSize = 8, .indentSize = 2, .insertSpaces = false};
    Rig rig(server, host);
    rig.controller.reformat();
    ASSERT_TRUE(pumpUntil([&] { return host.undoSteps == 1; }));
    const auto sent = server.paramsOf("textDocument/rangeFormatting");
    ASSERT_EQ(sent.size(), 1U);
    ASSERT_NE(sent[0].find("options"), nullptr);
    EXPECT_EQ(sent[0].find("options")->integerAt("tabSize"), 8);
    EXPECT_EQ(sent[0].find("options")->boolAt("insertSpaces"), false);
    ASSERT_NE(sent[0].find("range"), nullptr);
    EXPECT_EQ(sent[0].find("range")->find("start")->integerAt("character"), 0);
    EXPECT_EQ(sent[0].find("range")->find("end")->integerAt("character"), 9);
    EXPECT_EQ(host.text(), "int x = 1;\n");
}

TEST(LanguageControllerRefactoring, FormattingResponseDoesNotOverwriteNewTyping) {
    FeatureServer server;
    std::promise<void> release;
    auto gate = release.get_future().share();
    server.on("textDocument/formatting", [gate](const Json&) {
        gate.wait_for(std::chrono::seconds(5));
        return Json::array({Json::object().set("range", rangeJson(0, 3, 6)).set("newText", " ")});
    });
    FakeHost host("int   x = 1;\n");
    host.caretAfter("x");
    Rig rig(server, host);
    rig.controller.reformat();
    ASSERT_TRUE(pumpUntil([&] { return !server.paramsOf("textDocument/formatting").empty(); }));
    host.type("y");
    release.set_value();
    bool finished = false;
    QObject::connect(&rig.controller, &frost::ui::lang::LanguageController::requestFinished, [&] { finished = true; });
    ASSERT_TRUE(pumpUntil([&] { return finished; }));
    EXPECT_EQ(host.text(), "int   xy = 1;\n");
    EXPECT_EQ(host.undoSteps, 0);
}

TEST(LanguageControllerRefactoring, FormattingResponseDoesNotApplyAfterIndentSettingsChange) {
    FeatureServer server;
    std::promise<void> release;
    auto gate = release.get_future().share();
    server.on("textDocument/formatting", [gate](const Json&) {
        gate.wait_for(std::chrono::seconds(5));
        return Json::array({Json::object().set("range", rangeJson(0, 3, 6)).set("newText", " ")});
    });
    FakeHost host("int   x = 1;\n");
    Rig rig(server, host);
    rig.controller.reformat();
    ASSERT_TRUE(pumpUntil([&] { return !server.paramsOf("textDocument/formatting").empty(); }));
    host.formatOptions.insertSpaces = false;
    release.set_value();
    bool finished = false;
    QObject::connect(&rig.controller, &frost::ui::lang::LanguageController::requestFinished, [&] { finished = true; });
    ASSERT_TRUE(pumpUntil([&] { return finished; }));
    EXPECT_EQ(host.text(), "int   x = 1;\n");
    EXPECT_EQ(host.undoSteps, 0);
}

TEST(LanguageControllerRefactoring, FormattingResponseDoesNotApplyAfterIndentWidthOnlyChange) {
    FeatureServer server;
    std::promise<void> release;
    auto gate = release.get_future().share();
    server.on("textDocument/formatting", [gate](const Json&) {
        gate.wait_for(std::chrono::seconds(5));
        return Json::array({Json::object().set("range", rangeJson(0, 3, 6)).set("newText", " ")});
    });
    FakeHost host("int   x = 1;\n");
    Rig rig(server, host);
    rig.controller.reformat();
    ASSERT_TRUE(pumpUntil([&] { return !server.paramsOf("textDocument/formatting").empty(); }));
    host.formatOptions.indentSize = 2;
    release.set_value();
    bool finished = false;
    QObject::connect(&rig.controller, &frost::ui::lang::LanguageController::requestFinished, [&] { finished = true; });
    ASSERT_TRUE(pumpUntil([&] { return finished; }));
    EXPECT_EQ(host.text(), "int   x = 1;\n");
    EXPECT_EQ(host.undoSteps, 0);
}

TEST(LanguageControllerActions, RegistersTheIntelliJIdsAndRunsThem) {
    FeatureServer server;
    const std::string header = workPath("main.h");
    server.on("textDocument/switchSourceHeader", [&](const Json&) { return Json(uriOf(header)); });
    FakeHost host("int main() {}\n");
    Rig rig(server, host);
    frost::input::ActionRegistry registry;
    ASSERT_TRUE(rig.controller.registerActions(registry).has_value());
    for (const char* id : {"code.completion", "code.parameterInfo", "code.quickDocumentation", "navigate.declaration",
                           "navigate.implementation", "navigate.relatedFiles", "navigate.findUsages",
                           "navigate.highlightUsages", "navigate.fileStructure", "navigate.gotoSymbol",
                           "navigate.gotoClass", "refactor.rename", "code.showIntentions", "code.reformat"}) {
        EXPECT_TRUE(registry.find(id).has_value()) << id;
    }
    // Registering twice is refused (another slice owning an id gets the same answer).
    EXPECT_FALSE(rig.controller.registerActions(registry).has_value());

    const auto related = *registry.find("navigate.relatedFiles");
    frost::input::ActionContext outside;
    EXPECT_FALSE(registry.isEnabled(related, outside)); // Needs EditorFocus.
    frost::input::ActionContext inEditor;
    inEditor.keys.set(frost::input::ContextKey::EditorFocus);
    EXPECT_EQ(registry.tryInvoke(related, inEditor), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(pumpUntil([&] { return host.opened.size() == 1; }));
    EXPECT_EQ(fl::documentKey(host.opened[0].first), fl::documentKey(header));
    EXPECT_TRUE(registry.isEnabled(*registry.find("navigate.gotoSymbol"), outside));
    EXPECT_TRUE(registry.isEnabled(*registry.find("navigate.gotoClass"), outside));
}

TEST(LanguageControllerNavigation, GotoClassFiltersToClassLikeSymbolsAndGotoSymbolKeepsAll) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    server.on("workspace/symbol", [&](const Json&) {
        return Json::array({
            Json::object()
                .set("name", "PlayerController")
                .set("kind", 5) // Class
                .set("location", Json::object().set("uri", uriOf(path)).set("range", rangeJson(2, 0, 16))),
            Json::object()
                .set("name", "PlayerState")
                .set("kind", 23) // Struct
                .set("location", Json::object().set("uri", uriOf(path)).set("range", rangeJson(6, 0, 11))),
            Json::object()
                .set("name", "playSound")
                .set("kind", 12) // Function
                .set("location", Json::object().set("uri", uriOf(path)).set("range", rangeJson(10, 0, 9))),
        });
    });
    FakeHost host("class PlayerController {};\n");
    Rig rig(server, host);

    rig.controller.gotoClass();
    auto& popup = rig.controller.symbolPopup();
    ASSERT_TRUE(pumpUntil([&] { return popup.isVisible(); }));
    popup.setQuery(QStringLiteral("Play"));
    ASSERT_TRUE(pumpUntil([&] { return popup.visibleRowCount() == 2; }));
    popup.activateCurrent();
    ASSERT_EQ(host.opened.size(), 1u);
    EXPECT_EQ(host.opened[0].second, (fl::LspPosition{6, 0})); // Shorter match "PlayerState" ranks first.

    rig.controller.gotoSymbol();
    ASSERT_TRUE(pumpUntil([&] { return popup.isVisible(); }));
    popup.setQuery(QStringLiteral("Play"));
    ASSERT_TRUE(pumpUntil([&] { return popup.visibleRowCount() == 3; }));
}

TEST(LanguageControllerNavigation, RelatedFilesFallsBackToProjectFilesWhenServerReturnsEmpty) {
    FeatureServer server;
    server.on("textDocument/switchSourceHeader", [](const Json&) { return Json(""); });
    FakeHost host("int main() {}\n");
    const std::string matchingHeader = workPath("include/main.hpp");
    frost::io::MemFs fs(frost::systemClock());
    RecordingApplier applier;
    LanguageController controller(frost::ui::lang::LanguageServices{
        .server = server.client(),
        .scheduler = server.scheduler(),
        .host = host,
        .applier = applier,
        .fs = fs,
        .diagnostics = [](std::string_view) { return std::vector<fl::LiveDiagnostic>{}; },
        .projectFiles =
            [matchingHeader] { return std::vector<std::string>{workPath("src/other.hpp"), matchingHeader}; },
    });
    controller.gotoRelatedFile();
    ASSERT_TRUE(pumpUntil([&] { return host.opened.size() == 1; }));
    EXPECT_EQ(fl::documentKey(host.opened[0].first), fl::documentKey(matchingHeader));
}

TEST(LanguageControllerNavigation, StructurePreservesCollapsedHierarchyOnJumpAndUnchangedStructure) {
    FeatureServer server;
    std::atomic<int> requests = 0;
    server.on("textDocument/documentSymbol", [&](const Json&) {
        ++requests;
        return Json::array({Json::object()
                                .set("name", "BigClass")
                                .set("kind", 5)
                                .set("range", rangeJson(0, 0, 20))
                                .set("selectionRange", rangeJson(0, 0, 8))
                                .set("children", Json::array({Json::object()
                                                                  .set("name", "memberFn")
                                                                  .set("kind", 6)
                                                                  .set("range", rangeJson(1, 0, 10))
                                                                  .set("selectionRange", rangeJson(1, 2, 10))}))});
    });
    FakeHost host("class BigClass {\n  void memberFn() {}\n};\n");
    Rig rig(server, host);
    auto* tree = rig.controller.structurePanel().findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    ASSERT_NE(tree, nullptr);
    rig.controller.setStructureVisible(true);
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1; }));
    ASSERT_EQ(requests.load(), 1);

    // User collapses BigClass in the Structure panel.
    QTreeWidgetItem* rootItem = tree->topLevelItem(0);
    rootItem->setExpanded(false);
    ASSERT_FALSE(rootItem->isExpanded());

    // Jumping to a signature in the same unchanged document fires editorChanged() and highlightLine(),
    // which must not re-request symbols or force-expand the collapsed node.
    rig.controller.editorChanged();
    rig.controller.structurePanel().highlightLine(1);
    EXPECT_EQ(requests.load(), 1);
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    EXPECT_FALSE(tree->topLevelItem(0)->isExpanded());

    // Editing the document triggers structureChanged(), and when the server returns the same symbol
    // hierarchy, the collapsed state and tree items are preserved in-place.
    host.type("\n");
    rig.controller.structureChanged();
    ASSERT_TRUE(pumpUntil([&] { return requests.load() == 2; }));
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    EXPECT_FALSE(tree->topLevelItem(0)->isExpanded());
}

TEST(LanguageControllerNavigation, StructureKeepsTheTreeWhileTypingButClearsItWhenTheTextIsReplaced) {
    FeatureServer server;
    std::atomic<int> requests = 0;
    server.on("textDocument/documentSymbol", [&](const Json&) {
        ++requests;
        return Json::array({Json::object()
                                .set("name", "Holder")
                                .set("kind", 5)
                                .set("range", rangeJson(0, 0, 12))
                                .set("selectionRange", rangeJson(0, 6, 12))});
    });
    FakeHost host("class Holder {};\n");
    Rig rig(server, host);
    auto* tree = rig.controller.structurePanel().findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    ASSERT_NE(tree, nullptr);
    rig.controller.setStructureVisible(true);
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1; }));

    // An edit keeps the (inert) tree until the reply arrives.
    host.type("\n");
    rig.controller.structureChanged();
    EXPECT_EQ(tree->topLevelItemCount(), 1);
    ASSERT_TRUE(pumpUntil([&] { return requests.load() == 2; }));
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1; }));

    // A replacement of the whole text (a reload from disk) clears it at once, then the reply refills it.
    host.type("\n");
    rig.controller.structureReplaced();
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    ASSERT_TRUE(pumpUntil([&] { return requests.load() == 3 && tree->topLevelItemCount() == 1; }));
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("Holder"));
}

// The metadata cache of the tests: a profile directory, never the workspace (workPath).
std::string metadataCache() {
#ifdef _WIN32
    return "C:\\profile\\caches\\metadata";
#else
    return "/profile/caches/metadata";
#endif
}

// FND-3 (LTR-1, D32 replaces the old synthesis test): a C# server's real metadata source is shown
// from the metadata cache, byte for byte, and nothing is written into the workspace.
TEST(LanguageControllerNavigation, GotoDeclarationShowsServerMetadataFromTheCacheOnly) {
    FeatureServer server;
    constexpr std::string_view kSource = "namespace System\n{\n    public static class Console\n    {\n        "
                                         "public static void WriteLine(string? value);\n    }\n}\n";
    server.on("textDocument/declaration", [](const Json&) { return Json::array(); });
    server.on("textDocument/definition", [](const Json&) {
        return Json::array(
            {Json::object()
                 .set("uri", "csharp:/metadata/projects/App/assemblies/System.Console/symbols/Console.cs")
                 .set("range", rangeJson(4, 27, 36))});
    });
    server.on("csharp/metadata", [&](const Json&) { return Json::object().set("source", std::string(kSource)); });
    FakeHost host("using System;\nclass Program {\n    static void Main() {\n        "
                  "System.Console.WriteLine(\"hi\");\n    }\n}\n",
                  workPath("Program.cs"));
    host.caretAfter("System.Console.Write");
    frost::io::MemFs fs(frost::systemClock());
    RecordingApplier applier;
    LanguageController controller(frost::ui::lang::LanguageServices{
        .server = server.client(),
        .scheduler = server.scheduler(),
        .host = host,
        .applier = applier,
        .fs = fs,
        .diagnostics = [](std::string_view) { return std::vector<fl::LiveDiagnostic>{}; },
        .metadataCacheDir = metadataCache(),
    });
    controller.gotoDeclaration();
    ASSERT_TRUE(pumpUntil([&] { return host.opened.size() == 1; }));
    const std::string& shown = host.opened[0].first;
    EXPECT_TRUE(shown.starts_with(metadataCache())) << shown;
    EXPECT_EQ(host.opened[0].second, (fl::LspPosition{4, 27})); // The server's range.
    EXPECT_EQ(fs.readFile(shown).value_or(""), kSource);        // Exactly what the server sent.
    EXPECT_FALSE(fs.exists(workDir()).value_or(true));          // Nothing under the workspace.

    // Without a cache directory the source is not shown, and nothing is written anywhere.
    FakeHost noCacheHost(host.text(), workPath("Program.cs"));
    noCacheHost.caretAfter("System.Console.Write");
    frost::io::MemFs emptyFs(frost::systemClock());
    LanguageController noCache(frost::ui::lang::LanguageServices{
        .server = server.client(),
        .scheduler = server.scheduler(),
        .host = noCacheHost,
        .applier = applier,
        .fs = emptyFs,
        .diagnostics = [](std::string_view) { return std::vector<fl::LiveDiagnostic>{}; },
    });
    std::vector<QString> status;
    QObject::connect(&noCache, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });
    noCache.decompileExternalSymbol();
    EXPECT_TRUE(noCacheHost.opened.empty());
    ASSERT_EQ(status.size(), 1U);
    EXPECT_TRUE(status[0].contains(QStringLiteral("Source not available"))) << status[0].toStdString();
    EXPECT_TRUE(status[0].contains(QStringLiteral("LTR-6")));
}

// R0 follow-up 4: when the C# server answers Go to Declaration with a metadata URI and no cache is
// configured, the message names LTR-6 and the missing cache instead of "returned no result", and
// csharp/metadata is not asked (nothing could be shown).
TEST(LanguageControllerNavigation, MetadataWithoutACacheNamesLtr6) {
    FeatureServer server;
    server.on("textDocument/declaration", [](const Json&) { return Json::array(); });
    server.on("textDocument/definition", [](const Json&) {
        return Json::array(
            {Json::object()
                 .set("uri", "csharp:/metadata/projects/App/assemblies/System.Console/symbols/Console.cs")
                 .set("range", rangeJson(4, 27, 36))});
    });
    server.on("csharp/metadata", [](const Json&) { return Json::object().set("source", "namespace System {}\n"); });
    FakeHost host("class Program {\n    static void Main() {\n        System.Console.WriteLine(\"hi\");\n    }\n}\n",
                  workPath("Program.cs"));
    host.caretAfter("System.Console.Write");
    frost::io::MemFs fs(frost::systemClock());
    RecordingApplier applier;
    LanguageController controller(frost::ui::lang::LanguageServices{
        .server = server.client(),
        .scheduler = server.scheduler(),
        .host = host,
        .applier = applier,
        .fs = fs,
        .diagnostics = [](std::string_view) { return std::vector<fl::LiveDiagnostic>{}; },
    });
    std::vector<QString> status;
    QObject::connect(&controller, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });
    controller.gotoDeclaration();
    ASSERT_TRUE(pumpUntil([&] { return !status.empty(); }));
    EXPECT_TRUE(host.opened.empty());
    EXPECT_TRUE(status[0].contains(QStringLiteral("LTR-6"))) << status[0].toStdString();
    EXPECT_TRUE(status[0].contains(QStringLiteral("no metadata cache"))) << status[0].toStdString();
    EXPECT_FALSE(status[0].contains(QStringLiteral("returned no result"))) << status[0].toStdString();
    EXPECT_TRUE(server.paramsOf("csharp/metadata").empty());
    EXPECT_FALSE(fs.exists(workDir()).value_or(true));

    // A server that offers no metadata keeps the honest empty-result message.
    FeatureServer plain;
    plain.on("textDocument/declaration", [](const Json&) { return Json::array(); });
    plain.on("textDocument/definition", [](const Json&) { return Json::array(); });
    FakeHost plainHost(host.text(), workPath("Program.cs"));
    plainHost.caretAfter("System.Console.Write");
    LanguageController plainController(frost::ui::lang::LanguageServices{
        .server = plain.client(),
        .scheduler = plain.scheduler(),
        .host = plainHost,
        .applier = applier,
        .fs = fs,
        .diagnostics = [](std::string_view) { return std::vector<fl::LiveDiagnostic>{}; },
    });
    std::vector<QString> plainStatus;
    QObject::connect(&plainController, &LanguageController::statusMessage,
                     [&](const QString& message) { plainStatus.push_back(message); });
    plainController.gotoDeclaration();
    ASSERT_TRUE(pumpUntil([&] { return !plainStatus.empty(); }));
    EXPECT_TRUE(plainStatus[0].contains(QStringLiteral("Cannot find a declaration"))) << plainStatus[0].toStdString();
    EXPECT_FALSE(plainStatus[0].contains(QStringLiteral("LTR-6"))) << plainStatus[0].toStdString();
}

// FND-3 (D32): a C++ symbol clangd cannot resolve is not "decompiled" from hover text: nothing is
// written or opened, and the message says why the result is empty.
TEST(LanguageControllerNavigation, EmptyCppDeclarationWritesNothingAndExplainsWhy) {
    FeatureServer server;
    server.on("textDocument/definition", [](const Json&) { return Json::array(); });
    server.on("textDocument/declaration", [](const Json&) { return Json::array(); });
    server.on("textDocument/hover", [](const Json&) {
        return Json::object().set(
            "contents",
            Json::object().set("kind", "markdown").set("value", "```cpp\ntemplate <class T> class vector\n```"));
    });
    FakeHost host("#include <vector>\nint main() {\n    std::vector<int> items;\n}\n", workPath("main.cpp"));
    host.caretAfter("std::vec");
    Rig rig(server, host);
    std::vector<QString> status;
    QObject::connect(&rig.controller, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });

    rig.controller.gotoDeclaration();
    ASSERT_TRUE(pumpUntil([&] { return !status.empty(); }));
    EXPECT_TRUE(host.opened.empty());
    EXPECT_FALSE(rig.fs.exists(workDir()).value_or(true));
    EXPECT_TRUE(server.paramsOf("textDocument/hover").empty()); // No hover-text synthesis.
    EXPECT_TRUE(status[0].contains(QStringLiteral("Cannot find a declaration"))) << status[0].toStdString();
    EXPECT_TRUE(status[0].contains(QStringLiteral("clangd returned no result"))) << status[0].toStdString();

    rig.controller.decompileExternalSymbol();
    ASSERT_TRUE(pumpUntil([&] { return status.size() == 2; }));
    EXPECT_TRUE(status[1].contains(QStringLiteral("Source not available"))) << status[1].toStdString();
    EXPECT_TRUE(host.opened.empty());
    EXPECT_FALSE(rig.fs.exists(workDir()).value_or(true));
}

// FND-3: the reason for an empty answer comes from the readiness hook (no compile command,
// indexing) when the workbench provides one.
TEST(LanguageControllerNavigation, EmptyImplementationNamesTheServerReadiness) {
    FeatureServer server;
    server.on("textDocument/implementation", [](const Json&) { return Json::array(); });
    server.on("textDocument/definition", [](const Json&) { return Json::array(); });
    FakeHost host("int compute();\n", workPath("main.cpp"));
    host.caretAfter("comp");
    frost::io::MemFs fs(frost::systemClock());
    RecordingApplier applier;
    frost::ui::lang::ServerReadiness readiness = frost::ui::lang::ServerReadiness::NoCompileCommand;
    LanguageController controller(frost::ui::lang::LanguageServices{
        .server = server.client(),
        .scheduler = server.scheduler(),
        .host = host,
        .applier = applier,
        .fs = fs,
        .diagnostics = [](std::string_view) { return std::vector<fl::LiveDiagnostic>{}; },
        .readiness = [&](std::string_view) { return readiness; },
    });
    std::vector<QString> status;
    QObject::connect(&controller, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });
    controller.gotoImplementation();
    ASSERT_TRUE(pumpUntil([&] { return status.size() == 1; }));
    EXPECT_TRUE(status[0].contains(QStringLiteral("no compile command"))) << status[0].toStdString();
    readiness = frost::ui::lang::ServerReadiness::Indexing;
    controller.gotoImplementation();
    ASSERT_TRUE(pumpUntil([&] { return status.size() == 2; }));
    EXPECT_TRUE(status[1].contains(QStringLiteral("still indexing"))) << status[1].toStdString();
}

TEST(LanguageControllerNavigation, QuickDefinitionAndGotoSuperMethodRunSafelyOnScheduler) {
    FeatureServer server;
    const std::string header = workPath("main.cpp");
    server.on("textDocument/definition", [](const Json&) { return Json::array(); });
    server.on("textDocument/declaration", [&](const Json&) {
        return Json::array({Json::object().set("uri", uriOf(header)).set("range", rangeJson(0, 6, 13))});
    });
    server.on("textDocument/prepareTypeHierarchy", [&](const Json&) {
        return Json::array({Json::object()
                                .set("name", "Derived")
                                .set("kind", 5)
                                .set("uri", uriOf(header))
                                .set("range", rangeJson(0, 0, 20))
                                .set("selectionRange", rangeJson(0, 6, 13))});
    });
    server.on("typeHierarchy/supertypes", [&](const Json&) {
        return Json::array({Json::object()
                                .set("name", "Base")
                                .set("kind", 5)
                                .set("uri", uriOf(header))
                                .set("range", rangeJson(0, 0, 15))
                                .set("selectionRange", rangeJson(0, 23, 27))});
    });

    FakeHost host("class Derived : public Base {};\n", header);
    host.caretAfter("class Der");
    Rig rig(server, host);

    rig.controller.quickDefinition();
    ASSERT_TRUE(pumpUntil([&] { return rig.controller.quickDefinitionPopup().isVisible(); }));
    rig.controller.quickDefinitionPopup().hide();

    rig.controller.gotoSuperMethod();
    ASSERT_TRUE(pumpUntil([&] { return host.opened.size() == 1; }));
    EXPECT_EQ(fl::documentKey(host.opened[0].first), fl::documentKey(header));
    EXPECT_EQ(host.opened[0].second, (fl::LspPosition{0, 23}));
}

TEST(LanguageControllerNavigation, GotoDeclarationGoesToHeaderAndGotoImplementationGoesToCppBody) {
    FeatureServer server;
    const std::string header = workPath("math_utils.h");
    const std::string impl = workPath("math_utils.cpp");
    server.on("textDocument/declaration", [&](const Json&) {
        return Json::array({Json::object().set("uri", uriOf(header)).set("range", rangeJson(2, 4, 15))});
    });
    server.on("textDocument/definition", [&](const Json&) {
        return Json::array({Json::object().set("uri", uriOf(impl)).set("range", rangeJson(8, 4, 15))});
    });
    server.on("textDocument/implementation", [](const Json&) { return Json::array(); });

    FakeHost host("#include \"math_utils.h\"\nint main() {\n    return computeHash(42);\n}\n", workPath("main.cpp"));
    host.caretAfter("computeH");
    Rig rig(server, host);

    // "Go to Declaration or Usages" jumps to the declaration in math_utils.h (not math_utils.cpp).
    rig.controller.gotoDeclaration();
    ASSERT_TRUE(pumpUntil([&] { return host.opened.size() == 1; }));
    EXPECT_EQ(fl::documentKey(host.opened[0].first), fl::documentKey(header));
    EXPECT_EQ(host.opened[0].second, (fl::LspPosition{2, 4}));

    // "Go to Implementation" jumps to the function implementation in math_utils.cpp even when
    // textDocument/implementation is empty.
    rig.controller.gotoImplementation();
    ASSERT_TRUE(pumpUntil([&] { return host.opened.size() == 2; }));
    EXPECT_EQ(fl::documentKey(host.opened[1].first), fl::documentKey(impl));
    EXPECT_EQ(host.opened[1].second, (fl::LspPosition{8, 4}));
}

// A clangd tweak action as clangd 23 sends it (command clangd.applyTweak with {file, selection, tweakID}).
Json tweakAction(const std::string& path, std::string id, const std::string& title, std::string kind = "refactor") {
    Json argument = Json::object()
                        .set("file", uriOf(path))
                        .set("selection", fl::toJson(fl::LspRange{{0, 0}, {0, 0}}))
                        .set("tweakID", std::move(id));
    return Json::object()
        .set("title", title)
        .set("kind", std::move(kind))
        .set("command", Json::object()
                            .set("title", title)
                            .set("command", "clangd.applyTweak")
                            .set("arguments", Json::array({std::move(argument)})));
}

// FND-3 (REF-0): Refactor This lists Rename, then the server's refactorings in the defined order
// (known tweaks first, then the rest in server order); quick fixes and source actions stay with
// Alt+Enter. Choosing a tweak executes it on the server; nothing edits text locally.
TEST(LanguageControllerRefactoring, TweakAdmissionPredatesCodeActionAndRefusesStaleSelectionBeforeExecute) {
    FeatureServer server;
    const auto path = workPath("main.cpp");
    std::atomic<int> captures = 0;
    std::atomic<bool> current = true;
    server.on("textDocument/codeAction", [&](const Json&) {
        EXPECT_EQ(captures.load(), 1);
        current = false; // Models the captured document version changing while the query is in flight.
        return Json::array({tweakAction(path, "ExtractVariable", "Extract subexpression to variable")});
    });
    FakeHost host("int f(){return 2+3;}", path);
    Rig rig(server, host);
    rig.controller.setCppSemanticEditFactory([&]() -> frost::Result<LanguageController::SemanticApply> {
        ++captures;
        return LanguageController::SemanticApply{[&](const fl::WorkspaceEdit&) -> frost::Result<void> {
            if (!current)
                return frost::fail(frost::ErrorCode::Busy, "captured version changed");
            return {};
        }};
    });
    std::vector<QString> status;
    QObject::connect(&rig.controller, &LanguageController::statusMessage,
                     [&](const auto& message) { status.push_back(message); });
    rig.controller.extractVariable();
    ASSERT_TRUE(pumpUntil([&] { return !status.empty(); }));
    EXPECT_TRUE(status.back().contains(QStringLiteral("captured version changed")));
    EXPECT_TRUE(server.paramsOf("workspace/executeCommand").empty());
    EXPECT_TRUE(rig.applier.edits.empty());
}
TEST(LanguageControllerRefactoring, MoveAndSafeDeleteDispatchToTheAppTransactionAndAstOwners) {
    FeatureServer server;
    FakeHost host("int main(){int unused; return 0;}", workPath("main.cpp"));
    Rig rig(server, host);
    std::vector<std::string> operations;
    rig.controller.setCppAstRefactoring([&](std::string operation) { operations.push_back(std::move(operation)); });
    rig.controller.moveSymbol();
    rig.controller.safeDelete();
    EXPECT_EQ(operations, (std::vector<std::string>{"move-file", "safe-delete"}));
    EXPECT_TRUE(rig.applier.edits.empty());
    EXPECT_EQ(host.undoSteps, 0);
}

TEST(LanguageControllerRefactoring, RefactorThisListsRenameThenServerActionsInOrder) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    server.on("textDocument/codeAction", [&](const Json&) {
        return Json::array({
            tweakAction(path, "SwapBinaryOperands", "Swap operands to *"),
            Json::object()
                .set("title", "Insert ';'")
                .set("kind", "quickfix")
                .set("edit", Json::object().set("changes", Json::object())),
            tweakAction(path, "ExtractVariable", "Extract subexpression to variable"),
            tweakAction(path, "OverridePureVirtuals", "Override pure virtual methods", "quickfix"),
            Json::object()
                .set("title", "Organize")
                .set("kind", "source.organizeImports")
                .set("command", Json::object().set("title", "Organize").set("command", "x.organize")),
        });
    });
    server.on("workspace/executeCommand", [](const Json&) { return Json("Tweak applied."); });
    FakeHost host("int total = a * b;\n", path);
    host.caretAfter("a *");
    Rig rig(server, host);

    rig.controller.refactorThis();
    auto& choice = rig.controller.choicePopup();
    ASSERT_TRUE(pumpUntil([&] { return choice.isVisible(); }));
    ASSERT_EQ(choice.list()->count(), 4);
    EXPECT_EQ(choice.list()->item(0)->text(), QStringLiteral("Rename..."));
    EXPECT_EQ(choice.list()->item(1)->text(), QStringLiteral("Extract subexpression to variable"));
    EXPECT_EQ(choice.list()->item(2)->text(), QStringLiteral("Override pure virtual methods"));
    EXPECT_EQ(choice.list()->item(3)->text(), QStringLiteral("Swap operands to *"));
    emit choice.chosen(1);
    ASSERT_TRUE(pumpUntil([&] { return server.paramsOf("workspace/executeCommand").size() == 1; }));
    const Json sent = server.paramsOf("workspace/executeCommand")[0];
    EXPECT_EQ(sent.stringAt("command"), "clangd.applyTweak");
    EXPECT_EQ(sent.find("arguments")->items().at(0).stringAt("tweakID"), "ExtractVariable");
    EXPECT_EQ(host.undoSteps, 0); // The edit arrives as the server's workspace/applyEdit.
    EXPECT_EQ(host.text(), "int total = a * b;\n");
}

// FND-3: named commands map to clangd tweaks by id; commands without an engine stay registered but
// disabled with the epic in their description, and report it without touching the text.
TEST(LanguageControllerRefactoring, NamedCommandsRunClangdTweaksOrReportTheirEpic) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    server.on("textDocument/codeAction", [&](const Json&) {
        return Json::array({tweakAction(path, "ExtractFunction", "Extract to function"),
                            tweakAction(path, "MemberwiseConstructor", "Define constructor")});
    });
    server.on("workspace/executeCommand", [](const Json&) { return Json("Tweak applied."); });
    FakeHost host("struct P { int x; };\nint f() { int a = 1; return a; }\n", path);
    host.caretAfter("int a");
    Rig rig(server, host);
    std::vector<QString> status;
    QObject::connect(&rig.controller, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });
    frost::input::ActionRegistry registry;
    ASSERT_TRUE(rig.controller.registerActions(registry).has_value());
    frost::input::ActionContext inEditor;
    inEditor.keys.set(frost::input::ContextKey::EditorFocus);
    for (const char* id : {"refactor.extractVariable", "refactor.extractMethod", "code.generate",
                           "code.implementMethods", "code.optimizeImports", "refactor.this"}) {
        EXPECT_TRUE(registry.isEnabled(*registry.find(id), inEditor)) << id;
    }
    struct Disabled {
        const char* id;
        const char* epic;
    };
    for (const Disabled& d : {Disabled{"refactor.changeSignature", "REF-6"},
                              Disabled{"refactor.extractConstant", "REF-7"}, Disabled{"refactor.extractField", "REF-7"},
                              Disabled{"refactor.extractParameter", "REF-7"}, Disabled{"refactor.move", "REF-8"},
                              Disabled{"refactor.safeDelete", "REF-8"}, Disabled{"code.overrideMethods", "REF-9"}}) {
        const auto action = registry.find(d.id);
        ASSERT_TRUE(action.has_value()) << d.id; // Still visible (Find Action, menus).
        EXPECT_FALSE(registry.isEnabled(*action, inEditor)) << d.id;
        EXPECT_NE(registry.info(*action).description.find(d.epic), std::string::npos) << d.id;
    }

    rig.controller.changeSignature();
    rig.controller.moveSymbol();
    rig.controller.safeDelete();
    rig.controller.extractField();
    ASSERT_EQ(status.size(), 4U);
    EXPECT_TRUE(status[0].contains(QStringLiteral("REF-6")));
    EXPECT_TRUE(status[1].contains(QStringLiteral("REF-8")));
    EXPECT_TRUE(status[3].contains(QStringLiteral("REF-7")));
    EXPECT_EQ(host.undoSteps, 0);
    EXPECT_TRUE(rig.applier.edits.empty());

    rig.controller.extractMethod();
    ASSERT_TRUE(pumpUntil([&] { return server.paramsOf("workspace/executeCommand").size() == 1; }));
    EXPECT_EQ(server.paramsOf("workspace/executeCommand")[0].find("arguments")->items().at(0).stringAt("tweakID"),
              "ExtractFunction");
    rig.controller.generateConstructor();
    ASSERT_TRUE(pumpUntil([&] { return server.paramsOf("workspace/executeCommand").size() == 2; }));
    EXPECT_EQ(server.paramsOf("workspace/executeCommand")[1].find("arguments")->items().at(0).stringAt("tweakID"),
              "MemberwiseConstructor");
    // A tweak clangd does not offer here: a message, no edit.
    rig.controller.extractVariable();
    ASSERT_TRUE(pumpUntil([&] { return status.size() == 5; }));
    EXPECT_TRUE(status[4].contains(QStringLiteral("Extract Variable")));
    EXPECT_EQ(server.paramsOf("workspace/executeCommand").size(), 2U);
    EXPECT_EQ(host.undoSteps, 0);
}

// FND-3: Generate lists what clangd offers, and the generators without an engine disabled (REF-9).
TEST(LanguageControllerRefactoring, GenerateShowsClangdTweaksAndDisabledGenerators) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    server.on("textDocument/codeAction", [&](const Json&) {
        return Json::array({tweakAction(path, "SpecialMembers", "Declare implicit copy/move members"),
                            tweakAction(path, "MemberwiseConstructor", "Define constructor")});
    });
    FakeHost host("struct P {\n    int x;\n};\n", path);
    host.caretAfter("struct P");
    Rig rig(server, host);
    rig.controller.generateCode();
    auto& choice = rig.controller.choicePopup();
    ASSERT_TRUE(pumpUntil([&] { return choice.isVisible(); }));
    ASSERT_EQ(choice.list()->count(), 6);
    EXPECT_EQ(choice.list()->item(0)->text(), QStringLiteral("Define constructor"));
    EXPECT_EQ(choice.list()->item(1)->text(), QStringLiteral("Declare implicit copy/move members"));
    EXPECT_TRUE(choice.list()->item(0)->flags().testFlag(Qt::ItemIsEnabled));
    for (int row = 2; row < 6; ++row) {
        EXPECT_FALSE(choice.list()->item(row)->flags().testFlag(Qt::ItemIsEnabled)) << row;
        EXPECT_TRUE(choice.list()->item(row)->toolTip().contains(QStringLiteral("REF-9"))) << row;
    }
    EXPECT_EQ(choice.list()->currentRow(), 0);
}

// FND-3 (REF-11): Optimize Includes applies clangd's include-cleaner fix for its diagnostics; the
// old line sorting is gone.
TEST(LanguageControllerRefactoring, OptimizeIncludesAppliesClangdsIncludeCleanerFix) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    server.on("textDocument/codeAction", [&](const Json&) {
        const auto remove = [&](std::string title, std::uint32_t lines) {
            Json edits = Json::array();
            for (std::uint32_t line = 0; line < lines; ++line) {
                edits.push(
                    Json::object().set("range", fl::toJson(fl::LspRange{{line, 0}, {line + 1, 0}})).set("newText", ""));
            }
            return Json::object()
                .set("title", std::move(title))
                .set("kind", "quickfix")
                .set("edit", Json::object().set("changes", Json::object().set(uriOf(path), std::move(edits))));
        };
        return Json::array({remove("remove #include directive", 1), remove("remove all unused includes", 2)});
    });
    FakeHost host("#include \"b.h\"\n#include \"a.h\"\nint main() {}\n", path);
    Rig rig(server, host);
    for (std::uint32_t line = 0; line < 2; ++line) {
        fl::LiveDiagnostic unused;
        unused.code = "unused-includes";
        unused.source = "clangd";
        unused.message = "Included header is not used directly (fixes available)";
        unused.lspRange = fl::LspRange{{line, 0}, {line, 14}};
        rig.diagnostics.push_back(unused);
    }
    rig.controller.optimizeImports();
    ASSERT_TRUE(pumpUntil([&] { return rig.applier.edits.size() == 1; }));
    EXPECT_EQ(rig.applier.labels[0], "Optimize Includes");
    EXPECT_EQ(rig.applier.edits[0].files.at(0).edits.size(), 2U); // "remove all unused includes".
    const auto sent = server.paramsOf("textDocument/codeAction");
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].find("context")->find("diagnostics")->size(), 2U);
    EXPECT_EQ(host.undoSteps, 0);
}

// FND-3 (LUA-2): C++/C# editor features never fire in a .lua file, and Lua's commands that need
// an engine are disabled; Lua keeps the LSP-backed ones.
TEST(LanguageControllerRefactoring, CppFeaturesNeverTouchLuaFiles) {
    FeatureServer server;
    const std::string original = "local items = {}\nitems.if";
    FakeHost host(original, workPath("init.lua"));
    host.caretAfter("items.if");
    Rig rig(server, host);
    std::vector<QString> status;
    QObject::connect(&rig.controller, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });
    EXPECT_FALSE(rig.controller.keyPress(Qt::Key_Tab, Qt::NoModifier)); // No postfix template.
    rig.controller.surroundWith();
    rig.controller.completeStatement();
    rig.controller.insertLiveTemplate();
    rig.controller.optimizeImports();
    rig.controller.extractVariable();
    rig.controller.generateCode();
    EXPECT_EQ(host.text(), original);
    EXPECT_EQ(host.undoSteps, 0);
    EXPECT_FALSE(rig.controller.choicePopup().isVisible());
    EXPECT_EQ(status.size(), 6U);
    EXPECT_TRUE(server.paramsOf("textDocument/codeAction").empty());

    frost::input::ActionRegistry registry;
    ASSERT_TRUE(rig.controller.registerActions(registry).has_value());
    frost::input::ActionContext inEditor;
    inEditor.keys.set(frost::input::ContextKey::EditorFocus);
    for (const char* id :
         {"code.surroundWith", "code.completeStatement", "code.insertLiveTemplate", "code.optimizeImports",
          "refactor.extractVariable", "refactor.extractMethod", "code.generate", "code.implementMethods"}) {
        EXPECT_FALSE(registry.isEnabled(*registry.find(id), inEditor)) << id;
    }
    for (const char* id : {"code.completion", "refactor.rename", "navigate.declaration", "code.reformat",
                           "code.showIntentions", "refactor.this"}) {
        EXPECT_TRUE(registry.isEnabled(*registry.find(id), inEditor)) << id;
    }
}

// FND-3: live templates are per language (no C# 'prop' in C++), and a postfix template takes only
// the operand that ends at the dot and only at the start of a statement.
TEST(LanguageControllerRefactoring, LiveAndPostfixTemplatesFollowTheLanguage) {
    FeatureServer server;
    {
        FakeHost host("int main() {\n    \n}\n", workPath("main.cpp"));
        host.caretAfter("{\n    ");
        Rig rig(server, host);
        rig.controller.insertLiveTemplate();
        auto& choice = rig.controller.choicePopup();
        ASSERT_TRUE(choice.isVisible());
        bool hasProp = false;
        bool hasFor = false;
        for (int row = 0; row < choice.list()->count(); ++row) {
            hasProp = hasProp || choice.list()->item(row)->text().startsWith(QStringLiteral("prop"));
            hasFor = hasFor || choice.list()->item(row)->text().startsWith(QStringLiteral("for"));
        }
        EXPECT_FALSE(hasProp);
        EXPECT_TRUE(hasFor);
        choice.hide();
    }
    {
        FakeHost host("class C {\n    \n}\n", workPath("C.cs"));
        host.caretAfter("{\n    ");
        Rig rig(server, host);
        rig.controller.insertLiveTemplate();
        auto& choice = rig.controller.choicePopup();
        ASSERT_TRUE(choice.isVisible());
        bool hasProp = false;
        for (int row = 0; row < choice.list()->count(); ++row) {
            hasProp = hasProp || choice.list()->item(row)->text().startsWith(QStringLiteral("prop"));
        }
        EXPECT_TRUE(hasProp);
        choice.hide();
    }
    {
        FakeHost host("void f() {\n    items.size().if\n}\n", workPath("main.cpp"));
        host.caretAfter("items.size().if");
        Rig rig(server, host);
        EXPECT_TRUE(rig.controller.keyPress(Qt::Key_Tab, Qt::NoModifier));
        EXPECT_EQ(host.text(), "void f() {\n    if (items.size()) {\n        \n    }\n}\n");
    }
    {
        // Not at the start of a statement: the old whole-line expansion produced
        // "if (total += items.size()) {...}"; now nothing happens.
        const std::string text = "void f() {\n    total += items.size().if\n}\n";
        FakeHost host(text, workPath("main.cpp"));
        host.caretAfter("items.size().if");
        Rig rig(server, host);
        EXPECT_FALSE(rig.controller.keyPress(Qt::Key_Tab, Qt::NoModifier));
        EXPECT_EQ(host.text(), text);
    }
}

// Rename's "search in comments and strings" adds occurrences inside comments and string literals
// only: an unrelated identifier with the same name in code is left alone.
TEST(LanguageControllerRefactoring, RenameInCommentsAndStringsLeavesCodeAlone) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    server.on("textDocument/prepareRename", [](const Json&) { return rangeJson(1, 4, 9); });
    server.on("textDocument/rename", [&](const Json& params) {
        return Json::object().set(
            "changes",
            Json::object().set(
                uriOf(path),
                Json::array(
                    {Json::object().set("range", rangeJson(1, 4, 9)).set("newText", params.stringAt("newName"))})));
    });
    // Line 1 holds the renamed symbol; "value" on line 2 is another variable in another scope.
    FakeHost host("// value is the count\nint value = 1;\nvoid g() { int value = 2; const char* s = \"value\"; }\n",
                  path);
    host.caretAfter("int va");
    Rig rig(server, host);
    rig.controller.rename();
    auto& input = rig.controller.renameInput();
    ASSERT_TRUE(pumpUntil([&] { return input.isVisible(); }));
    input.searchCommentsCheckbox()->setChecked(true);
    emit input.accepted(QStringLiteral("amount"));
    ASSERT_TRUE(pumpUntil([&] { return rig.applier.edits.size() == 1; }));
    const auto& edits = rig.applier.edits[0].files.at(0).edits;
    ASSERT_EQ(edits.size(), 3U); // The server's, the comment's and the string's.
    std::vector<std::uint32_t> lines;
    for (const fl::TextEdit& edit : edits) {
        lines.push_back(edit.range.start.line);
        EXPECT_EQ(edit.newText, "amount");
    }
    std::ranges::sort(lines);
    EXPECT_EQ(lines, (std::vector<std::uint32_t>{0, 1, 2}));
    for (const fl::TextEdit& edit : edits) {
        if (edit.range.start.line == 2) {
            EXPECT_GT(edit.range.start.character, 40U); // Inside the string, not "int value".
        }
    }
}

TEST(LanguageControllerNavigation, CallHierarchyPopulatesIncomingAndOutgoingAndNeverFabricatesOnEmpty) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    bool returnEmpty = false;
    server.on("textDocument/prepareCallHierarchy", [&](const Json&) {
        if (returnEmpty) {
            return Json::array();
        }
        return Json::array({Json::object()
                                .set("name", "BeginScene")
                                .set("kind", 12)
                                .set("uri", uriOf(path))
                                .set("range", rangeJson(1, 0, 20))
                                .set("selectionRange", rangeJson(1, 5, 15))});
    });
    server.on("callHierarchy/incomingCalls", [&](const Json&) {
        return Json::array({Json::object()
                                .set("from", Json::object()
                                                 .set("name", "OnUpdate")
                                                 .set("kind", 6)
                                                 .set("uri", uriOf(path))
                                                 .set("range", rangeJson(5, 0, 25))
                                                 .set("selectionRange", rangeJson(5, 5, 13)))
                                .set("fromRanges", Json::array({rangeJson(6, 4, 14)}))});
    });
    server.on("callHierarchy/outgoingCalls", [&](const Json&) {
        return Json::array({Json::object()
                                .set("to", Json::object()
                                               .set("name", "ResetStats")
                                               .set("kind", 12)
                                               .set("uri", uriOf(path))
                                               .set("range", rangeJson(0, 0, 18))
                                               .set("selectionRange", rangeJson(0, 5, 15)))
                                .set("fromRanges", Json::array({rangeJson(2, 4, 14)}))});
    });

    FakeHost host("void ResetStats() {}\nvoid BeginScene() {\n    ResetStats();\n}\n", path);
    host.caretAfter("void Begin");
    Rig rig(server, host);

    QTreeWidget* tree = rig.controller.hierarchyPopup().tree();
    ASSERT_NE(tree, nullptr);
    rig.controller.callHierarchy();
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 1; }));
    EXPECT_TRUE(tree->topLevelItem(0)->text(0).contains(QStringLiteral("BeginScene")));
    EXPECT_TRUE(tree->topLevelItem(0)->child(0)->text(0).contains(QStringLiteral("OnUpdate")));

    // Switch to Outgoing Calls mode (secondaryMode = true).
    emit rig.controller.hierarchyPopup().modeChanged(true);
    ASSERT_TRUE(pumpUntil([&] {
        return tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 1 &&
               tree->topLevelItem(0)->child(0)->text(0).contains(QStringLiteral("ResetStats"));
    }));

    // Empty prepareCallHierarchy response must emit a status message and never fabricate a root node (ADR 0078).
    returnEmpty = true;
    std::vector<QString> status;
    QObject::connect(&rig.controller, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });
    rig.controller.callHierarchy();
    ASSERT_TRUE(pumpUntil([&] { return !status.empty(); }));
    EXPECT_TRUE(status[0].contains(QStringLiteral("No call hierarchy found"))) << status[0].toStdString();
}

TEST(LanguageControllerNavigation, TypeHierarchyPopulatesSupertypesAndSubtypesAndNeverFabricatesOnEmpty) {
    FeatureServer server;
    const std::string path = workPath("layer.hpp");
    bool returnEmpty = false;
    int supertypeRequests = 0;
    int subtypeRequests = 0;
    server.on("textDocument/prepareTypeHierarchy", [&](const Json&) {
        if (returnEmpty) {
            return Json::array();
        }
        return Json::array({Json::object()
                                .set("name", "EditorLayer")
                                .set("kind", 5)
                                .set("uri", uriOf(path))
                                .set("range", rangeJson(1, 0, 30))
                                .set("selectionRange", rangeJson(1, 6, 17))});
    });
    server.on("typeHierarchy/subtypes", [&](const Json&) {
        ++subtypeRequests;
        return Json::array({Json::object()
                                .set("name", "SandboxEditorLayer")
                                .set("kind", 5)
                                .set("uri", uriOf(path))
                                .set("range", rangeJson(2, 0, 40))
                                .set("selectionRange", rangeJson(2, 6, 24))});
    });
    server.on("typeHierarchy/supertypes", [&](const Json&) {
        ++supertypeRequests;
        return Json::array({Json::object()
                                .set("name", "Layer")
                                .set("kind", 5)
                                .set("uri", uriOf(path))
                                .set("range", rangeJson(0, 0, 15))
                                .set("selectionRange", rangeJson(0, 6, 11))});
    });

    FakeHost host("class Layer {};\nclass EditorLayer : public Layer {};\n", path);
    host.caretAfter("class Editor");
    Rig rig(server, host);

    QTreeWidget* tree = rig.controller.hierarchyPopup().tree();
    ASSERT_NE(tree, nullptr);
    rig.controller.typeHierarchy();
    ASSERT_TRUE(pumpUntil([&] { return tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 1; }));
    EXPECT_TRUE(tree->topLevelItem(0)->text(0).contains(QStringLiteral("EditorLayer")));
    EXPECT_TRUE(tree->topLevelItem(0)->child(0)->text(0).contains(QStringLiteral("Layer")));

    // Switch from the primary Supertypes mode to secondary Subtypes mode.
    emit rig.controller.hierarchyPopup().modeChanged(true);
    ASSERT_TRUE(pumpUntil([&] {
        return tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 1 &&
               tree->topLevelItem(0)->child(0)->text(0).contains(QStringLiteral("SandboxEditorLayer"));
    }));

    // A Code Vision implementation count opens its matching subtype results immediately.
    const int previousSupertypes = supertypeRequests;
    const int previousSubtypes = subtypeRequests;
    rig.controller.typeImplementations();
    ASSERT_TRUE(pumpUntil([&] { return subtypeRequests > previousSubtypes; }));
    ASSERT_TRUE(pumpUntil([&] {
        return tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 1 &&
               tree->topLevelItem(0)->child(0)->text(0) == QStringLiteral("SandboxEditorLayer");
    }));
    EXPECT_EQ(supertypeRequests, previousSupertypes);

    // Empty prepareTypeHierarchy response must emit a status message and never fabricate a root node (ADR 0078).
    returnEmpty = true;
    std::vector<QString> status;
    QObject::connect(&rig.controller, &LanguageController::statusMessage,
                     [&](const QString& message) { status.push_back(message); });
    rig.controller.typeHierarchy();
    ASSERT_TRUE(pumpUntil([&] { return !status.empty(); }));
    EXPECT_TRUE(status[0].contains(QStringLiteral("No type hierarchy found"))) << status[0].toStdString();
}

} // namespace

namespace frost::ui::lang {
namespace {
TEST(LanguageControllerTemplates, SharedLoopPlaceholderEditsMirrorBeforeAdvancing) {
    FeatureServer server;
    FakeHost host("void f() {\n    \n}\n", workPath("main.cpp"));
    host.caretAfter("{\n    "); Rig rig(server,host);
    rig.controller.insertLiveTemplate();
    auto& choice=rig.controller.choicePopup(); ASSERT_TRUE(choice.isVisible());
    choice.chosen(0); // The catalogue's actual first C++ item is the index loop.
    ASSERT_TRUE(host.selected); const auto active=*host.selected;
    fl::ByteEdit edit{active,"index"};
    ASSERT_TRUE(host.applyEdits(std::span(&edit,1),"Edit placeholder",ByteOffset{active.begin.value()+5}));
    EXPECT_TRUE(rig.controller.keyPress(Qt::Key_Tab,Qt::NoModifier));
    EXPECT_NE(host.text().find("std::size_t index = 0; index < count; ++index"),std::string::npos);
    ASSERT_TRUE(host.selected);
    EXPECT_EQ(host.snapshot()->text(*host.selected),"count");
}
TEST(LanguageControllerTemplates, CMakePostfixAndLiveSetsUseCurrentEditorText) {
    FeatureServer server;
    FakeHost host("    items.for",workPath("CMakeLists.txt")); host.caretAfter("items.for");
    Rig rig(server,host);
    EXPECT_TRUE(rig.controller.keyPress(Qt::Key_Tab,Qt::NoModifier));
    EXPECT_EQ(host.text(),"    foreach(item IN LISTS items)\n        \n    endforeach()");
    rig.controller.insertLiveTemplate();
    auto& choices=rig.controller.choicePopup(); ASSERT_TRUE(choices.isVisible());
    bool function=false; for(int i=0;i<choices.list()->count();++i) function |= choices.list()->item(i)->text().startsWith("function");
    EXPECT_TRUE(function);
}
TEST(LanguageControllerTemplates, StalePopupCannotInsertIntoChangedDocument) {
    FeatureServer server; FakeHost host("",workPath("main.cpp")); Rig rig(server,host);
    rig.controller.insertLiveTemplate(); ASSERT_TRUE(rig.controller.choicePopup().isVisible());
    host.type("changed"); rig.controller.choicePopup().chosen(0);
    EXPECT_EQ(host.text(),"changed");
}
} // namespace
} // namespace frost::ui::langnamespace frost::ui::lang {
namespace {
TEST(LanguageControllerTemplates, ClosingControllerDuringAuthorPromptDoesNotApplyATemplate) {
    FeatureServer server; FakeHost host("",workPath("main.cpp"));
    auto rig=std::make_unique<Rig>(server,host);
    rig->controller.insertLiveTemplate();
    auto& popup=rig->controller.choicePopup();
    std::size_t selected=static_cast<std::size_t>(popup.list()->count());
    for(int row=0;row<popup.list()->count();++row) if(popup.list()->item(row)->text().startsWith("Doxygen")) selected=static_cast<std::size_t>(row);
    ASSERT_LT(selected,static_cast<std::size_t>(popup.list()->count()));
    QTimer::singleShot(0,[&] {
        auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        rig.reset();
        if(dialog) dialog->reject();
    });
    popup.chosen(selected);
    EXPECT_FALSE(rig); EXPECT_TRUE(host.text().empty());
}
}
}