// SPDX-License-Identifier: Apache-2.0
//
// Tests for CPP-6 navigation, reference grouping, call hierarchy, type hierarchy, workspace
// class filtering, file ranking, and the 10-symbol FrostEngine clangd oracle (ADR 0078).

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace fl = frost::lang;
using fl::ClangdService;
using fl::ClangdState;
using fl::Json;
using namespace std::chrono_literals;

template<class Condition>
bool waitForNav(Condition condition, std::chrono::milliseconds timeout = 60s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

class NavClangdEnv {
public:
    NavClangdEnv() {
        frost::test::disableCrashDialogs();
        auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-cpp6-nav"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<frost::sched::ThreadPool>(frost::sched::ThreadPoolOptions{.workerCount = 2});
    }
    ~NavClangdEnv() {
        if (m_reactor) {
            m_reactor->shutdown();
        }
        if (m_pool) {
            m_pool->shutdown();
        }
    }
    NavClangdEnv(const NavClangdEnv&) = delete;
    NavClangdEnv& operator=(const NavClangdEnv&) = delete;

    [[nodiscard]] bool ok() const { return m_reactor && m_pool; }
    [[nodiscard]] frost::io::AsyncContext context() { return frost::io::AsyncContext{*m_reactor, *m_pool}; }
    template<class T>
    T run(frost::sched::Task<T> task) {
        return m_pool->spawn(frost::sched::Lane::UserVisible, std::move(task)).join();
    }

private:
    std::unique_ptr<frost::io::Reactor> m_reactor;
    std::unique_ptr<frost::sched::ThreadPool> m_pool;
};

class ScriptedNavRequester final : public fl::ILspRequester {
public:
    using Handler = std::function<Json(const Json&)>;

    void on(std::string method, Handler handler) { m_handlers[std::move(method)] = std::move(handler); }

    [[nodiscard]] fl::PositionEncoding encoding() const override { return fl::PositionEncoding::Utf16; }

    [[nodiscard]] std::optional<fl::ServerCapabilities> serverCapabilities() const override {
        return fl::ServerCapabilities{};
    }

    frost::sched::Task<frost::Result<Json>> request(std::string method, Json params, std::stop_token) override {
        if (const auto it = m_handlers.find(method); it != m_handlers.end()) {
            co_return it->second(params);
        }
        co_return Json();
    }

private:
    std::map<std::string, Handler> m_handlers;
};

fl::LspPosition findPositionIn(std::string_view text, std::string_view marker, std::size_t offset = 0) {
    const std::size_t at = text.find(marker);
    EXPECT_NE(at, std::string_view::npos) << marker;
    if (at == std::string_view::npos) {
        return {};
    }
    const std::string_view before = text.substr(0, at + offset);
    const auto line = static_cast<std::uint32_t>(std::ranges::count(before, '\n'));
    const std::size_t lineStart = before.rfind('\n') == std::string_view::npos ? 0 : before.rfind('\n') + 1;
    return fl::LspPosition{line, static_cast<std::uint32_t>(before.size() - lineStart)};
}

std::int64_t rawIntAt(const Json& object, std::string_view key) { return object.integerAt(key).value_or(-1); }

fl::LspPosition rawPos(const Json& pos) {
    return {static_cast<std::uint32_t>(rawIntAt(pos, "line")), static_cast<std::uint32_t>(rawIntAt(pos, "character"))};
}

fl::LspRange rawRange(const Json& range) {
    const Json* s = range.find("start");
    const Json* e = range.find("end");
    return {s ? rawPos(*s) : fl::LspPosition{}, e ? rawPos(*e) : fl::LspPosition{}};
}

using NormalizedLoc = std::pair<std::string, fl::LspRange>;

std::vector<NormalizedLoc> normalizeLocations(std::span<const fl::Location> locations) {
    std::vector<NormalizedLoc> out;
    out.reserve(locations.size());
    for (const fl::Location& loc : locations) {
        out.emplace_back(fl::documentKey(loc.path), loc.range);
    }
    std::ranges::sort(out, [](const NormalizedLoc& a, const NormalizedLoc& b) {
        return std::tie(a.first, a.second.start.line, a.second.start.character, a.second.end.line,
                        a.second.end.character) < std::tie(b.first, b.second.start.line, b.second.start.character,
                                                           b.second.end.line, b.second.end.character);
    });
    const auto dup = std::ranges::unique(out);
    out.erase(dup.begin(), dup.end());
    return out;
}

std::vector<NormalizedLoc> parseRawReferenceOracle(const Json& rawRefs) {
    std::vector<fl::Location> locs;
    if (rawRefs.isArray()) {
        for (const Json& item : rawRefs.items()) {
            const Json* range = item.find("range");
            const std::string_view uri = item.stringAt("uri");
            if (!range || uri.empty()) {
                continue;
            }
            auto path = fl::uriToPath(uri);
            if (!path) {
                continue;
            }
            locs.push_back(fl::Location{
                .path = std::move(*path),
                .uri = std::string(uri),
                .range = rawRange(*range),
            });
        }
    }
    return normalizeLocations(locs);
}

using HierarchyEdgeKey = std::tuple<std::string, std::string, fl::LspRange>;

void sortHierarchyEdges(std::vector<HierarchyEdgeKey>& edges) {
    std::ranges::sort(edges, [](const HierarchyEdgeKey& a, const HierarchyEdgeKey& b) {
        const auto& [nameA, docA, rangeA] = a;
        const auto& [nameB, docB, rangeB] = b;
        return std::tie(nameA, docA, rangeA.start.line, rangeA.start.character, rangeA.end.line, rangeA.end.character) <
               std::tie(nameB, docB, rangeB.start.line, rangeB.start.character, rangeB.end.line, rangeB.end.character);
    });
}

std::vector<HierarchyEdgeKey> parseRawCallEdges(const Json& rawCalls, std::string_view itemField) {
    std::vector<HierarchyEdgeKey> out;
    if (!rawCalls.isArray()) {
        return out;
    }
    for (const Json& entry : rawCalls.items()) {
        const Json* item = entry.find(itemField);
        if (!item || !item->isObject()) {
            continue;
        }
        const std::string_view uri = item->stringAt("uri");
        const Json* sel = item->find("selectionRange");
        auto path = fl::uriToPath(uri);
        if (!path || !sel) {
            continue;
        }
        out.emplace_back(std::string(item->stringAt("name")), fl::documentKey(*path), rawRange(*sel));
    }
    sortHierarchyEdges(out);
    return out;
}

std::vector<HierarchyEdgeKey> parseRawTypeEdges(const Json& rawTypes) {
    std::vector<HierarchyEdgeKey> out;
    if (!rawTypes.isArray()) {
        return out;
    }
    for (const Json& item : rawTypes.items()) {
        if (!item.isObject()) {
            continue;
        }
        const std::string_view uri = item.stringAt("uri");
        const Json* sel = item.find("selectionRange");
        auto path = fl::uriToPath(uri);
        if (!path || !sel) {
            continue;
        }
        out.emplace_back(std::string(item.stringAt("name")), fl::documentKey(*path), rawRange(*sel));
    }
    sortHierarchyEdges(out);
    return out;
}

TEST(Navigation, FindUsagesGroupsAndFlattensAuthoritativeLspLocations) {
    ScriptedNavRequester server;
#ifdef _WIN32
    const std::string path = "C:\\work\\Engine.cpp";
#else
    const std::string path = "/work/Engine.cpp";
#endif
    const std::string uri = *fl::pathToUri(path);
    server.on("textDocument/references", [&](const Json&) {
        return Json::array({
            Json::object().set("uri", uri).set("range", fl::toJson(fl::LspRange{{0, 4}, {0, 9}})),
            Json::object().set("uri", uri).set("range", fl::toJson(fl::LspRange{{1, 4}, {1, 9}})),
            Json::object().set("uri", uri).set("range", fl::toJson(fl::LspRange{{2, 11}, {2, 16}})),
            Json::object().set("uri", uri).set("range", fl::toJson(fl::LspRange{{3, 4}, {3, 9}})),
        });
    });

    const std::vector<std::string> sourceLines = {
        "int value = 0;",
        "    value += 5;",
        "    return value;",
        "    value(10);",
    };
    const fl::LineSource lineSource = [&](std::string_view, std::uint32_t line) -> std::optional<std::string> {
        if (line < sourceLines.size()) {
            return sourceLines[line];
        }
        return std::nullopt;
    };

    NavClangdEnv env;
    ASSERT_TRUE(env.ok());
    const auto groups = env.run(fl::findUsages(server, path, fl::LspPosition{0, 4}, lineSource));
    ASSERT_TRUE(groups.has_value()) << groups.error().describe();
    ASSERT_EQ(groups->size(), 1U);
    ASSERT_EQ((*groups)[0].usages.size(), 4U);
    EXPECT_TRUE(std::ranges::all_of((*groups)[0].usages,
                                    [](const fl::Usage& usage) { return usage.kind == fl::UsageKind::Reference; }));

    const std::vector<fl::Location> flattened = fl::flattenUsageLocations(*groups);
    ASSERT_EQ(flattened.size(), 4U);
    EXPECT_EQ(flattened[0].range.start.line, 0U);
    EXPECT_EQ(flattened[1].range.start.line, 1U);
    EXPECT_EQ(flattened[2].range.start.line, 2U);
    EXPECT_EQ(flattened[3].range.start.line, 3U);
}

TEST(Navigation, WorkspaceClassesAndFileRankingFilterAndOrderDeterministically) {
    EXPECT_TRUE(fl::isClassLikeSymbol(fl::SymbolKind::Class));
    EXPECT_TRUE(fl::isClassLikeSymbol(fl::SymbolKind::Struct));
    EXPECT_TRUE(fl::isClassLikeSymbol(fl::SymbolKind::Interface));
    EXPECT_TRUE(fl::isClassLikeSymbol(fl::SymbolKind::Enum));
    EXPECT_FALSE(fl::isClassLikeSymbol(fl::SymbolKind::Function));
    EXPECT_FALSE(fl::isClassLikeSymbol(fl::SymbolKind::Method));
    EXPECT_FALSE(fl::isClassLikeSymbol(fl::SymbolKind::Variable));

    std::vector<fl::WorkspaceSymbol> symbols = {
        {.name = "Application", .containerName = "", .kind = fl::SymbolKind::Class, .location = {}, .match = {}},
        {.name = "BeginScene", .containerName = "", .kind = fl::SymbolKind::Method, .location = {}, .match = {}},
        {.name = "Timestep", .containerName = "", .kind = fl::SymbolKind::Struct, .location = {}, .match = {}},
        {.name = "g_Engine", .containerName = "", .kind = fl::SymbolKind::Variable, .location = {}, .match = {}},
    };
    const auto classes = fl::filterWorkspaceClasses(std::move(symbols));
    ASSERT_EQ(classes.size(), 2U);
    EXPECT_EQ(classes[0].name, "Application");
    EXPECT_EQ(classes[1].name, "Timestep");

    const std::vector<std::string> files = {
        "/repo/Frost/src/Frost/Renderer/Renderer.cpp",
        "/repo/Frost/src/Frost/Core/Application.h",
        "/repo/Frost/src/Frost/Scene/EditorLayer.h",
        "/repo/Frost/src/Frost/Core/Layer.h",
    };
    const auto ranked = fl::rankWorkspaceFiles(files, "Layer.h");
    ASSERT_GE(ranked.size(), 2U);
    EXPECT_EQ(ranked[0].fileName, "Layer.h");
    EXPECT_EQ(ranked[1].fileName, "EditorLayer.h");

    const auto allFiles = fl::rankWorkspaceFiles(files, "");
    ASSERT_EQ(allFiles.size(), 4U);
    EXPECT_EQ(allFiles[0].fileName, "Application.h");
}

constexpr std::string_view kEngineHeader = R"cpp(#pragma once

namespace Frost {

struct Timestep {
    float m_Time = 0.0f;
    float GetSeconds() const { return m_Time; }
    void AddSeconds(float delta) { m_Time += delta; }
};

class Event {
public:
    virtual ~Event() = default;
    virtual const char* GetName() const { return "Event"; }
    bool Handled = false;
};

class WindowResizeEvent : public Event {
public:
    WindowResizeEvent(unsigned int w, unsigned int h) : Width(w), Height(h) {}
    const char* GetName() const override { return "WindowResizeEvent"; }
    unsigned int Width = 0;
    unsigned int Height = 0;
};

class Entity {
public:
    explicit Entity(unsigned int id) : m_Id(id) {}
    unsigned int GetId() const { return m_Id; }
    void SetId(unsigned int id) { m_Id = id; }

private:
    unsigned int m_Id = 0;
};

class Scene {
public:
    Entity CreateEntity(unsigned int id);
    void OnUpdate(Timestep ts);
    int EntityCount = 0;
};

class Renderer {
public:
    static void ResetStats();
    static void BeginScene(Scene& scene, Timestep ts);
};

class Layer {
public:
    virtual ~Layer() = default;
    virtual void OnAttach() {}
    virtual void OnUpdate(Timestep ts) = 0;
    virtual void OnEvent(Event& event) {}
};

class EditorLayer : public Layer {
public:
    void OnAttach() override;
    void OnUpdate(Timestep ts) override;
    void OnEvent(Event& event) override;

private:
    Scene m_EditorScene;
};

class RuntimeLayer : public Layer {
public:
    void OnAttach() override;
    void OnUpdate(Timestep ts) override;

private:
    Scene m_RuntimeScene;
};

class Application {
public:
    void PushLayer(Layer* layer);
    void DispatchEvent(Event& event);
    void StepFrame(Timestep ts);
    void Run();

private:
    EditorLayer m_Editor;
    RuntimeLayer m_Runtime;
};

} // namespace Frost
)cpp";

