// SPDX-License-Identifier: Apache-2.0
//
// CPP-5 and REF-12 with the real clangd of the pinned LLVM (label "clangd"): document links,
// folding ranges, clangd's AST, inlay hints and signature help on a small source, each parsed by
// frost::lang and compared with clangd's own raw answer to the same request (read here with plain
// JSON access, not FrostIDE's parsers) and with what the source makes certain (the link's target
// is the header on disk, the AST node under `a + b` is clang's BinaryOperator). Then formatting:
// clangd's textDocument/formatting edits, and the save action built on them, give exactly the
// output of the pinned clang-format CLI (`clang-format -style=file`) on a badly formatted source
// with its own .clang-format; and the include-cleaner save action removes the unused include.
// Each test starts one clangd and stops it before returning.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/code_assist.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/document_features.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/save_actions.hpp"
#include "frost/lang/semantic_tokens.hpp"
#include "frost/lang/uri.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fl = frost::lang;
using fl::ClangdService;
using fl::ClangdState;
using fl::Json;
using namespace std::chrono_literals;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
constexpr auto kPatience = 60s; // clangd answers these in well under a second; this is slack.

template<class Condition>
bool waitFor(Condition condition, std::chrono::milliseconds timeout = kPatience) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// The reactor and pool a ClangdService runs on, shut down in the documented order.
class Env {
public:
    Env() {
        frost::test::disableCrashDialogs(); // clangd inherits the error mode: no WER box if it dies.
        auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-clangd-features"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<frost::sched::ThreadPool>(frost::sched::ThreadPoolOptions{.workerCount = 2});
    }
    ~Env() {
        if (m_reactor) {
            m_reactor->shutdown();
        }
        if (m_pool) {
            m_pool->shutdown();
        }
    }
    Env(const Env&) = delete ("owns the reactor");
    Env& operator=(const Env&) = delete ("owns the reactor");

    [[nodiscard]] bool ok() const { return m_reactor && m_pool; }
    [[nodiscard]] frost::io::AsyncContext context() { return frost::io::AsyncContext{*m_reactor, *m_pool}; }
    [[nodiscard]] frost::sched::ThreadPool& pool() { return *m_pool; }
    template<class T>
    T run(frost::sched::Task<T> task) {
        return m_pool->spawn(frost::sched::Lane::UserVisible, std::move(task)).join();
    }

private:
    std::unique_ptr<frost::io::Reactor> m_reactor;
    std::unique_ptr<frost::sched::ThreadPool> m_pool;
};

std::string clangdPath() {
    auto found = fl::findClangd(frost::io::realFileSystem(), fl::defaultClangdSearch());
    EXPECT_TRUE(found.has_value()) << found.error().describe();
    return found ? found->path : std::string();
}

std::string describe(ClangdService& service) {
    std::string out = std::string("state ") + std::string(fl::toString(service.state()));
    if (auto error = service.lastError()) {
        out += "; last error: " + error->describe();
    }
    for (const std::string& line : service.recentStderr()) {
        out += "\n  " + line;
    }
    return out;
}

// A service on `env` for `root` with the compile_commands.json under <root>/build.
std::unique_ptr<ClangdService> startService(Env& env, const std::string& root) {
    fl::ClangdServiceOptions options;
    options.clangdPath = clangdPath();
    options.workspaceRoot = root;
    options.launch.jobs = 2;
    // No index files are written into the scratch directory (these features need only the AST),
    // so nothing clangd still holds can stop the directory's removal.
    options.launch.backgroundIndex = false;
    options.compileCommands = std::make_shared<fl::ExistingCompileCommandsProvider>(frost::io::realFileSystem(), root);
    auto service = std::make_unique<ClangdService>(frost::io::realProcessLauncher(), env.context(), std::move(options));
    auto started = service->start();
    EXPECT_TRUE(started.has_value()) << started.error().describe();
    EXPECT_TRUE(
        waitFor([&] { return service->state() == ClangdState::Running || service->state() == ClangdState::Failed; }));
    return service;
}

void stopAndCheck(Env& env, ClangdService& service) {
    env.run(service.stop());
    EXPECT_EQ(service.state(), ClangdState::Stopped);
}

