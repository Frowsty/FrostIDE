// SPDX-License-Identifier: Apache-2.0
//
// LanguageController offscreen against the scripted language server (tests/lang/feature_server)
// and a fake editor host over a TextBuffer: completion from Ctrl+Space and from typing '.',
// Enter commits and Esc cancels, parameter info on '(', go to declaration into another file,
// related files, find usages into the panel, highlight usages after the caret debounce, the
// file structure popup, rename and quick fixes through the edit applier (a refusal reaches the
// status bar), reformat, and the registered IntelliJ action ids.

#include "frost/input/action.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "lang/feature_server.hpp"
#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QScopeGuard>
#include <QShortcut>
#include <QThread>
#include <QTreeWidget>
#include <QWidget>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fl = frost::lang;
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

// The active editor: one document in a TextBuffer.
class FakeHost final : public frost::ui::lang::IEditorLanguageHost {
public:
    explicit FakeHost(std::string text) : m_buffer(frost::text::TextBuffer::fromUtf8(std::move(text)).value()) {}

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

Json completionItem(std::string label, int kind, std::string sort, std::string insert = {}, bool snippet = false) {
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
    ASSERT_EQ(choice.list()->count(), 1);
    EXPECT_EQ(choice.list()->item(0)->text(), QStringLiteral("Insert ';'"));
    const auto sentActions = server.paramsOf("textDocument/codeAction");
    ASSERT_EQ(sentActions.size(), 1u);
    const Json* sentDiagnostics = sentActions[0].find("context")->find("diagnostics");
    ASSERT_EQ(sentDiagnostics->size(), 1u); // The line's diagnostic, though the caret is elsewhere.
    rig.applier.refuse = true;
    emit choice.chosen(0);
    ASSERT_EQ(status.size(), 1u);
    EXPECT_TRUE(status[0].contains(QStringLiteral("save or reload")));
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
    for (const char* id :
         {"code.completion", "code.parameterInfo", "code.quickDocumentation", "navigate.declaration",
          "navigate.implementation", "navigate.relatedFiles", "navigate.findUsages", "navigate.highlightUsages",
          "navigate.fileStructure", "navigate.gotoSymbol", "refactor.rename", "code.showIntentions", "code.reformat"}) {
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
}

} // namespace
