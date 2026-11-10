// SPDX-License-Identifier: Apache-2.0
//
// The corpus tier of CPP-5 and REF-12 (label corpus; verify -Corpus only) on the pinned FrostEngine
// clone. R4's C++ exit criteria: "clangd semantic tokens, inlay hints and signature help work in
// the clone" and "format on save equals the clang-format CLI output on 5 FrostEngine files".
/*
 * CPP-5. The pinned clangd runs on the clone with the compile_commands.json its configured preset
 * wrote (read only; no background index, so nothing is written into the clone). For three engine
 * sources, FrostIDE's models of semantic tokens, inlay hints, document links, folding ranges and
 * the AST equal clangd's raw answers to the same requests, decoded here independently (semantic
 * tokens by hand from the relative stream and the legend's names). Signature help is checked
 * against a second clangd answer: at each of the first parameter-name inlay hints (an argument's
 * start), clangd's signature help names that parameter. Every link's target exists and is the
 * header its #include names.
 *
 * REF-12. Five engine files are copied with the clone's .clang-format into a work directory and
 * their indentation is stripped (so the formatter has work to do). The workbench, with
 * editor.formatOnSave on and the real clangd, saves each one; the saved bytes must equal the pinned
 * clang-format CLI's output (`clang-format -style=file`) for the same stripped text, which must
 * differ from that text.
 *
 * A missing corpus, or one not at its pin, fails (the tier never skips); so does a preset that was
 * never configured (python tools/corpora.py engine --configure).
 */

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/code_assist.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/document_features.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/semantic_tokens.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/settings/keys.hpp"
#include "frost/text/document.hpp"

#include "support/crash_dialogs.hpp"
#include "support/engine_lock.hpp"
#include "workbench_rig.hpp"
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fl = frost::lang;
using fl::Json;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
using namespace std::chrono_literals;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;

std::string corpusRoot() { return qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString(); }

// <root>/frostengine-<sha12>, checked: present and detached at its pin; "" with a failure otherwise.
std::string checkedCorpus() {
    const std::string root = corpusRoot();
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus";
        return {};
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const std::string corpus = frost::io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
    auto& fs = frost::io::realFileSystem();
    const auto head = fs.readFile(corpus + "/.git/HEAD");
    if (!head || head->substr(0, 40) != sha) {
        ADD_FAILURE() << corpus << " is missing or not detached at its pin (python tools/corpora.py verify)";
        return {};
    }
    return corpus;
}

std::string clangdPath() {
    auto found = fl::findClangd(frost::io::realFileSystem(), fl::defaultClangdSearch());
    EXPECT_TRUE(found.has_value()) << found.error().describe();
    return found ? found->path : std::string();
}