frost::text::Document documentOf(std::string_view text) {
    auto document = frost::text::Document::fromUtf8(std::string(text));
    EXPECT_TRUE(document.has_value());
    return std::move(*document);
}

// The LSP position (UTF-8 columns; the sources are ASCII) of `marker` + `offset` in `text`.
fl::LspPosition positionIn(std::string_view text, std::string_view marker, std::size_t offset = 0) {
    const std::size_t at = text.find(marker);
    EXPECT_NE(at, std::string_view::npos) << marker;
    const std::string_view before = text.substr(0, at + offset);
    const auto line = static_cast<std::uint32_t>(std::ranges::count(before, '\n'));
    const std::size_t lineStart = before.rfind('\n') == std::string_view::npos ? 0 : before.rfind('\n') + 1;
    return fl::LspPosition{line, static_cast<std::uint32_t>(before.size() - lineStart)};
}

// Raw JSON access for the oracles: an integer member, -1 when missing.
std::int64_t intAt(const Json& object, std::string_view key) { return object.integerAt(key).value_or(-1); }
fl::LspPosition rawPosition(const Json& position) {
    return {static_cast<std::uint32_t>(intAt(position, "line")),
            static_cast<std::uint32_t>(intAt(position, "character"))};
}

// The compile_commands.json for `sources` under <dir>/build.
void writeDatabase(const frost::test::ScratchDir& dir, const std::vector<std::string>& sources) {
    std::vector<fl::CompileCommand> commands;
    for (const std::string& source : sources) {
        commands.push_back(fl::CompileCommand{.directory = dir.path(),
                                              .file = source,
                                              .arguments = {"clang++", "-std=c++20", "-I" + dir.path(), "-c", source}});
    }
    ASSERT_TRUE(fl::writeCompileCommands(frost::io::realFileSystem(), dir.join("build"), commands).has_value());
}

constexpr std::string_view kHeader = "#pragma once\n"
                                     "int add(int a, int b);\n"
                                     "struct Point {\n"
                                     "    int x;\n"
                                     "    int y;\n"
                                     "};\n";

constexpr std::string_view kSource = "#include \"widgets.h\"\n"
                                     "\n"
                                     "/*\n"
                                     " * A block comment\n"
                                     " * over four lines.\n"
                                     " */\n"
                                     "int add(int a, int b) {\n"
                                     "    return a + b;\n"
                                     "}\n"
                                     "\n"
                                     "int use() {\n"
                                     "    auto total = add(1, 2);\n"
                                     "    Point p{3, 4};\n"
                                     "    return total + p.x;\n"
                                     "}\n";