constexpr std::string_view kEngineRuntimeCpp = R"cpp(#include "EngineSymbols.h"

namespace Frost {

Entity Scene::CreateEntity(unsigned int id) {
    EntityCount += 1;
    Entity created(id);
    created.SetId(id);
    return created;
}

void Scene::OnUpdate(Timestep ts) {
    if (ts.GetSeconds() > 0.0f) {
        Entity entity = CreateEntity(1u);
        (void)entity.GetId();
    }
}

void Renderer::ResetStats() {}

void Renderer::BeginScene(Scene& scene, Timestep ts) {
    ResetStats();
    scene.OnUpdate(ts);
}

void EditorLayer::OnAttach() {
    Entity root = m_EditorScene.CreateEntity(10u);
    (void)root.GetId();
}

void EditorLayer::OnUpdate(Timestep ts) {
    ts.AddSeconds(0.001f);
    Renderer::BeginScene(m_EditorScene, ts);
}

void EditorLayer::OnEvent(Event& event) {
    WindowResizeEvent resize(1280u, 720u);
    if (resize.Width > 0u) {
        event.Handled = true;
    }
}

} // namespace Frost
)cpp";

constexpr std::string_view kEngineAppCpp = R"cpp(#include "EngineSymbols.h"

namespace Frost {

void RuntimeLayer::OnAttach() {
    Entity player = m_RuntimeScene.CreateEntity(20u);
    (void)player.GetId();
}

void RuntimeLayer::OnUpdate(Timestep ts) {
    Renderer::BeginScene(m_RuntimeScene, ts);
}

void Application::PushLayer(Layer* layer) {
    if (layer != nullptr) {
        layer->OnAttach();
    }
}

void Application::DispatchEvent(Event& event) {
    m_Editor.OnEvent(event);
}

void Application::StepFrame(Timestep ts) {
    m_Editor.OnUpdate(ts);
    m_Runtime.OnUpdate(ts);
}

void Application::Run() {
    PushLayer(&m_Editor);
    PushLayer(&m_Runtime);
    Timestep frame{0.016f};
    WindowResizeEvent evt(1920u, 1080u);
    DispatchEvent(evt);
    StepFrame(frame);
}

} // namespace Frost
)cpp";

