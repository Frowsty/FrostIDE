// SPDX-License-Identifier: Apache-2.0
//
// CPP-9 corpus acceptance: Code Vision usage counts for ten named FrostEngine symbols equal the
// direct references answers from the pinned clangd after its background index completes.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/code_vision.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "support/crash_dialogs.hpp"
#include "support/engine_lock.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef FROST_CORPUS_FROSTENGINE_SHA
#error "FROST_CORPUS_FROSTENGINE_SHA must come from the pinned corpora lock"
#endif
#ifndef FROST_CORPUS_FROSTENGINE_PRESET
#error "FROST_CORPUS_FROSTENGINE_PRESET must come from the pinned corpora lock"
#endif

namespace {

namespace fl = frost::lang;
using namespace std::chrono_literals;

struct EngineSymbol {
    std::string_view name;
    bool inHeader;
    std::string_view marker;
    std::size_t offset;
    fl::SymbolKind kind;
};

constexpr EngineSymbol kSymbols[] = {
    {"Frost::FrameTimeSource", true, "class FrameTimeSource", 6, fl::SymbolKind::Class},
    {"Frost::FramePacing", true, "enum class FramePacing", 11, fl::SymbolKind::Enum},
    {"Frost::ApplicationSpecification", true, "struct ApplicationSpecification", 7, fl::SymbolKind::Struct},
    {"Frost::ImGuiFrameHook", true, "class ImGuiFrameHook", 6, fl::SymbolKind::Class},
    {"Frost::FrameLimiter", true, "class FrameLimiter", 6, fl::SymbolKind::Class},
    {"Frost::FrameLimiter::WaitUntil", true, "void WaitUntil(", 5, fl::SymbolKind::Method},
    {"Frost::Application", true, "class Application", 6, fl::SymbolKind::Class},
    {"Frost::Application::Run", true, "void Run();", 5, fl::SymbolKind::Method},
    {"Frost::Application::PushLayer", true, "Layer* PushLayer(", 7, fl::SymbolKind::Method},
    {"Frost::SystemFrameTimeSource", false, "class SystemFrameTimeSource", 6, fl::SymbolKind::Class},
};

template<class Condition>
bool waitUntil(Condition condition, std::chrono::milliseconds timeout = 60s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(25ms);
    }
    return true;
}

class ClangdEnv {
public:
    ClangdEnv() {
        frost::test::disableCrashDialogs();
        auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-code-vision-corpus"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<frost::sched::ThreadPool>(frost::sched::ThreadPoolOptions{.workerCount = 4});
    }

    ~ClangdEnv() {
        if (m_reactor) {
            m_reactor->shutdown();
        }
        if (m_pool) {
            m_pool->shutdown();
        }
    }

    [[nodiscard]] bool ok() const { return m_reactor && m_pool; }
    [[nodiscard]] frost::io::AsyncContext context() { return {*m_reactor, *m_pool}; }

    template<class T>
    T run(frost::sched::Task<T> task) {
        return m_pool->spawn(frost::sched::Lane::UserVisible, std::move(task)).join();
    }

private:
    std::unique_ptr<frost::io::Reactor> m_reactor;
    std::unique_ptr<frost::sched::ThreadPool> m_pool;
};

class FixedCompileCommandsProvider final : public fl::ICompileCommandsProvider {
public:
    explicit FixedCompileCommandsProvider(fl::CompileCommandsLocation location) : m_location(std::move(location)) {}
    frost::Result<std::optional<fl::CompileCommandsLocation>> locate() override { return m_location; }

private:
    fl::CompileCommandsLocation m_location;
};

std::string pinnedCorpus() {
    const std::string root = frost::test::environment("FROST_CORPORA_ROOT");
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is unset; run the corpus verification tier";
        return {};
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    std::string path = frost::io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
    auto head = frost::io::realFileSystem().readFile(path + "/.git/HEAD");
    if (!head || head->substr(0, 40) != sha) {
        ADD_FAILURE() << path << " is not checked out at pinned FrostEngine commit " << sha;
        return {};
    }
    return path;
}

fl::LspPosition positionOf(std::string_view source, std::string_view marker, std::size_t offset) {
    const std::size_t at = source.find(marker);
    EXPECT_NE(at, std::string_view::npos) << marker;
    if (at == std::string_view::npos) {
        return {};
    }
    const std::string_view before = source.substr(0, at + offset);
    const std::size_t lineStart = before.rfind('\n') == std::string_view::npos ? 0 : before.rfind('\n') + 1;
    return {static_cast<std::uint32_t>(std::ranges::count(before, '\n')),
            static_cast<std::uint32_t>(before.size() - lineStart)};
}

std::size_t responseCount(const fl::Json& response) { return response.isArray() ? response.size() : 0U; }