template<class Condition>
bool pollFor(Condition condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

std::int64_t intAt(const Json& object, std::string_view key) { return object.integerAt(key).value_or(-1); }

// The raw relative token stream decoded by hand: (line, UTF-8 column, length, type name, modifier bits).
struct RawToken {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t length = 0;
    std::string type;
    std::uint32_t modifiers = 0;
};

std::vector<RawToken> decodeByHand(const Json& data, const Json& legendTypes) {
    std::vector<RawToken> out;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    const auto& items = data.items();
    for (std::size_t i = 0; i + 4 < items.size(); i += 5) {
        const auto deltaLine = static_cast<std::uint32_t>(items[i].asInteger().value_or(0));
        const auto deltaStart = static_cast<std::uint32_t>(items[i + 1].asInteger().value_or(0));
        line += deltaLine;
        column = deltaLine == 0 ? column + deltaStart : deltaStart;
        const auto type = static_cast<std::size_t>(items[i + 3].asInteger().value_or(0));
        out.push_back(RawToken{line, column, static_cast<std::uint32_t>(items[i + 2].asInteger().value_or(0)),
                               type < legendTypes.size() ? std::string(*legendTypes.items()[type].asString()) : "?",
                               static_cast<std::uint32_t>(items[i + 4].asInteger().value_or(0))});
    }
    return out;
}

std::size_t countNodes(const Json& node) {
    std::size_t count = 1;
    if (const Json* children = node.find("children"); children != nullptr) {
        for (const Json& child : children->items()) {
            count += countNodes(child);
        }
    }
    return count;
}

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

TEST(CorpusCppEditor, SemanticTokensInlayHintsSignatureHelpLinksFoldsAndAstMatchClangdOnTheClone) {
    frost::test::disableCrashDialogs();
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    // The build tree's compile_commands.json is read: no reconfigure may rewrite it meanwhile.
    const frost::test::EngineBuildLock buildLock(corpusRoot() + "/frostengine-" + sha.substr(0, 12) + ".build.lock");
    ASSERT_TRUE(buildLock.held());
    auto& fs = frost::io::realFileSystem();
    const std::string buildDir = corpus + "/build/" + FROST_CORPUS_FROSTENGINE_PRESET;
    ASSERT_TRUE(fs.exists(buildDir + "/compile_commands.json").value_or(false))
        << "the preset was never configured: python tools/corpora.py engine --configure";

    auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "corpus-cpp-editor"});
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    fl::ClangdServiceOptions options;
    options.clangdPath = clangdPath();
    options.workspaceRoot = corpus;
    options.launch.jobs = 2;
    options.launch.backgroundIndex = false; // Nothing is written into the clone.
    options.compileCommands = std::make_shared<fl::ExistingCompileCommandsProvider>(fs, buildDir);
    auto service = std::make_unique<fl::ClangdService>(frost::io::realProcessLauncher(),
                                                       frost::io::AsyncContext{**reactor, pool}, std::move(options));
    ASSERT_TRUE(service->start().has_value());
    ASSERT_TRUE(pollFor([&] { return service->state() == fl::ClangdState::Running; }, 120s));
    const auto run = [&]<class T>(frost::sched::Task<T> task) {
        return pool.spawn(frost::sched::Lane::UserVisible, std::move(task)).join();
    };
    const fl::ServerCapabilities caps = *service->capabilities();
    const auto support = fl::semanticTokensSupport(caps);
    ASSERT_TRUE(support.has_value());
    const Json* legendTypes = caps.raw.find("semanticTokensProvider")->find("legend")->find("tokenTypes");
    ASSERT_NE(legendTypes, nullptr);

    for (const std::string relative : {"Frost-Editor/src/EditorApp.cpp", "Frost/src/Frost/Core/Application.cpp",
                                       "Frost-Runtime/src/RuntimeApp.cpp"}) {
        const std::string file = frost::io::normalizePath(corpus + "/" + relative).value_or("");
        SCOPED_TRACE(file);
        auto bytes = fs.readFile(file);
        ASSERT_TRUE(bytes.has_value());
        auto document = frost::text::Document::fromUtf8(*bytes);
        ASSERT_TRUE(document.has_value());
        const frost::text::Snapshot& snapshot = document->text();
        ASSERT_TRUE(service->openDocument(file, snapshot).has_value());
        // The first AST (with the engine's precompiled header) is built when clangd publishes
        // diagnostics for the text; a clean file's empty publish counts.
        ASSERT_TRUE(pollFor(
            [&] {
                return service->diagnosedVersion(file).has_value() &&
                       service->diagnosedVersion(file) == service->documentVersion(file);
            },
            10min));
        const Json identifier = fl::textDocumentIdentifier(file).value();
        const auto raw = [&](std::string method, Json params) {
            auto answer = run(service->request(std::move(method), std::move(params)));
            EXPECT_TRUE(answer.has_value()) << answer.error().describe();
            return answer.value_or(Json());
        };

        // Semantic tokens: FrostIDE's decode equals the hand decode of clangd's /full answer.
        const auto data = run(fl::requestSemanticTokens(*service, file, std::nullopt, false));
        ASSERT_TRUE(data.has_value()) << data.error().describe();
        const auto tokens = fl::decodeSemanticTokens(data->data, support->legend, snapshot, service->encoding());
        ASSERT_TRUE(tokens.has_value());
        const Json rawTokens = raw("textDocument/semanticTokens/full", Json::object().set("textDocument", identifier));
        const std::vector<RawToken> byHand = decodeByHand(*rawTokens.find("data"), *legendTypes);
        ASSERT_FALSE(byHand.empty());
        ASSERT_EQ(tokens->size(), byHand.size());
        for (std::size_t i = 0; i < byHand.size(); ++i) {
            const fl::SemanticToken& token = (*tokens)[i];
            const fl::LspPosition start = fl::toLspPosition(snapshot, token.range.begin, service->encoding());
            const fl::LspPosition end = fl::toLspPosition(snapshot, token.range.end, service->encoding());
            ASSERT_EQ(start.line, byHand[i].line) << i;
            ASSERT_EQ(start.character, byHand[i].column) << i;
            ASSERT_EQ(end.character - start.character, byHand[i].length) << i;
            ASSERT_EQ(token.type, fl::parseSemanticTokenType(byHand[i].type)) << i << " " << byHand[i].type;
        }

        // Inlay hints: the model's labels and positions are clangd's.
        const auto lines = static_cast<std::uint32_t>(snapshot.lineCount());
        const fl::LspRange whole{{0, 0}, {lines, 0}};
        const Json rawHints = raw("textDocument/inlayHint",
                                  Json::object().set("textDocument", identifier).set("range", fl::toJson(whole)));
        const auto hints = run(fl::requestInlayHints(*service, file, whole));
        ASSERT_TRUE(hints.has_value()) << hints.error().describe();
        ASSERT_EQ(hints->size(), rawHints.size());
        ASSERT_FALSE(hints->empty()) << "clangd gave no inlay hints for an engine source";
        for (std::size_t i = 0; i < hints->size(); ++i) {
            const Json& item = rawHints.items()[i];
            std::string label;
            if (const Json* l = item.find("label"); l->asString() != nullptr) {
                label = *l->asString();
            } else {
                for (const Json& part : l->items()) {
                    label += part.stringAt("value");
                }
            }
            label.erase(0, label.find_first_not_of(' '));
            label.erase(label.find_last_not_of(' ') + 1);
            EXPECT_EQ((*hints)[i].label, label) << i;
            EXPECT_EQ((*hints)[i].position, fl::parsePosition(*item.find("position"))) << i;
        }

        // Signature help: at the first parameter-name hints, clangd's signatures name that parameter.
        std::size_t checked = 0;
        for (const fl::LspInlayHint& hint : *hints) {
            if (hint.kind != fl::LspInlayHintKind::Parameter || !hint.label.ends_with(':') || checked == 5) {
                continue;
            }
            const std::string name = hint.label.substr(0, hint.label.size() - 1);
            const auto help = run(fl::requestSignatureHelp(*service, file, hint.position, std::nullopt, false));
            ASSERT_TRUE(help.has_value()) << help.error().describe();
            ASSERT_TRUE(help->has_value())
                << "no signature help at the argument of " << name << " (line " << hint.position.line + 1 << ")";
            EXPECT_TRUE(std::ranges::any_of(
                (*help)->signatures,
                [&](const fl::SignatureInfo& signature) { return signature.label.find(name) != std::string::npos; }))
                << name << " at line " << hint.position.line + 1;
            ++checked;
        }
        EXPECT_GT(checked, 0U) << "no parameter-name hint to check signature help at";

        // Document links: clangd's, each target on disk and named by its #include.
        const Json rawLinks = raw("textDocument/documentLink", Json::object().set("textDocument", identifier));
        const auto links = run(fl::requestDocumentLinks(*service, file));
        ASSERT_TRUE(links.has_value());
        ASSERT_EQ(links->size(), rawLinks.size());
        ASSERT_FALSE(links->empty());
        for (std::size_t i = 0; i < links->size(); ++i) {
            const fl::DocumentLink& link = (*links)[i];
            EXPECT_EQ(link.uri, rawLinks.items()[i].stringAt("target"));
            EXPECT_TRUE(fs.exists(link.path).value_or(false)) << link.path;
            const auto begin = fl::fromLspPosition(snapshot, link.range.start, service->encoding());
            const auto end = fl::fromLspPosition(snapshot, link.range.end, service->encoding());
            ASSERT_TRUE(begin && end);
            std::string spelled = snapshot.text(frost::text::ByteRange{*begin, *end});
            std::erase_if(spelled, [](char c) { return c == '"' || c == '<' || c == '>'; });
            // The last component of the spelled include (relative, '/'-separated) and of the target.
            const std::string named = spelled.substr(spelled.find_last_of("/\\") + 1);
            const std::string target = link.path.substr(link.path.find_last_of("/\\") + 1);
            EXPECT_EQ(kWindows ? lower(named) : named, kWindows ? lower(target) : target);
        }

        // Folding ranges: the model is clangd's answer.
        const Json rawFolds = raw("textDocument/foldingRange", Json::object().set("textDocument", identifier));
        const auto folds = run(fl::requestFoldingRanges(*service, file));
        ASSERT_TRUE(folds.has_value());
        ASSERT_EQ(folds->size(), rawFolds.size());
        ASSERT_FALSE(folds->empty());
        for (std::size_t i = 0; i < folds->size(); ++i) {
            EXPECT_EQ(static_cast<std::int64_t>((*folds)[i].startLine), intAt(rawFolds.items()[i], "startLine"));
            EXPECT_EQ(static_cast<std::int64_t>((*folds)[i].endLine), intAt(rawFolds.items()[i], "endLine"));
        }

        // AST at the first hint's argument: the same tree clangd sends.
        const fl::LspRange at{hints->front().position, hints->front().position};
        const Json rawAst =
            raw("textDocument/ast", Json::object().set("textDocument", identifier).set("range", fl::toJson(at)));
        const auto ast = run(fl::requestAst(*service, file, at));
        ASSERT_TRUE(ast.has_value()) << ast.error().describe();
        ASSERT_TRUE(ast->has_value());
        ASSERT_TRUE(rawAst.isObject());
        EXPECT_EQ((*ast)->kind, rawAst.stringAt("kind"));
        EXPECT_EQ(fl::astNodeCount(**ast), countNodes(rawAst));

        ASSERT_TRUE(service->closeDocument(file).has_value());
    }
    pool.spawn(frost::sched::Lane::UserVisible, service->stop()).join();
    EXPECT_EQ(service->state(), fl::ClangdState::Stopped);
    service.reset();
    (*reactor)->shutdown();
    pool.shutdown();
}

