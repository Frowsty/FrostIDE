// SPDX-License-Identifier: Apache-2.0
//
// CPP-6 corpus acceptance: requires the pinned FrostEngine clone and its configured compile database.
// Find Usages, Call Hierarchy, and Type Hierarchy are compared with independent raw clangd responses
// after background indexing the full configured corpus, with shared-cache and isolated cold variants.

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
#include "support/engine_lock.hpp"
#include "support/process_memory_evidence.hpp"
#include "support/scratch_dir.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
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
bool waitForCpp6(Condition condition, std::chrono::milliseconds timeout = 60s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

class Cpp6Env {
public:
    Cpp6Env() {
        auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-cpp6-corpus"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<frost::sched::ThreadPool>(frost::sched::ThreadPoolOptions{.workerCount = 2});
    }
    ~Cpp6Env() {
        if (m_reactor)
            m_reactor->shutdown();
        if (m_pool)
            m_pool->shutdown();
    }
    bool ok() const { return m_reactor && m_pool; }
    frost::io::AsyncContext context() { return frost::io::AsyncContext{*m_reactor, *m_pool}; }
    template<class T>
    T run(frost::sched::Task<T> task) {
        return m_pool->spawn(frost::sched::Lane::UserVisible, std::move(task)).join();
    }

private:
    std::unique_ptr<frost::io::Reactor> m_reactor;
    std::unique_ptr<frost::sched::ThreadPool> m_pool;
};

using Cpp6Edge = std::tuple<std::string, std::string, fl::LspRange>;

fl::LspRange cpp6RawRange(const Json& range) {
    const Json* start = range.find("start");
    const Json* end = range.find("end");
    const auto pos = [](const Json& p) {
        return fl::LspPosition{static_cast<std::uint32_t>(p.integerAt("line").value_or(0)),
                               static_cast<std::uint32_t>(p.integerAt("character").value_or(0))};
    };
    return {start ? pos(*start) : fl::LspPosition{}, end ? pos(*end) : fl::LspPosition{}};
}

fl::LspPosition cpp6FindPosition(std::string_view text, std::string_view marker, std::size_t offset) {
    const std::size_t at = text.find(marker);
    EXPECT_NE(at, std::string_view::npos) << marker;
    if (at == std::string_view::npos)
        return {};
    const std::string_view before = text.substr(0, at + offset);
    const std::size_t lineStart = before.rfind('\n') == std::string_view::npos ? 0 : before.rfind('\n') + 1;
    return {static_cast<std::uint32_t>(std::ranges::count(before, '\n')),
            static_cast<std::uint32_t>(before.size() - lineStart)};
}

std::vector<std::pair<std::string, fl::LspRange>> cpp6NormalizeLocations(std::span<const fl::Location> locations) {
    std::vector<std::pair<std::string, fl::LspRange>> out;
    for (const fl::Location& location : locations)
        out.emplace_back(fl::documentKey(location.path), location.range);
    std::ranges::sort(out, [](const auto& a, const auto& b) {
        return std::tie(a.first, a.second.start.line, a.second.start.character, a.second.end.line,
                        a.second.end.character) < std::tie(b.first, b.second.start.line, b.second.start.character,
                                                           b.second.end.line, b.second.end.character);
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::pair<std::string, fl::LspRange>> cpp6RawReferences(const Json& json) {
    std::vector<fl::Location> locations;
    if (json.isArray())
        for (const Json& item : json.items()) {
            const auto path = fl::uriToPath(item.stringAt("uri"));
            const Json* range = item.find("range");
            if (!path || !range)
                continue;
            const Json* start = range->find("start");
            const Json* end = range->find("end");
            if (!start || !end)
                continue;
            locations.push_back(
                {.path = *path, .uri = std::string(item.stringAt("uri")), .range = cpp6RawRange(*range)});
        }
    return cpp6NormalizeLocations(locations);
}

std::vector<Cpp6Edge> cpp6RawEdges(const Json& json, std::string_view field) {
    std::vector<Cpp6Edge> edges;
    if (!json.isArray())
        return edges;
    for (const Json& entry : json.items()) {
        const Json* item = field.empty() ? &entry : entry.find(field);
        if (!item || !item->isObject())
            continue;
        const auto path = fl::uriToPath(item->stringAt("uri"));
        const Json* range = item->find("selectionRange");
        if (!path || !range)
            continue;
        if (!range->find("start") || !range->find("end"))
            continue;
        edges.emplace_back(std::string(item->stringAt("name")), fl::documentKey(*path), cpp6RawRange(*range));
    }
    std::ranges::sort(edges, [](const auto& a, const auto& b) {
        const auto& [an, ap, ar] = a;
        const auto& [bn, bp, br] = b;
        return std::tie(an, ap, ar.start.line, ar.start.character, ar.end.line, ar.end.character) <
               std::tie(bn, bp, br.start.line, br.start.character, br.end.line, br.end.character);
    });
    return edges;
}

std::vector<Cpp6Edge> cpp6ActualIncomingEdges(const std::vector<fl::CallHierarchyIncomingCall>& calls) {
    std::vector<Cpp6Edge> edges;
    for (const auto& call : calls) {
        const auto& item = call.from;
        edges.emplace_back(item.name, fl::documentKey(item.path), item.selectionRange);
    }
    std::ranges::sort(edges, [](const auto& a, const auto& b) {
        const auto& [an, ap, ar] = a;
        const auto& [bn, bp, br] = b;
        return std::tie(an, ap, ar.start.line, ar.start.character, ar.end.line, ar.end.character) <
               std::tie(bn, bp, br.start.line, br.start.character, br.end.line, br.end.character);
    });
    return edges;
}

std::vector<Cpp6Edge> cpp6ActualOutgoingEdges(const std::vector<fl::CallHierarchyOutgoingCall>& calls) {
    std::vector<Cpp6Edge> edges;
    for (const auto& call : calls) {
        const auto& item = call.to;
        edges.emplace_back(item.name, fl::documentKey(item.path), item.selectionRange);
    }
    std::ranges::sort(edges, [](const auto& a, const auto& b) {
        const auto& [an, ap, ar] = a;
        const auto& [bn, bp, br] = b;
        return std::tie(an, ap, ar.start.line, ar.start.character, ar.end.line, ar.end.character) <
               std::tie(bn, bp, br.start.line, br.start.character, br.end.line, br.end.character);
    });
    return edges;
}

std::vector<Cpp6Edge> cpp6ActualTypeEdges(const std::vector<fl::TypeHierarchyItem>& items) {
    std::vector<Cpp6Edge> edges;
    for (const auto& item : items)
        edges.emplace_back(item.name, fl::documentKey(item.path), item.selectionRange);
    std::ranges::sort(edges, [](const auto& a, const auto& b) {
        const auto& [an, ap, ar] = a;
        const auto& [bn, bp, br] = b;
        return std::tie(an, ap, ar.start.line, ar.start.character, ar.end.line, ar.end.character) <
               std::tie(bn, bp, br.start.line, br.start.character, br.end.line, br.end.character);
    });
    return edges;
}

class FixedCorpusCdbProvider final : public fl::ICompileCommandsProvider {
public:
    explicit FixedCorpusCdbProvider(fl::CompileCommandsLocation location) : m_location(std::move(location)) {}

    frost::Result<std::optional<fl::CompileCommandsLocation>> locate() override { return m_location; }

private:
    fl::CompileCommandsLocation m_location;
};

std::optional<std::string> locateFrostEngineCorpusDir() {
    auto& fs = frost::io::realFileSystem();
    const std::string direct = frost::test::environment("FROST_CORPUS_DIR");
    if (!direct.empty() && fs.exists(direct + "/Frost/src/Frost/Core/Application.cpp").value_or(false)) {
        return frost::io::normalizePath(direct).value_or(direct);
    }
    const std::string root = frost::test::environment("FROST_CORPORA_ROOT");
    if (!root.empty()) {
        for (std::string_view sub : {"/frostengine-12b8c6de4c38", "/FrostEngine"}) {
            const std::string candidate = root + std::string(sub);
            if (fs.exists(candidate + "/Frost/src/Frost/Core/Application.cpp").value_or(false)) {
                return frost::io::normalizePath(candidate).value_or(candidate);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> locateCorpusCompileCommands(const std::string& corpusDir) {
    auto& fs = frost::io::realFileSystem();
    const std::string candidate = corpusDir + "/build/" + FROST_CORPUS_FROSTENGINE_PRESET + "/compile_commands.json";
    if (fs.exists(candidate).value_or(false)) {
        return frost::io::normalizePath(candidate).value_or(candidate);
    }
    return std::nullopt;
}

struct CorpusSymbolTarget {
    std::string_view name;
    bool inHeader;
    std::string_view marker;
    std::size_t offset;
};

constexpr CorpusSymbolTarget kCorpusTenSymbols[10] = {
    {"Frost::FrameTimeSource", true, "class FrameTimeSource", 6},
    {"Frost::FramePacing", true, "enum class FramePacing", 11},
    {"Frost::ApplicationSpecification", true, "struct ApplicationSpecification", 7},
    {"Frost::ImGuiFrameHook", true, "class ImGuiFrameHook", 6},
    {"Frost::FrameLimiter", true, "class FrameLimiter", 6},
    {"Frost::FrameLimiter::WaitUntil", true, "void WaitUntil(", 5},
    {"Frost::Application", true, "class Application", 6},
    {"Frost::Application::Run", true, "void Run();", 5},
    {"Frost::Application::PushLayer", true, "Layer* PushLayer(", 7},
    {"Frost::SystemFrameTimeSource", false, "class SystemFrameTimeSource", 6},
};

void checkCpp6Corpus(bool coldCache) {
    frost::test::disableCrashDialogs();
    const auto corpusDirOpt = locateFrostEngineCorpusDir();
    ASSERT_TRUE(corpusDirOpt.has_value()) << "CPP-6 corpus gate requires the pinned FrostEngine clone";
    const std::string& corpusDir = *corpusDirOpt;
    const std::string pinnedSha = FROST_CORPUS_FROSTENGINE_SHA;
    ASSERT_GE(pinnedSha.size(), 12U);
    ASSERT_NE(corpusDir.find(pinnedSha.substr(0, 12)), std::string::npos)
        << "CPP-6 must run against the pinned clone named by corpora.lock.json";
    const auto gitHead = frost::io::realFileSystem().readFile(corpusDir + "/.git/HEAD");
    ASSERT_TRUE(gitHead.has_value()) << "CPP-6 corpus must be a detached clone with readable .git/HEAD";
    std::string actualSha = *gitHead;
    while (!actualSha.empty() && (actualSha.back() == '\r' || actualSha.back() == '\n'))
        actualSha.pop_back();
    ASSERT_EQ(actualSha, pinnedSha) << "CPP-6 corpus directory name does not prove its checkout; compare HEAD";
    frost::test::EngineBuildLock buildLock(corpusDir + ".build.lock");
    ASSERT_TRUE(buildLock.held()) << "could not acquire the pinned corpus build lock";
    const auto rawCdbOpt = locateCorpusCompileCommands(corpusDir);
    ASSERT_TRUE(rawCdbOpt.has_value()) << "CPP-6 corpus gate requires configured compile_commands.json";

    auto& fs = frost::io::realFileSystem();
    const auto rawCdbText = fs.readFile(*rawCdbOpt);
    ASSERT_TRUE(rawCdbText.has_value()) << rawCdbText.error().describe();
    const auto parsedCdb = fl::parseJson(*rawCdbText);
    ASSERT_TRUE(parsedCdb.has_value()) << parsedCdb.error().describe();
    ASSERT_TRUE(parsedCdb->isArray());

    std::vector<std::string> compiledFileKeys;
    for (const Json& entry : parsedCdb->items()) {
        if (coldCache) {
            ASSERT_TRUE(frost::io::isAbsolutePath(entry.stringAt("directory")));
            ASSERT_TRUE(frost::io::isAbsolutePath(entry.stringAt("file")));
        }
        compiledFileKeys.push_back(fl::documentKey(entry.stringAt("file")));
    }
    std::ranges::sort(compiledFileKeys);
    const std::string cdbDir(frost::io::parentPath(*rawCdbOpt));
    std::unique_ptr<frost::test::ScratchDir> scratch;
    std::string selectedCdbDir = cdbDir;
    std::string selectedCdbFile = *rawCdbOpt;
    std::string coldShardDir;
    if (coldCache) {
        scratch = std::make_unique<frost::test::ScratchDir>("cpp6-cold-index-corpus");
        selectedCdbDir = scratch->join("cdb");
        selectedCdbFile = scratch->join("cdb/compile_commands.json");
        coldShardDir = scratch->join("cdb/.cache/clangd/index");
        ASSERT_TRUE(fs.createDirectories(selectedCdbDir).has_value());
        ASSERT_TRUE(fs.writeFileAtomic(selectedCdbFile, *rawCdbText).has_value());
        const auto copied = fs.readFile(selectedCdbFile);
        ASSERT_TRUE(copied.has_value());
        ASSERT_EQ(*copied, *rawCdbText) << "cold indexing must preserve every original command byte";
        const auto cacheExists = fs.exists(coldShardDir);
        ASSERT_TRUE(cacheExists.has_value());
        ASSERT_FALSE(*cacheExists) << "cold indexing requires a fresh shard directory";
    }

    Cpp6Env env;
    ASSERT_TRUE(env.ok());
    auto foundClangd = fl::findClangd(fs, fl::defaultClangdSearch());
    ASSERT_TRUE(foundClangd.has_value()) << foundClangd.error().describe();
    ASSERT_NE(foundClangd->origin, fl::ClangdOrigin::Path) << "the acceptance test must use the pinned clangd";

    fl::ClangdServiceOptions options;
    options.clangdPath = foundClangd->path;
    options.workspaceRoot = corpusDir;
    options.launch.jobs = 0; // ResourceGovernor selects concurrency; its default RSS cap is checked below.
    options.launch.backgroundIndex = true;
    // Both variants use every original command. The shared-cache variant permits warm shards;
    // the cold variant uses a byte-identical database copy to isolate its index storage.
    options.compileCommands = std::make_shared<FixedCorpusCdbProvider>(fl::CompileCommandsLocation{
        .directory = selectedCdbDir,
        .file = selectedCdbFile,
        .source = coldCache ? fl::CompileCommandsSource::CMakeCopy : fl::CompileCommandsSource::Existing,
        .origin = cdbDir,
    });

    auto service = std::make_unique<ClangdService>(frost::io::realProcessLauncher(), env.context(), std::move(options));
#ifdef _WIN32
    const auto wallStarted = std::chrono::steady_clock::now();
#endif
    ASSERT_TRUE(service->start().has_value());
    ASSERT_TRUE(waitForCpp6(
        [&] { return service->state() == ClangdState::Running || service->state() == ClangdState::Failed; }));
    ASSERT_EQ(service->state(), ClangdState::Running);

    const std::string appCpp =
        frost::io::normalizePath(corpusDir + "/Frost/src/Frost/Core/Application.cpp").value_or("");
    const std::string appH = frost::io::normalizePath(corpusDir + "/Frost/src/Frost/Core/Application.h").value_or("");
    ASSERT_TRUE(std::ranges::binary_search(compiledFileKeys, fl::documentKey(appCpp)))
        << "the configured corpus database must contain the opened engine translation unit";
    const auto appCppBytes = fs.readFile(appCpp);
    const auto appHBytes = fs.readFile(appH);
    ASSERT_TRUE(appCppBytes.has_value() && appHBytes.has_value());
    auto appCppDoc = frost::text::Document::fromUtf8(*appCppBytes);
    auto appHDoc = frost::text::Document::fromUtf8(*appHBytes);
    ASSERT_TRUE(appCppDoc.has_value() && appHDoc.has_value());

    ASSERT_TRUE(service->openDocument(appCpp, appCppDoc->text()).has_value());
    ASSERT_TRUE(service->openDocument(appH, appHDoc->text()).has_value());
    ASSERT_TRUE(waitForCpp6(
        [&] {
            return service->diagnosedVersion(appCpp) == service->documentVersion(appCpp) &&
                   service->diagnosedVersion(appH) == service->documentVersion(appH);
        },
        5min))
        << "clangd did not finish analyzing the opened engine files";
    const bool completed = waitForCpp6(
        [&] {
            const fl::IndexMetrics metrics = fl::indexMetrics(*service);
            return metrics.completed && metrics.shardCount > 0;
        },
        20min);
    const auto indexed = fl::indexMetrics(*service);
    std::printf("[CPP-6 corpus] cache=%s TUs=%zu completed=%d elapsedMs=%llu peakRssBytes=%llu capBytes=%llu cdb=%s\n",
                coldCache ? "cold" : "shared", compiledFileKeys.size(), indexed.completed,
                static_cast<unsigned long long>(indexed.elapsedMs),
                static_cast<unsigned long long>(indexed.peakRssBytes),
                static_cast<unsigned long long>(indexed.governorRssCapBytes), selectedCdbDir.c_str());
    std::fflush(stdout);
#ifdef _WIN32
    if (const auto pid = service->serverPid())
        frost::test::reportProcessMemoryEvidence(*pid, coldCache ? "CPP6 cold indexed" : "CPP6 shared indexed",
                                                 indexed.elapsedMs, wallStarted, indexed.governorRssCapBytes,
                                                 indexed.residentMemoryLimitBytes);
    else
        ADD_FAILURE() << "missing live clangd PID for independent memory evidence";
#endif
    ASSERT_TRUE(completed) << "clangd did not finish background indexing the full configured corpus";
    if (coldCache) {
        const auto entries = fs.listDirectory(coldShardDir);
        ASSERT_TRUE(entries.has_value()) << "clangd must write shards in the fresh database's own cache";
        const auto shards = std::ranges::count_if(*entries, [](const frost::io::DirEntry& entry) {
            return entry.type == frost::io::FileType::Regular && entry.name.ends_with(".idx");
        });
        EXPECT_GT(shards, 0) << coldShardDir;
        std::printf("[CPP-6 cold cache] shards=%lld path=%s\n", static_cast<long long>(shards), coldShardDir.c_str());
        std::fflush(stdout);
    }
    EXPECT_TRUE(indexed.peakRssSampleValid) << indexed.peakRssSampleError;
    EXPECT_GT(indexed.peakRssBytes, 0U);
    EXPECT_LT(indexed.peakRssBytes, indexed.governorRssCapBytes);

    const auto rawRequest = [&](std::string method, Json params) {
        auto res = env.run(service->request(std::move(method), std::move(params)));
        EXPECT_TRUE(res.has_value()) << res.error().describe();
        return res.value_or(Json());
    };

    bool foundReferenceInAnotherTranslationUnit = false;
    for (const CorpusSymbolTarget& target : kCorpusTenSymbols) {
        SCOPED_TRACE(target.name);
        const std::string& file = target.inHeader ? appH : appCpp;
        const std::string_view text = target.inHeader ? std::string_view(*appHBytes) : std::string_view(*appCppBytes);
        const fl::LspPosition pos = cpp6FindPosition(text, target.marker, target.offset);

        // Find Usages vs raw textDocument/references oracle.
        Json refParams = fl::textDocumentPosition(file, pos).value();
        refParams.set("context", Json::object().set("includeDeclaration", true));
        const Json rawRefs = rawRequest("textDocument/references", std::move(refParams));
        const auto oracleRefs = cpp6RawReferences(rawRefs);
        ASSERT_FALSE(oracleRefs.empty());
        for (const auto& reference : oracleRefs) {
            if (reference.first != fl::documentKey(appCpp) &&
                std::ranges::binary_search(compiledFileKeys, reference.first)) {
                foundReferenceInAnotherTranslationUnit = true;
            }
        }

        const auto groups = env.run(fl::findUsages(*service, file, pos, {}, true));
        ASSERT_TRUE(groups.has_value()) << groups.error().describe();
        const auto actualRefs = cpp6NormalizeLocations(fl::flattenUsageLocations(*groups));
        EXPECT_EQ(actualRefs, oracleRefs);

        // Call Hierarchy vs raw clangd oracle.
        const Json rawCallPrep =
            rawRequest("textDocument/prepareCallHierarchy", fl::textDocumentPosition(file, pos).value());
        const auto callSnap = env.run(fl::queryCallHierarchy(*service, file, pos));
        ASSERT_TRUE(callSnap.has_value()) << callSnap.error().describe();
        if (!rawCallPrep.isArray() || rawCallPrep.size() == 0) {
            EXPECT_FALSE(callSnap->has_value());
        } else {
            ASSERT_TRUE(callSnap->has_value());
            const Json& rawRoot = rawCallPrep.items()[0];
            EXPECT_EQ((*callSnap)->root.name, rawRoot.stringAt("name"));
            const auto rawRootPath = fl::uriToPath(rawRoot.stringAt("uri"));
            const Json* rawSelection = rawRoot.find("selectionRange");
            ASSERT_TRUE(rawRootPath.has_value() && rawSelection != nullptr);
            EXPECT_EQ(fl::documentKey((*callSnap)->root.path), fl::documentKey(*rawRootPath));
            EXPECT_EQ((*callSnap)->root.selectionRange, cpp6RawRange(*rawSelection));
            const auto rawIncoming = rawRequest("callHierarchy/incomingCalls", Json::object().set("item", rawRoot));
            const auto rawOutgoing = rawRequest("callHierarchy/outgoingCalls", Json::object().set("item", rawRoot));
            EXPECT_EQ(cpp6ActualIncomingEdges((*callSnap)->incoming), cpp6RawEdges(rawIncoming, "from"));
            EXPECT_EQ(cpp6ActualOutgoingEdges((*callSnap)->outgoing), cpp6RawEdges(rawOutgoing, "to"));
        }

        // Type Hierarchy vs raw clangd oracle.
        const Json rawTypePrep =
            rawRequest("textDocument/prepareTypeHierarchy", fl::textDocumentPosition(file, pos).value());
        const auto typeSnap = env.run(fl::queryTypeHierarchy(*service, file, pos));
        ASSERT_TRUE(typeSnap.has_value()) << typeSnap.error().describe();
        if (!rawTypePrep.isArray() || rawTypePrep.size() == 0) {
            EXPECT_FALSE(typeSnap->has_value());
        } else {
            ASSERT_TRUE(typeSnap->has_value());
            const Json& rawRoot = rawTypePrep.items()[0];
            EXPECT_EQ((*typeSnap)->root.name, rawRoot.stringAt("name"));
            const auto rawRootPath = fl::uriToPath(rawRoot.stringAt("uri"));
            const Json* rawSelection = rawRoot.find("selectionRange");
            ASSERT_TRUE(rawRootPath.has_value() && rawSelection != nullptr);
            EXPECT_EQ(fl::documentKey((*typeSnap)->root.path), fl::documentKey(*rawRootPath));
            EXPECT_EQ((*typeSnap)->root.selectionRange, cpp6RawRange(*rawSelection));
            const auto rawSupers = rawRequest("typeHierarchy/supertypes", Json::object().set("item", rawRoot));
            const auto rawSubs = rawRequest("typeHierarchy/subtypes", Json::object().set("item", rawRoot));
            EXPECT_EQ(cpp6ActualTypeEdges((*typeSnap)->supertypes), cpp6RawEdges(rawSupers, {}));
            EXPECT_EQ(cpp6ActualTypeEdges((*typeSnap)->subtypes), cpp6RawEdges(rawSubs, {}));
        }
    }
    EXPECT_TRUE(foundReferenceInAnotherTranslationUnit)
        << "the reference oracle must include an indexed translation unit beyond the opened Application.cpp";
    const auto queried = fl::indexMetrics(*service);
    std::printf("[CPP-6 corpus queries] cache=%s peakRssBytes=%llu capBytes=%llu\n", coldCache ? "cold" : "shared",
                static_cast<unsigned long long>(queried.peakRssBytes),
                static_cast<unsigned long long>(queried.governorRssCapBytes));
    std::fflush(stdout);
    EXPECT_TRUE(queried.peakRssSampleValid) << queried.peakRssSampleError;
    EXPECT_GT(queried.peakRssBytes, 0U);
    EXPECT_LT(queried.peakRssBytes, queried.governorRssCapBytes);
#ifdef _WIN32
    ASSERT_TRUE(queried.residentMemoryLimitBytes.has_value());
    EXPECT_GT(*queried.residentMemoryLimitBytes, 0U);
    EXPECT_LT(*queried.residentMemoryLimitBytes, queried.governorRssCapBytes);
    if (const auto pid = service->serverPid())
        frost::test::reportProcessMemoryEvidence(*pid, coldCache ? "CPP6 cold queried" : "CPP6 shared queried",
                                                 queried.elapsedMs, wallStarted, queried.governorRssCapBytes,
                                                 queried.residentMemoryLimitBytes);
    else
        ADD_FAILURE() << "missing live clangd PID for independent memory evidence";
#endif

    ASSERT_TRUE(service->closeDocument(appH).has_value());
    ASSERT_TRUE(service->closeDocument(appCpp).has_value());
    env.run(service->stop());
    EXPECT_EQ(service->state(), ClangdState::Stopped);
}

TEST(Cpp6NavigationCorpus, TenEngineSymbolsMatchClangdReferenceAndHierarchyOracles) { checkCpp6Corpus(false); }

TEST(Cpp6NavigationCorpus, ColdIndexTenEngineSymbolsMatchClangdReferenceAndHierarchyOracles) { checkCpp6Corpus(true); }

} // namespace