TEST(ClangdE2E, LinksFoldingAstInlayHintsAndSignatureHelpMatchClangdsAnswers) {
    frost::test::ScratchDir scratch("lang-clangd-features");
    auto& fs = frost::io::realFileSystem();
    const std::string header = scratch.join("widgets.h");
    const std::string source = scratch.join("features.cpp");
    ASSERT_TRUE(fs.writeFileAtomic(header, kHeader).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(source, kSource).has_value());
    writeDatabase(scratch, {source});
    Env env;
    ASSERT_TRUE(env.ok());
    auto service = startService(env, scratch.path());
    ASSERT_EQ(service->state(), ClangdState::Running) << describe(*service);
    const fl::ServerCapabilities caps = *service->capabilities();
    EXPECT_TRUE(caps.documentLink);
    EXPECT_TRUE(caps.foldingRange);
    EXPECT_TRUE(caps.ast);
    EXPECT_TRUE(caps.inlayHint);
    EXPECT_TRUE(caps.signatureHelp);
    frost::text::Document document = documentOf(kSource);
    ASSERT_TRUE(service->openDocument(source, document.text()).has_value());
    // The file is clean: clangd's (empty) publish leaves no entry in the store but records its version.
    ASSERT_TRUE(waitFor([&] { return service->diagnosedVersion(source) == service->documentVersion(source); }))
        << describe(*service);
    EXPECT_EQ(service->diagnosedVersion(source), std::optional<std::int64_t>(1));
    EXPECT_FALSE(service->diagnostics().forFile(source).has_value());
    EXPECT_FALSE(service->diagnosedVersion(scratch.join("not-open.cpp")).has_value());
    const Json identifier = fl::textDocumentIdentifier(source).value();
    const auto raw = [&](std::string method, Json params) {
        auto answer = env.run(service->request(std::move(method), std::move(params)));
        EXPECT_TRUE(answer.has_value()) << answer.error().describe();
        return answer.value_or(Json());
    };

    // Document links: the model equals clangd's answer, and the one link is the header on disk.
    const Json rawLinks = raw("textDocument/documentLink", Json::object().set("textDocument", identifier));
    const auto links = env.run(fl::requestDocumentLinks(*service, source));
    ASSERT_TRUE(links.has_value()) << links.error().describe();
    ASSERT_EQ(links->size(), rawLinks.size());
    ASSERT_EQ(links->size(), 1U);
    EXPECT_EQ(links->front().uri, rawLinks.items()[0].stringAt("target"));
    EXPECT_EQ(fl::documentKey(links->front().path), fl::documentKey(header));
    EXPECT_EQ(links->front().range.start, rawPosition(*rawLinks.items()[0].find("range")->find("start")));
    EXPECT_EQ(links->front().range.start, positionIn(kSource, "\"widgets.h\""));

    // Folding ranges: the model equals clangd's answer, and add's body folds from its first line.
    const Json rawFolds = raw("textDocument/foldingRange", Json::object().set("textDocument", identifier));
    const auto folds = env.run(fl::requestFoldingRanges(*service, source));
    ASSERT_TRUE(folds.has_value()) << folds.error().describe();
    ASSERT_EQ(folds->size(), rawFolds.size());
    for (std::size_t i = 0; i < folds->size(); ++i) {
        const Json& item = rawFolds.items()[i];
        EXPECT_EQ(static_cast<std::int64_t>((*folds)[i].startLine), intAt(item, "startLine"));
        EXPECT_EQ(static_cast<std::int64_t>((*folds)[i].endLine), intAt(item, "endLine"));
        EXPECT_EQ((*folds)[i].startCharacter.value_or(UINT32_MAX),
                  item.find("startCharacter") ? static_cast<std::uint32_t>(intAt(item, "startCharacter")) : UINT32_MAX);
    }
    const std::uint32_t addLine = positionIn(kSource, "int add(").line;
    EXPECT_TRUE(std::ranges::any_of(*folds, [&](const fl::FoldingRange& f) {
        return f.startLine == addLine && f.endLine > addLine;
    })) << "no fold for add's body";

    // AST: the node under `a + b` is clang's BinaryOperator with both operands.
    const fl::LspPosition plus = positionIn(kSource, "a + b", 2);
    const fl::LspRange plusRange{plus, plus};
    const Json rawAst =
        raw("textDocument/ast", Json::object().set("textDocument", identifier).set("range", fl::toJson(plusRange)));
    const auto ast = env.run(fl::requestAst(*service, source, plusRange));
    ASSERT_TRUE(ast.has_value()) << ast.error().describe();
    ASSERT_TRUE(ast->has_value());
    EXPECT_EQ((*ast)->kind, rawAst.stringAt("kind"));
    EXPECT_EQ((*ast)->kind, "BinaryOperator");
    EXPECT_EQ((*ast)->detail, "+");
    EXPECT_EQ((*ast)->children.size(), rawAst.find("children")->size());
    EXPECT_EQ((*ast)->children.size(), 2U);
    EXPECT_EQ((*ast)->range, fl::parseRange(*rawAst.find("range")));

    // Inlay hints: the model's labels and positions are clangd's; add(1, 2) gets a: and b:, and
    // `auto total` its deduced type.
    const std::uint32_t lines = static_cast<std::uint32_t>(std::ranges::count(kSource, '\n'));
    const fl::LspRange whole{{0, 0}, {lines, 0}};
    const Json rawHints =
        raw("textDocument/inlayHint", Json::object().set("textDocument", identifier).set("range", fl::toJson(whole)));
    const auto hints = env.run(fl::requestInlayHints(*service, source, whole));
    ASSERT_TRUE(hints.has_value()) << hints.error().describe();
    ASSERT_EQ(hints->size(), rawHints.size());
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
        EXPECT_EQ((*hints)[i].label, label);
        EXPECT_EQ((*hints)[i].position, rawPosition(*item.find("position")));
    }
    const auto hintAt = [&](fl::LspPosition position) -> std::string {
        for (const fl::LspInlayHint& hint : *hints) {
            if (hint.position == position) {
                return hint.label;
            }
        }
        return {};
    };
    EXPECT_EQ(hintAt(positionIn(kSource, "add(1, 2)", 4)), "a:");
    EXPECT_EQ(hintAt(positionIn(kSource, "add(1, 2)", 7)), "b:");
    EXPECT_TRUE(std::ranges::any_of(*hints, [](const fl::LspInlayHint& hint) {
        return hint.kind == fl::LspInlayHintKind::Type && hint.label.find("int") != std::string::npos;
    }));

    // Signature help inside add(1, 2): the declaration's label, the parameter under the caret.
    for (const auto& [offset, parameter] : {std::pair<std::size_t, std::uint32_t>{4, 0}, {7, 1}}) {
        const fl::LspPosition at = positionIn(kSource, "add(1, 2)", offset);
        const Json rawHelp = raw("textDocument/signatureHelp", fl::textDocumentPosition(source, at).value());
        const auto help = env.run(fl::requestSignatureHelp(*service, source, at, std::nullopt, false));
        ASSERT_TRUE(help.has_value()) << help.error().describe();
        ASSERT_TRUE(help->has_value());
        ASSERT_EQ((*help)->signatures.size(), rawHelp.find("signatures")->size());
        EXPECT_EQ((*help)->signatures.front().label, rawHelp.find("signatures")->items()[0].stringAt("label"));
        EXPECT_NE((*help)->signatures.front().label.find("int a, int b"), std::string::npos);
        EXPECT_EQ((*help)->activeParameterOf((*help)->activeSignature), parameter);
    }

    ASSERT_TRUE(service->closeDocument(source).has_value());
    stopAndCheck(env, *service);
}