// `text` with every line's leading spaces and tabs removed.
std::string stripIndentation(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool lineStart = true;
    for (const char c : text) {
        if (lineStart && (c == ' ' || c == '\t')) {
            continue;
        }
        lineStart = c == '\n';
        out.push_back(c);
    }
    return out;
}

TEST(CorpusCppEditor, FormatOnSaveEqualsTheClangFormatCliOnFiveEngineFiles) {
    frost::test::disableCrashDialogs();
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());
    auto& fs = frost::io::realFileSystem();
    const auto style = fs.readFile(corpus + "/.clang-format");
    ASSERT_TRUE(style.has_value()) << "the clone has no .clang-format";
    const std::vector<std::string> files = {
        "Frost-Editor/src/EditorApp.cpp",
        "Frost/src/Frost/Core/Application.cpp",
        "Frost-Runtime/src/RuntimeApp.cpp",
        "Frost/src/Frost/Core/Application.h",
        "Frost-Editor/src/Editor/EditorApplication.h",
    };
    WorkDir dir("corpus-format-on-save");
    dir.write(".clang-format", *style);
    std::vector<std::string> copies;
    std::vector<fl::CompileCommand> commands;
    for (const std::string& relative : files) {
        auto original = fs.readFile(corpus + "/" + relative);
        ASSERT_TRUE(original.has_value()) << relative;
        const std::string name = QFileInfo(QString::fromStdString(relative)).fileName().toStdString();
        dir.write(name, stripIndentation(*original));
        dir.write("cli_" + name, stripIndentation(*original)); // The oracle's input, beside it.
        copies.push_back(name);
        if (!name.ends_with(".h")) {
            commands.push_back(fl::CompileCommand{.directory = dir.path(),
                                                  .file = dir.join(name),
                                                  .arguments = {"clang++", "-std=c++20", "-c", dir.join(name)}});
        }
    }
    ASSERT_TRUE(fl::writeCompileCommands(fs, dir.join("build"), commands).has_value());

    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_NE(rig.workbench->clangd(), nullptr);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::editorFormatOnSave, true));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->clangd()->state() == fl::ClangdState::Running; }, 120s));

    const std::string cli =
        frost::io::joinPath(frost::io::parentPath(clangdPath()), kWindows ? "clang-format.exe" : "clang-format")
            .value_or("");
    for (const std::string& name : copies) {
        SCOPED_TRACE(name);
        frost::io::ProcessSpec spec;
        spec.executable = cli;
        spec.arguments = {"-style=file", dir.join("cli_" + name)};
        spec.workingDirectory = dir.path();
        auto oracle = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 120s);
        ASSERT_TRUE(oracle.has_value()) << oracle.error().describe();
        ASSERT_EQ(oracle->exitCode, 0) << oracle->stderrBytes;
        const std::string stripped = fs.readFile(dir.join(name)).value_or("");
        ASSERT_NE(oracle->stdoutBytes, stripped) << "the CLI left the stripped text unchanged";

        ASSERT_TRUE(rig.workbench->openPath(dir.join(name)).has_value());
        frost::app::DocumentEditorView* view = rig.workbench->currentView();
        ASSERT_NE(view, nullptr);
        ASSERT_EQ(fl::documentKey(view->path()), fl::documentKey(dir.join(name)));
        ASSERT_TRUE(rig.workbench->save(*view).has_value());
        EXPECT_EQ(fs.readFile(dir.join(name)).value_or(""), oracle->stdoutBytes)
            << "format on save differs from the CLI";
        EXPECT_FALSE(view->document().dirty());
    }
}

} // namespace
