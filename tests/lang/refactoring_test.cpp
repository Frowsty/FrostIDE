// SPDX-License-Identifier: Apache-2.0
//
// End-to-end tests for REF-3 (clangd quick fixes & tweaks via the normal LSP command path) and
// REF-11 (include management: unused-include removal + missing-include insertion via
// requestOptimizeIncludes) using the real clangd and real clang++ compiler.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"
#include "frost/text/text_buffer.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fl = frost::lang;
namespace fio = frost::io;
using namespace std::chrono_literals;

constexpr bool kWindows = fio::kNativePathStyle == fio::PathStyle::Windows;
constexpr auto kPatience = 60s;

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

class RefactorEnv {
public:
    RefactorEnv() {
        frost::test::disableCrashDialogs();
        auto reactor = fio::Reactor::create(fio::ReactorOptions{1, "frost-test-refactoring"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<frost::sched::ThreadPool>(frost::sched::ThreadPoolOptions{.workerCount = 2});
    }
    ~RefactorEnv() {
        if (m_reactor) {
            m_reactor->shutdown();
        }
        if (m_pool) {
            m_pool->shutdown();
        }
    }
    RefactorEnv(const RefactorEnv&) = delete;
    RefactorEnv& operator=(const RefactorEnv&) = delete;

    [[nodiscard]] bool ok() const { return m_reactor && m_pool; }
    [[nodiscard]] fio::AsyncContext context() { return fio::AsyncContext{*m_reactor, *m_pool}; }

    template<class T>
    T run(frost::sched::Task<T> task) {
        return m_pool->spawn(frost::sched::Lane::UserVisible, std::move(task)).join();
    }

private:
    std::unique_ptr<fio::Reactor> m_reactor;
    std::unique_ptr<frost::sched::ThreadPool> m_pool;
};

[[nodiscard]] std::string clangdPath() {
    auto found = fl::findClangd(fio::realFileSystem(), fl::defaultClangdSearch());
    EXPECT_TRUE(found.has_value()) << found.error().describe();
    return found ? found->path : std::string();
}

[[nodiscard]] std::string describeService(fl::ClangdService& service) {
    std::string out = std::string("state ") + std::string(fl::toString(service.state()));
    if (auto error = service.lastError()) {
        out += "; last error: " + error->describe();
    }
    for (const std::string& line : service.recentStderr()) {
        out += "\n  " + line;
    }
    return out;
}

[[nodiscard]] std::unique_ptr<fl::ClangdService> startClangd(RefactorEnv& env, const std::string& root,
                                                             bool trusted = true) {
    fl::ClangdServiceOptions options;
    options.clangdPath = clangdPath();
    options.workspaceRoot = root;
    options.launch.jobs = 2;
    options.launch.backgroundIndex = false;
    options.launch.trusted = trusted;
    options.compileCommands = std::make_shared<fl::ExistingCompileCommandsProvider>(fio::realFileSystem(), root);
    auto service = std::make_unique<fl::ClangdService>(fio::realProcessLauncher(), env.context(), std::move(options));
    auto started = service->start();
    EXPECT_TRUE(started.has_value()) << started.error().describe();
    EXPECT_TRUE(waitFor(
        [&] { return service->state() == fl::ClangdState::Running || service->state() == fl::ClangdState::Failed; }));
    return service;
}

void stopClangd(RefactorEnv& env, fl::ClangdService& service) {
    static_cast<void>(env.run(service.stop()));
    EXPECT_EQ(service.state(), fl::ClangdState::Stopped);
}

[[nodiscard]] fl::LspPosition positionIn(std::string_view text, std::string_view marker, std::size_t offset = 0) {
    const std::size_t at = text.find(marker);
    EXPECT_NE(at, std::string_view::npos) << marker;
    const std::string_view before = text.substr(0, at + offset);
    const auto line = static_cast<std::uint32_t>(std::ranges::count(before, '\n'));
    const std::size_t lineStart = before.rfind('\n') == std::string_view::npos ? 0 : before.rfind('\n') + 1;
    return fl::LspPosition{line, static_cast<std::uint32_t>(before.size() - lineStart)};
}

[[nodiscard]] ::testing::AssertionResult compileWithRealCompiler(const std::string& workDir,
                                                                 const std::string& filePath) {
    const std::string compiler =
        fio::joinPath(fio::parentPath(clangdPath()), kWindows ? "clang++.exe" : "clang++").value_or("");
    fio::ProcessSpec spec;
    spec.executable = compiler;
    spec.arguments = {"-fsyntax-only", "-std=c++20", "-I" + workDir, filePath};
    spec.workingDirectory = workDir;
    auto result = fio::runToCompletion(fio::realProcessLauncher(), spec, {}, 60s);
    if (!result) {
        return ::testing::AssertionFailure() << result.error().describe();
    }
    if (result->exitCode != 0) {
        return ::testing::AssertionFailure() << "clang++ exited with " << result->exitCode << ":\n"
                                             << result->stderrBytes;
    }
    return ::testing::AssertionSuccess();
}

// Closed-file store so WorkspaceEditApplier updates the file on disk atomically.
class DiskOnlyDocuments final : public fl::IWorkspaceDocuments {
public:
    std::optional<fl::OpenDocumentState> find(std::string_view) override { return std::nullopt; }
    frost::Result<void> applyEdits(std::string_view, std::span<const fl::ByteEdit>, std::string_view) override {
        return frost::fail(frost::ErrorCode::Internal,
                           "disk-only workspace documents cannot apply open-document edits");
    }
};

TEST(RefactoringTest, Ref3QuickFixAndClangdTweakApplyAndCompile) {
    frost::test::ScratchDir scratch("ref3-code-actions");
    auto& fs = fio::realFileSystem();

    // 1. A file with a missing semicolon diagnostic where clangd offers a direct quickfix edit,
    //    plus an expression where clangd offers the ExtractVariable tweak via workspace/applyEdit.
    const std::string quickFixFile = scratch.join("quickfix.cpp");
    constexpr std::string_view kBrokenSource = "int square(int x) {\n"
                                               "    return x * x\n"
                                               "}\n";
    ASSERT_TRUE(fs.writeFileAtomic(quickFixFile, kBrokenSource).has_value());

    const std::string tweakFile = scratch.join("tweak.cpp");
    constexpr std::string_view kTweakSource = "int compute(int a, int b) {\n"
                                              "    int total = a * b + 10;\n"
                                              "    return total;\n"
                                              "}\n";
    ASSERT_TRUE(fs.writeFileAtomic(tweakFile, kTweakSource).has_value());

    const std::vector<fl::CompileCommand> commands = {
        {.directory = scratch.path(),
         .file = quickFixFile,
         .arguments = {"clang++", "-std=c++20", "-I" + scratch.path(), "-c", quickFixFile}},
        {.directory = scratch.path(),
         .file = tweakFile,
         .arguments = {"clang++", "-std=c++20", "-I" + scratch.path(), "-c", tweakFile}},
    };
    ASSERT_TRUE(fl::writeCompileCommands(fs, scratch.join("build"), commands).has_value());

    RefactorEnv env;
    ASSERT_TRUE(env.ok());
    auto service = startClangd(env, scratch.path());
    ASSERT_EQ(service->state(), fl::ClangdState::Running) << describeService(*service);

    DiskOnlyDocuments docs;
    fl::WorkspaceEditApplier applier(docs, fs, service->encoding());

    // Part A: Apply clangd's quick fix on `quickFixFile` via requestApplyCodeAction and compile
    auto quickFixBuffer = frost::text::TextBuffer::fromUtf8(std::string(kBrokenSource));
    ASSERT_TRUE(quickFixBuffer.has_value());
    ASSERT_TRUE(service->openDocument(quickFixFile, quickFixBuffer->snapshot()).has_value());
    std::vector<fl::LiveDiagnostic> diags;
    ASSERT_TRUE(waitFor([&] {
        if (const auto set = service->diagnostics().forFile(quickFixFile)) {
            diags = set->diagnostics;
            return !diags.empty();
        }
        return false;
    })) << describeService(*service);

    const fl::LiveDiagnostic& errDiag = diags.front();
    auto fixActions = env.run(fl::requestCodeActions(*service, quickFixFile, errDiag.lspRange, {errDiag}));
    ASSERT_TRUE(fixActions.has_value()) << fixActions.error().describe();
    ASSERT_FALSE(fixActions->empty());

    ASSERT_FALSE(fixActions->front().edit.empty());
    ASSERT_TRUE(applier.apply(fixActions->front().edit, fixActions->front().title).has_value());

    auto fixedContent = fs.readFile(quickFixFile);
    ASSERT_TRUE(fixedContent.has_value());
    EXPECT_NE(fixedContent->find("return x * x;"), std::string::npos) << *fixedContent;
    EXPECT_TRUE(compileWithRealCompiler(scratch.path(), quickFixFile));
    ASSERT_TRUE(service->closeDocument(quickFixFile).has_value());

    // Part B: Apply clangd's ExtractVariable tweak through the same executeCommand and
    // workspace/applyEdit callback path used by the editor.
    auto tweakBuffer = frost::text::TextBuffer::fromUtf8(std::string(kTweakSource));
    ASSERT_TRUE(tweakBuffer.has_value());
    ASSERT_TRUE(service->openDocument(tweakFile, tweakBuffer->snapshot()).has_value());
    // Empty diagnostics remove the file from DiagnosticsStore. Wait for clangd's
    // versioned diagnostic publication so a clean file still proves AST readiness.
    const auto tweakVersion = service->documentVersion(tweakFile);
    ASSERT_TRUE(tweakVersion.has_value());
    ASSERT_TRUE(waitFor([&] { return service->diagnosedVersion(tweakFile) == tweakVersion; }))
        << describeService(*service);

    const fl::LspRange exprRange{positionIn(kTweakSource, "a * b"), positionIn(kTweakSource, "a * b", 5)};
    auto tweakActions = env.run(fl::requestCodeActions(*service, tweakFile, exprRange, {}));
    ASSERT_TRUE(tweakActions.has_value()) << tweakActions.error().describe();

    const fl::CodeActionItem* extractVar = fl::findTweak(*tweakActions, "ExtractVariable");
    ASSERT_NE(extractVar, nullptr);
    EXPECT_TRUE(extractVar->edit.empty());
    EXPECT_EQ(extractVar->commandName, fl::kClangdApplyTweak);

    bool serverEditApplied = false;
    service->setApplyEditHandler([&](const fl::Json& params, const std::function<void(bool)>& done) {
        const fl::Json* editJson = params.find("edit");
        auto edit = fl::parseWorkspaceEdit(editJson != nullptr ? *editJson : params);
        if (!edit || edit->empty()) {
            done(false);
            return;
        }
        auto applied = applier.apply(*edit, params.stringAt("label"));
        serverEditApplied = applied.has_value();
        done(serverEditApplied);
    });
    auto executed = env.run(fl::requestExecuteCommand(*service, extractVar->commandName, extractVar->commandArguments));
    ASSERT_TRUE(executed.has_value()) << executed.error().describe();
    EXPECT_TRUE(serverEditApplied);

    auto tweakedContent = fs.readFile(tweakFile);
    ASSERT_TRUE(tweakedContent.has_value());
    EXPECT_NE(tweakedContent->find("placeholder = a * b;"), std::string::npos) << *tweakedContent;
    EXPECT_TRUE(compileWithRealCompiler(scratch.path(), tweakFile));

    ASSERT_TRUE(service->closeDocument(tweakFile).has_value());
    stopClangd(env, *service);
}

TEST(RefactoringTest, Ref11OptimizeIncludesRemovesUnusedAndAddsMissingIncludeAndCompiles) {
    frost::test::ScratchDir scratch("ref11-optimize-includes");
    auto& fs = fio::realFileSystem();

    constexpr std::string_view kClangdConfig = "Diagnostics:\n"
                                               "  UnusedIncludes: Strict\n"
                                               "  MissingIncludes: Strict\n";
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join(".clangd"), kClangdConfig).has_value());

    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("unused_dep.h"), "#pragma once\nint unusedDep();\n").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("direct_type.h"), "#pragma once\nstruct DirectVal {\n    int v;\n};\n")
                    .has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("umbrella_dep.h"),
                                   "#pragma once\n#include \"direct_type.h\"\ninline int "
                                   "umbrellaStep() { return 1; }\n")
                    .has_value());

    const std::string source = scratch.join("optimize_target.cpp");
    constexpr std::string_view kSource = "#include \"unused_dep.h\"\n"
                                         "#include \"umbrella_dep.h\"\n"
                                         "\n"
                                         "int computeValue(DirectVal item) {\n"
                                         "    return umbrellaStep() + item.v;\n"
                                         "}\n";
    ASSERT_TRUE(fs.writeFileAtomic(source, kSource).has_value());

    const std::vector<fl::CompileCommand> commands = {
        {.directory = scratch.path(),
         .file = source,
         .arguments = {"clang++", "-std=c++20", "-I" + scratch.path(), "-c", source}},
    };
    ASSERT_TRUE(fl::writeCompileCommands(fs, scratch.join("build"), commands).has_value());

    RefactorEnv env;
    ASSERT_TRUE(env.ok());
    auto service = startClangd(env, scratch.path(), /*trusted=*/true);
    ASSERT_EQ(service->state(), fl::ClangdState::Running) << describeService(*service);

    auto sourceBuffer = frost::text::TextBuffer::fromUtf8(std::string(kSource));
    ASSERT_TRUE(sourceBuffer.has_value());
    ASSERT_TRUE(service->openDocument(source, sourceBuffer->snapshot()).has_value());

    std::vector<fl::LiveDiagnostic> diagnostics;
    ASSERT_TRUE(waitFor([&] {
        const auto set = service->diagnostics().forFile(source);
        if (!set) {
            return false;
        }
        diagnostics = set->diagnostics;
        const bool hasUnused =
            std::ranges::any_of(diagnostics, [](const fl::LiveDiagnostic& d) { return d.code == "unused-includes"; });
        const bool hasMissing =
            std::ranges::any_of(diagnostics, [](const fl::LiveDiagnostic& d) { return d.code == "missing-includes"; });
        return hasUnused && hasMissing;
    })) << describeService(*service);

    auto optimizedEdit = env.run(fl::requestOptimizeIncludes(*service, source, diagnostics));
    ASSERT_TRUE(optimizedEdit.has_value()) << optimizedEdit.error().describe();
    ASSERT_FALSE(optimizedEdit->empty());

    DiskOnlyDocuments docs;
    fl::WorkspaceEditApplier applier(docs, fs, service->encoding());
    auto applied = applier.apply(*optimizedEdit, "Optimize Includes");
    ASSERT_TRUE(applied.has_value()) << applied.error().describe();

    auto updatedText = fs.readFile(source);
    ASSERT_TRUE(updatedText.has_value());
    EXPECT_EQ(updatedText->find("unused_dep.h"), std::string::npos) << *updatedText;
    EXPECT_NE(updatedText->find("direct_type.h"), std::string::npos) << *updatedText;
    EXPECT_NE(updatedText->find("umbrella_dep.h"), std::string::npos) << *updatedText;

    EXPECT_TRUE(compileWithRealCompiler(scratch.path(), source));

    ASSERT_TRUE(service->closeDocument(source).has_value());
    stopClangd(env, *service);
}

} // namespace