// --- Formatting (REF-12) ---

// A style unlike clang-format's default, so formatting must have read the file.
constexpr std::string_view kStyle = "BasedOnStyle: LLVM\n"
                                    "IndentWidth: 3\n"
                                    "BreakBeforeBraces: Allman\n"
                                    "PointerAlignment: Left\n"
                                    "ColumnLimit: 80\n";

constexpr std::string_view kMessy = "#include \"unused.h\"\n"
                                    "#include \"widgets.h\"\n"
                                    "int   scale( int *v,int n ){ int total=0;\n"
                                    "  for(int i=0;i<n;++i){total+=v[i]*add(i,  n);}\n"
                                    "      return total;}\n";

// The pinned clang-format CLI (beside clangd) on `file`, with `extra` arguments: its standard output.
std::string clangFormatCli(const std::string& file, std::vector<std::string> extra = {}) {
    const std::string cli =
        frost::io::joinPath(frost::io::parentPath(clangdPath()), kWindows ? "clang-format.exe" : "clang-format")
            .value_or("");
    frost::io::ProcessSpec spec;
    spec.executable = cli;
    spec.arguments = {"-style=file"};
    spec.arguments.insert(spec.arguments.end(), extra.begin(), extra.end());
    spec.arguments.push_back(file);
    spec.workingDirectory = std::string(frost::io::parentPath(file));
    auto run = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 60s);
    EXPECT_TRUE(run.has_value()) << run.error().describe();
    if (!run) {
        return {};
    }
    EXPECT_EQ(run->exitCode, 0) << run->stderrBytes;
    return run->stdoutBytes;
}

// `text` with byte edits applied (sorted ascending, as toByteEdits returns them).
std::string applyBytes(std::string text, const std::vector<fl::ByteEdit>& edits) {
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        text.replace(it->range.begin.value(), it->range.size(), it->text);
    }
    return text;
}