struct NamedEngineSymbolSpec {
    std::string_view qualifiedName;
    std::string_view shortName;
    std::string_view headerMarker;
    std::size_t markerOffset = 0;
    bool expectCallHierarchy = false;
    bool expectSupertypes = false;
    bool expectSubtypes = false;
};

constexpr NamedEngineSymbolSpec kTenEngineSymbols[10] = {
    {"Frost::Application", "Application", "class Application", 6, false, false, false},
    {"Frost::Layer", "Layer", "class Layer", 6, false, false, true},
    {"Frost::EditorLayer", "EditorLayer", "class EditorLayer", 6, false, true, false},
    {"Frost::RuntimeLayer", "RuntimeLayer", "class RuntimeLayer", 6, false, true, false},
    {"Frost::Scene", "Scene", "class Scene", 6, false, false, false},
    {"Frost::Entity", "Entity", "class Entity", 6, false, false, false},
    {"Frost::Timestep", "Timestep", "struct Timestep", 7, false, false, false},
    {"Frost::Event", "Event", "class Event", 6, false, false, true},
    {"Frost::WindowResizeEvent", "WindowResizeEvent", "class WindowResizeEvent", 6, false, true, false},
    {"Frost::Renderer::BeginScene", "BeginScene", "static void BeginScene", 12, true, false, false},
};