TEST(CodeVisionCorpus, TenFrostEngineUsageCountsMatchPinnedClangdReferenceOracle) {
    const std::string corpus = pinnedCorpus();
    ASSERT_FALSE(corpus.empty());
    const std::string root = frost::test::environment("FROST_CORPORA_ROOT");
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    frost::test::EngineBuildLock buildLock(root + "/frostengine-" + sha.substr(0, 12) + ".build.lock");
    ASSERT_TRUE(buildLock.held()) << "could not acquire the pinned corpus build lock";

    auto& fs = frost::io::realFileSystem();
    const std::string buildDir = corpus + "/build/" + FROST_CORPUS_FROSTENGINE_PRESET;
    const std::string compileCommands = buildDir + "/compile_commands.json";
    ASSERT_TRUE(fs.exists(compileCommands).value_or(false))
        << "configure the pinned FrostEngine corpus before the corpus tests: " << compileCommands;

    const std::string appCpp = frost::io::normalizePath(corpus + "/Frost/src/Frost/Core/Application.cpp").value_or("");
    const std::string appHeader = frost::io::normalizePath(corpus + "/Frost/src/Frost/Core/Application.h").value_or("");
    auto cppBytes = fs.readFile(appCpp);
    auto headerBytes = fs.readFile(appHeader);
    ASSERT_TRUE(cppBytes.has_value()) << cppBytes.error().describe();
    ASSERT_TRUE(headerBytes.has_value()) << headerBytes.error().describe();
    auto cppDoc = frost::text::Document::fromUtf8(*cppBytes);
    auto headerDoc = frost::text::Document::fromUtf8(*headerBytes);
    ASSERT_TRUE(cppDoc.has_value()) << cppDoc.error().describe();
    ASSERT_TRUE(headerDoc.has_value()) << headerDoc.error().describe();

    ClangdEnv env;
    ASSERT_TRUE(env.ok());
    auto clangd = fl::findClangd(fs, fl::defaultClangdSearch());
    ASSERT_TRUE(clangd.has_value()) << clangd.error().describe();
    ASSERT_NE(clangd->origin, fl::ClangdOrigin::Path) << "the acceptance test must use the pinned clangd";

    fl::ClangdServiceOptions options;
    options.clangdPath = clangd->path;
    options.workspaceRoot = corpus;
    options.launch.jobs = 0;
    options.launch.backgroundIndex = true;
    options.compileCommands = std::make_shared<FixedCompileCommandsProvider>(fl::CompileCommandsLocation{
        .directory = buildDir,
        .file = compileCommands,
        .source = fl::CompileCommandsSource::Existing,
        .origin = buildDir,
    });
    fl::ClangdService service(frost::io::realProcessLauncher(), env.context(), std::move(options));
    ASSERT_TRUE(service.start().has_value());
    ASSERT_TRUE(waitUntil(
        [&] { return service.state() == fl::ClangdState::Running || service.state() == fl::ClangdState::Failed; }));
    ASSERT_EQ(service.state(), fl::ClangdState::Running);
    ASSERT_TRUE(service.openDocument(appCpp, cppDoc->text()).has_value());
    ASSERT_TRUE(service.openDocument(appHeader, headerDoc->text()).has_value());
    ASSERT_TRUE(waitUntil(
        [&] {
            return service.diagnosedVersion(appCpp) == service.documentVersion(appCpp) &&
                   service.diagnosedVersion(appHeader) == service.documentVersion(appHeader);
        },
        5min))
        << "clangd did not finish analyzing the opened engine files";
    ASSERT_TRUE(waitUntil(
        [&] {
            const fl::IndexMetrics metrics = fl::indexMetrics(service);
            return metrics.completed && metrics.shardCount > 0;
        },
        20min))
        << "clangd background index did not complete with indexed shards";

    const auto rawReferences = [&](const std::string& path, fl::LspPosition position) {
        auto params = fl::textDocumentPosition(path, position);
        EXPECT_TRUE(params.has_value()) << params.error().describe();
        if (!params) {
            return fl::Json();
        }
        params->set("context", fl::Json::object().set("includeDeclaration", false));
        auto reply = env.run(service.request("textDocument/references", std::move(*params)));
        EXPECT_TRUE(reply.has_value()) << reply.error().describe();
        return reply.value_or(fl::Json());
    };
    fl::CppCodeVisionProvider provider;

    for (const EngineSymbol& symbol : kSymbols) {
        SCOPED_TRACE(symbol.name);
        const std::string& path = symbol.inHeader ? appHeader : appCpp;
        const std::string_view source = symbol.inHeader ? std::string_view(*headerBytes) : std::string_view(*cppBytes);
        const fl::LspPosition position = positionOf(source, symbol.marker, symbol.offset);
        const fl::Json oracle = rawReferences(path, position);
        ASSERT_TRUE(oracle.isArray()) << "clangd returned a non-array references response";
        auto counts = env.run(provider.counts(service, path, {position, symbol.kind}));
        ASSERT_TRUE(counts.has_value()) << counts.error().describe();
        ASSERT_TRUE(counts->usages.has_value()) << "clangd returned no usage count";
        EXPECT_EQ(*counts->usages, responseCount(oracle));
    }

    ASSERT_TRUE(service.closeDocument(appHeader).has_value());
    ASSERT_TRUE(service.closeDocument(appCpp).has_value());
    env.run(service.stop());
    EXPECT_EQ(service.state(), fl::ClangdState::Stopped);
}

} // namespace