TEST(ClangdE2E, FormattingAndTheSaveActionsEqualTheClangFormatCliAndRemoveTheUnusedInclude) {
    frost::test::ScratchDir scratch("lang-clangd-format");
    auto& fs = frost::io::realFileSystem();
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join(".clang-format"), kStyle).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("widgets.h"), kHeader).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("unused.h"), "#pragma once\nint unusedHelper();\n").has_value());
    const std::string source = scratch.join("messy.cpp");
    ASSERT_TRUE(fs.writeFileAtomic(source, kMessy).has_value());
    // The oracle: the CLI on a copy beside it (same directory, so the same .clang-format).
    const std::string copy = scratch.join("messy_cli.cpp");
    ASSERT_TRUE(fs.writeFileAtomic(copy, kMessy).has_value());
    const std::string expected = clangFormatCli(copy);
    ASSERT_FALSE(expected.empty());
    ASSERT_NE(expected, kMessy);
    ASSERT_NE(expected.find("\n{\n   int total = 0;"), std::string::npos) << "the CLI did not apply the style:\n"
                                                                          << expected;
    writeDatabase(scratch, {source});
    Env env;
    ASSERT_TRUE(env.ok());
    auto service = startService(env, scratch.path());
    ASSERT_EQ(service->state(), ClangdState::Running) << describe(*service);
    frost::text::Document document = documentOf(kMessy);
    ASSERT_TRUE(service->openDocument(source, document.text()).has_value());

    // textDocument/formatting through the model, applied to the text.
    const auto edits = env.run(fl::requestFormatting(*service, source, std::nullopt, fl::FormattingOptions{}));
    ASSERT_TRUE(edits.has_value()) << edits.error().describe();
    const auto formatted = fl::applyTextEdits(kMessy, *edits, service->encoding());
    ASSERT_TRUE(formatted.has_value()) << formatted.error().describe();
    EXPECT_EQ(*formatted, expected);

    // The save action, waited for as the workbench waits.
    const auto saveEdits = fl::waitForResult<std::vector<fl::ByteEdit>>(
        env.pool(),
        [&](std::stop_token token) {
            return fl::formatEditsForSave(*service, source, document.text(), fl::FormattingOptions{}, std::move(token));
        },
        30s);
    ASSERT_TRUE(saveEdits.has_value()) << saveEdits.error().describe();
    EXPECT_EQ(applyBytes(std::string(kMessy), *saveEdits), expected);

    // rangeFormatting of the function's first line equals the CLI on the same bytes (--offset, --length).
    const std::size_t rangeBegin = kMessy.find("int   scale");
    const fl::LspRange first{positionIn(kMessy, "int   scale"), positionIn(kMessy, "int   scale", 10)};
    const auto ranged = env.run(fl::requestFormatting(*service, source, first, fl::FormattingOptions{}));
    ASSERT_TRUE(ranged.has_value()) << ranged.error().describe();
    const auto partly = fl::applyTextEdits(kMessy, *ranged, service->encoding());
    ASSERT_TRUE(partly.has_value());
    EXPECT_NE(*partly, kMessy);
    EXPECT_EQ(*partly, clangFormatCli(copy, {std::format("--offset={}", rangeBegin), "--length=10"}));

    // Optimize includes on save: once clangd reports the unused include, its fix removes it.
    std::vector<fl::LiveDiagnostic> diagnostics;
    ASSERT_TRUE(waitFor([&] {
        const auto set = service->diagnostics().forFile(source);
        if (!set) {
            return false;
        }
        diagnostics = set->diagnostics;
        return std::ranges::any_of(diagnostics,
                                   [](const fl::LiveDiagnostic& d) { return fl::isIncludeCleanerDiagnostic(d); });
    })) << describe(*service);
    const auto cleaner = fl::waitForResult<std::vector<fl::ByteEdit>>(
        env.pool(),
        [&](std::stop_token token) {
            return fl::includeCleanerEditsForSave(*service, source, document.text(), diagnostics, std::move(token));
        },
        30s);
    ASSERT_TRUE(cleaner.has_value()) << cleaner.error().describe();
    const std::string cleaned = applyBytes(std::string(kMessy), *cleaner);
    EXPECT_EQ(cleaned.find("unused.h"), std::string::npos) << cleaned;
    EXPECT_NE(cleaned.find("#include \"widgets.h\""), std::string::npos) << cleaned;

    ASSERT_TRUE(service->closeDocument(source).has_value());
    stopAndCheck(env, *service);
}

} // namespace