TEST(Navigation, TenFrostEngineSymbolsMatchClangdReferencesAndHierarchyOracles) {
    frost::test::ScratchDir scratch("cpp6-ten-engine-symbols");
    auto& fs = frost::io::realFileSystem();
    const std::string header = scratch.join("EngineSymbols.h");
    const std::string runtimeCpp = scratch.join("EngineRuntime.cpp");
    const std::string appCpp = scratch.join("Application.cpp");
    ASSERT_TRUE(fs.writeFileAtomic(header, kEngineHeader).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(runtimeCpp, kEngineRuntimeCpp).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(appCpp, kEngineAppCpp).has_value());

    std::vector<fl::CompileCommand> commands;
    for (const std::string& src : {runtimeCpp, appCpp}) {
        commands.push_back(fl::CompileCommand{
            .directory = scratch.path(),
            .file = src,
            .arguments = {"clang++", "-std=c++20", "-I" + scratch.path(), "-c", src},
        });
    }
    ASSERT_TRUE(fl::writeCompileCommands(fs, scratch.join("build"), commands).has_value());

    NavClangdEnv env;
    ASSERT_TRUE(env.ok());
    auto foundClangd = fl::findClangd(fs, fl::defaultClangdSearch());
    ASSERT_TRUE(foundClangd.has_value()) << foundClangd.error().describe();

    fl::ClangdServiceOptions options;
    options.clangdPath = foundClangd->path;
    options.workspaceRoot = scratch.path();
    options.launch.jobs = 2;
    options.launch.backgroundIndex = false;
    options.compileCommands = std::make_shared<fl::ExistingCompileCommandsProvider>(fs, scratch.path());
    auto service = std::make_unique<ClangdService>(frost::io::realProcessLauncher(), env.context(), std::move(options));
    ASSERT_TRUE(service->start().has_value());
    ASSERT_TRUE(waitForNav(
        [&] { return service->state() == ClangdState::Running || service->state() == ClangdState::Failed; }));
    ASSERT_EQ(service->state(), ClangdState::Running);

    auto headerDoc = frost::text::Document::fromUtf8(std::string(kEngineHeader));
    auto runtimeDoc = frost::text::Document::fromUtf8(std::string(kEngineRuntimeCpp));
    auto appDoc = frost::text::Document::fromUtf8(std::string(kEngineAppCpp));
    ASSERT_TRUE(headerDoc && runtimeDoc && appDoc);
    ASSERT_TRUE(service->openDocument(header, headerDoc->text()).has_value());
    ASSERT_TRUE(service->openDocument(runtimeCpp, runtimeDoc->text()).has_value());
    ASSERT_TRUE(service->openDocument(appCpp, appDoc->text()).has_value());

    ASSERT_TRUE(waitForNav([&] {
        return service->diagnosedVersion(header) == service->documentVersion(header) &&
               service->diagnosedVersion(runtimeCpp) == service->documentVersion(runtimeCpp) &&
               service->diagnosedVersion(appCpp) == service->documentVersion(appCpp);
    }));

    std::map<std::string, std::vector<std::string>> linesByKey;
    const auto splitLines = [](std::string_view content) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= content.size()) {
            const std::size_t nl = content.find('\n', start);
            if (nl == std::string_view::npos) {
                lines.emplace_back(content.substr(start));
                break;
            }
            lines.emplace_back(content.substr(start, nl - start));
            start = nl + 1;
        }
        return lines;
    };
    linesByKey[fl::documentKey(header)] = splitLines(kEngineHeader);
    linesByKey[fl::documentKey(runtimeCpp)] = splitLines(kEngineRuntimeCpp);
    linesByKey[fl::documentKey(appCpp)] = splitLines(kEngineAppCpp);

    const fl::LineSource lineSource = [&](std::string_view path, std::uint32_t line) -> std::optional<std::string> {
        const auto it = linesByKey.find(fl::documentKey(path));
        if (it == linesByKey.end() || line >= it->second.size()) {
            return std::nullopt;
        }
        return it->second[line];
    };

    const auto rawRequest = [&](std::string method, Json params) {
        auto res = env.run(service->request(std::move(method), std::move(params)));
        EXPECT_TRUE(res.has_value()) << res.error().describe();
        return res.value_or(Json());
    };

    for (const NamedEngineSymbolSpec& spec : kTenEngineSymbols) {
        SCOPED_TRACE(spec.qualifiedName);
        const fl::LspPosition pos = findPositionIn(kEngineHeader, spec.headerMarker, spec.markerOffset);

        // 1. Find Usages vs raw textDocument/references oracle.
        Json refParams = fl::textDocumentPosition(header, pos).value();
        refParams.set("context", Json::object().set("includeDeclaration", true));
        const Json rawRefs = rawRequest("textDocument/references", std::move(refParams));
        const std::vector<NormalizedLoc> oracleRefs = parseRawReferenceOracle(rawRefs);
        EXPECT_GE(oracleRefs.size(), 2U) << "expected multi-TU references for " << spec.qualifiedName;

        const auto groups = env.run(fl::findUsages(*service, header, pos, lineSource, true));
        ASSERT_TRUE(groups.has_value()) << groups.error().describe();
        const std::vector<fl::Location> flatUsages = fl::flattenUsageLocations(*groups);
        const std::vector<NormalizedLoc> actualRefs = normalizeLocations(flatUsages);
        EXPECT_EQ(actualRefs, oracleRefs);

        // 2. Call Hierarchy vs raw prepareCallHierarchy + incomingCalls + outgoingCalls oracle.
        const Json rawCallPrep =
            rawRequest("textDocument/prepareCallHierarchy", fl::textDocumentPosition(header, pos).value());
        const auto callSnap = env.run(fl::queryCallHierarchy(*service, header, pos));
        ASSERT_TRUE(callSnap.has_value()) << callSnap.error().describe();
        if (!rawCallPrep.isArray() || rawCallPrep.size() == 0) {
            EXPECT_FALSE(callSnap->has_value());
        } else {
            ASSERT_TRUE(callSnap->has_value());
            const Json& rawRoot = rawCallPrep.items()[0];
            EXPECT_EQ((*callSnap)->root.name, rawRoot.stringAt("name"));

            const Json rawIncoming = rawRequest("callHierarchy/incomingCalls", Json::object().set("item", rawRoot));
            const Json rawOutgoing = rawRequest("callHierarchy/outgoingCalls", Json::object().set("item", rawRoot));
            const auto oracleIncoming = parseRawCallEdges(rawIncoming, "from");
            const auto oracleOutgoing = parseRawCallEdges(rawOutgoing, "to");

            std::vector<HierarchyEdgeKey> actualIncoming;
            for (const fl::CallHierarchyIncomingCall& call : (*callSnap)->incoming) {
                actualIncoming.emplace_back(call.from.name, fl::documentKey(call.from.path), call.from.selectionRange);
            }
            sortHierarchyEdges(actualIncoming);
            EXPECT_EQ(actualIncoming, oracleIncoming);

            std::vector<HierarchyEdgeKey> actualOutgoing;
            for (const fl::CallHierarchyOutgoingCall& call : (*callSnap)->outgoing) {
                actualOutgoing.emplace_back(call.to.name, fl::documentKey(call.to.path), call.to.selectionRange);
            }
            sortHierarchyEdges(actualOutgoing);
            EXPECT_EQ(actualOutgoing, oracleOutgoing);

            if (spec.expectCallHierarchy) {
                EXPECT_GE(actualIncoming.size(), 2U);
            }
        }

        // 3. Type Hierarchy vs raw prepareTypeHierarchy + supertypes + subtypes oracle.
        const Json rawTypePrep =
            rawRequest("textDocument/prepareTypeHierarchy", fl::textDocumentPosition(header, pos).value());
        const auto typeSnap = env.run(fl::queryTypeHierarchy(*service, header, pos));
        ASSERT_TRUE(typeSnap.has_value()) << typeSnap.error().describe();
        if (!rawTypePrep.isArray() || rawTypePrep.size() == 0) {
            EXPECT_FALSE(typeSnap->has_value());
        } else {
            ASSERT_TRUE(typeSnap->has_value());
            const Json& rawRoot = rawTypePrep.items()[0];
            EXPECT_EQ((*typeSnap)->root.name, rawRoot.stringAt("name"));

            const Json rawSupers = rawRequest("typeHierarchy/supertypes", Json::object().set("item", rawRoot));
            const Json rawSubs = rawRequest("typeHierarchy/subtypes", Json::object().set("item", rawRoot));
            const auto oracleSupers = parseRawTypeEdges(rawSupers);
            const auto oracleSubs = parseRawTypeEdges(rawSubs);

            std::vector<HierarchyEdgeKey> actualSupers;
            for (const fl::TypeHierarchyItem& item : (*typeSnap)->supertypes) {
                actualSupers.emplace_back(item.name, fl::documentKey(item.path), item.selectionRange);
            }
            sortHierarchyEdges(actualSupers);
            EXPECT_EQ(actualSupers, oracleSupers);

            std::vector<HierarchyEdgeKey> actualSubs;
            for (const fl::TypeHierarchyItem& item : (*typeSnap)->subtypes) {
                actualSubs.emplace_back(item.name, fl::documentKey(item.path), item.selectionRange);
            }
            sortHierarchyEdges(actualSubs);
            EXPECT_EQ(actualSubs, oracleSubs);

            if (spec.expectSupertypes) {
                EXPECT_FALSE(actualSupers.empty());
            }
            if (spec.expectSubtypes) {
                EXPECT_FALSE(actualSubs.empty());
            }
        }

        // 4. Go to Symbol / Go to Class via workspace/symbol.
        const auto wsSymbols = env.run(fl::requestWorkspaceSymbols(*service, std::string(spec.shortName)));
        ASSERT_TRUE(wsSymbols.has_value()) << wsSymbols.error().describe();
        EXPECT_TRUE(std::ranges::any_of(*wsSymbols, [&](const fl::WorkspaceSymbol& s) {
            return s.name.find(spec.shortName) != std::string::npos;
        }));
    }

    ASSERT_TRUE(service->closeDocument(appCpp).has_value());
    ASSERT_TRUE(service->closeDocument(runtimeCpp).has_value());
    ASSERT_TRUE(service->closeDocument(header).has_value());
    env.run(service->stop());
    EXPECT_EQ(service->state(), ClangdState::Stopped);
}

} // namespace
