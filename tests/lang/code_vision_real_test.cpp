// SPDX-License-Identifier: Apache-2.0
//
// CPP-9 integration acceptance against the pinned clangd: Code Vision's usages, overrides and
// implementation counts equal the index answers returned directly by the same real server.

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
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fl = frost::lang;
using namespace std::chrono_literals;

constexpr std::string_view kHeader = R"cpp(#pragma once
namespace sample {
struct Renderer {
    virtual void draw() = 0;
};
struct Sprite : Renderer {
    void draw() override;
};
struct Mesh : Renderer {
    void draw() override;
};
void paint(Renderer& renderer);
}
)cpp";

constexpr std::string_view kSpriteCpp = R"cpp(#include "CodeVision.h"
void sample::Sprite::draw() {}
void sample::paint(sample::Renderer& renderer) {
    renderer.draw();
    renderer.draw();
}
)cpp";

constexpr std::string_view kMeshCpp = R"cpp(#include "CodeVision.h"
void sample::Mesh::draw() {}
void present(sample::Sprite& sprite) { sample::paint(sprite); }
)cpp";

template<class Condition>
bool waitUntil(Condition condition, std::chrono::milliseconds timeout = 60s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

class ClangdEnv {
public:
    ClangdEnv() {
        frost::test::disableCrashDialogs();
        auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-code-vision"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<frost::sched::ThreadPool>(frost::sched::ThreadPoolOptions{.workerCount = 2});
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

fl::LspPosition positionOf(std::string_view source, std::string_view marker, std::size_t offset = 0) {
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

TEST(CodeVisionRealClangd, CountsMatchRawReferencesOverridesAndTypeImplementations) {
    frost::test::ScratchDir scratch("code-vision-real-clangd");
    auto& fs = frost::io::realFileSystem();
    const std::string& root = scratch.path();
    const std::string header = scratch.join("CodeVision.h");
    const std::string spriteCpp = scratch.join("Sprite.cpp");
    const std::string meshCpp = scratch.join("Mesh.cpp");
    ASSERT_TRUE(fs.writeFileAtomic(header, kHeader).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(spriteCpp, kSpriteCpp).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(meshCpp, kMeshCpp).has_value());

    std::vector<fl::CompileCommand> commands;
    for (const std::string& source : {spriteCpp, meshCpp}) {
        commands.push_back(fl::CompileCommand{
            .directory = root,
            .file = source,
            .arguments = {"clang++", "-std=c++20", "-I", root, "-c", source},
        });
    }
    ASSERT_TRUE(fl::writeCompileCommands(fs, scratch.join("build"), commands).has_value());

    ClangdEnv env;
    ASSERT_TRUE(env.ok());
    auto clangd = fl::findClangd(fs, fl::defaultClangdSearch());
    ASSERT_TRUE(clangd.has_value()) << clangd.error().describe();
    ASSERT_NE(clangd->origin, fl::ClangdOrigin::Path) << "the acceptance test must use the pinned clangd";

    fl::ClangdServiceOptions options;
    options.clangdPath = clangd->path;
    options.workspaceRoot = root;
    options.launch.jobs = 2;
    options.launch.backgroundIndex = true;
    options.compileCommands = std::make_shared<fl::ExistingCompileCommandsProvider>(fs, root);
    fl::ClangdService service(frost::io::realProcessLauncher(), env.context(), std::move(options));
    ASSERT_TRUE(service.start().has_value());
    ASSERT_TRUE(waitUntil(
        [&] { return service.state() == fl::ClangdState::Running || service.state() == fl::ClangdState::Failed; }));
    ASSERT_EQ(service.state(), fl::ClangdState::Running);

    for (const auto& [path, text] : std::vector<std::pair<std::string, std::string_view>>{
             {header, kHeader}, {spriteCpp, kSpriteCpp}, {meshCpp, kMeshCpp}}) {
        auto doc = frost::text::Document::fromUtf8(std::string(text));
        ASSERT_TRUE(doc.has_value()) << path;
        ASSERT_TRUE(service.openDocument(path, doc->text()).has_value());
    }
    ASSERT_TRUE(waitUntil([&] {
        return service.diagnosedVersion(header) == service.documentVersion(header) &&
               service.diagnosedVersion(spriteCpp) == service.documentVersion(spriteCpp) &&
               service.diagnosedVersion(meshCpp) == service.documentVersion(meshCpp);
    }));
    ASSERT_TRUE(waitUntil([&] { return fl::indexMetrics(service).completed; }))
        << "clangd did not finish indexing the three-file fixture";

    const auto raw = [&](std::string method, fl::Json params) {
        auto reply = env.run(service.request(std::move(method), std::move(params)));
        EXPECT_TRUE(reply.has_value()) << reply.error().describe();
        return reply.value_or(fl::Json());
    };
    fl::CppCodeVisionProvider provider;

    // The renderer type exercises a positive type implementation count, while the abstract method
    // exercises a positive override count. Direct JSON-RPC array lengths are the independent oracle.
    const fl::LspPosition rendererPos = positionOf(kHeader, "struct Renderer", 7);
    auto rendererParams = fl::textDocumentPosition(header, rendererPos);
    ASSERT_TRUE(rendererParams.has_value());
    fl::Json rendererRefParams = *rendererParams;
    rendererRefParams.set("context", fl::Json::object().set("includeDeclaration", false));
    const fl::Json rendererRefs = raw("textDocument/references", std::move(rendererRefParams));
    const fl::Json rendererPrep = raw("textDocument/prepareTypeHierarchy", *rendererParams);
    ASSERT_GT(responseCount(rendererPrep), 0U);
    const fl::Json rendererSubtypes =
        raw("typeHierarchy/subtypes", fl::Json::object().set("item", rendererPrep.items().front()));
    ASSERT_GE(responseCount(rendererSubtypes), 2U);
    ASSERT_GE(responseCount(rendererRefs), 2U);
    auto rendererCounts = env.run(provider.counts(service, header, {rendererPos, fl::SymbolKind::Struct}));
    ASSERT_TRUE(rendererCounts.has_value()) << rendererCounts.error().describe();
    ASSERT_TRUE(rendererCounts->usages.has_value());
    ASSERT_TRUE(rendererCounts->implementations.has_value());
    EXPECT_EQ(*rendererCounts->usages, responseCount(rendererRefs));
    EXPECT_EQ(*rendererCounts->implementations, responseCount(rendererSubtypes));

    const fl::LspPosition drawPos = positionOf(kHeader, "void draw()", 5);
    auto drawParams = fl::textDocumentPosition(header, drawPos);
    ASSERT_TRUE(drawParams.has_value());
    fl::Json drawRefParams = *drawParams;
    drawRefParams.set("context", fl::Json::object().set("includeDeclaration", false));
    const fl::Json drawRefs = raw("textDocument/references", std::move(drawRefParams));
    const fl::Json drawImplementations = raw("textDocument/implementation", *drawParams);
    ASSERT_GE(responseCount(drawImplementations), 2U);
    ASSERT_GE(responseCount(drawRefs), 2U);
    auto drawCounts = env.run(provider.counts(service, header, {drawPos, fl::SymbolKind::Method}));
    ASSERT_TRUE(drawCounts.has_value()) << drawCounts.error().describe();
    ASSERT_TRUE(drawCounts->usages.has_value());
    ASSERT_TRUE(drawCounts->overrides.has_value());
    EXPECT_EQ(*drawCounts->usages, responseCount(drawRefs));
    EXPECT_EQ(*drawCounts->overrides, responseCount(drawImplementations));

    ASSERT_TRUE(service.closeDocument(meshCpp).has_value());
    ASSERT_TRUE(service.closeDocument(spriteCpp).has_value());
    ASSERT_TRUE(service.closeDocument(header).has_value());
    env.run(service.stop());
    EXPECT_EQ(service.state(), fl::ClangdState::Stopped);
}

} // namespace
