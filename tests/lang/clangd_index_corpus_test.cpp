// SPDX-License-Identifier: Apache-2.0
//
// Corpus integration test for CPP-4 ("Indexing at scale") and FND-18 ("ResourceGovernor") on the
// pinned FrostEngine clone (docs/adr/0084-cpp4-indexing-at-scale-baseline.md):
//   1. Runs a cold-cache clangd background index of all ~1,289 first-party FrostEngine C/C++
//      translation units (Frost/, Frost-Editor/, Frost-Runtime/, Frost-Services/) from the clone's
//      configured compile_commands.json while a real `cmake --build` runs concurrently under the
//      ResourceGovernor build lock.
//   2. Asserts the background index finishes within the ADR 0084 baseline budget.
//   3. Asserts `.idx` shards are written to `.cache/clangd/index` and `workspace/symbol` resolves
//      cross-TU engine symbols from the background index.
//   4. Asserts peak clangd RSS and combined index + build RSS stay strictly under the
//      ResourceGovernor caps (4096 MiB clangd cap, 8192 MiB combined cap).

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/resource_governor.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fl = frost::lang;
namespace fs_io = frost::io;
namespace fsched = frost::sched;
using namespace std::chrono_literals;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

// Fixed baseline budget recorded in docs/adr/0084-cpp4-indexing-at-scale-baseline.md.
constexpr std::uint64_t kAdr0084BaselineBudgetMs = 720'000ULL;
constexpr std::size_t kAdr0084MinExpectedShards = 3000U;

std::string corpusRoot() { return frost::test::environment("FROST_CORPORA_ROOT"); }

std::string checkedCorpus() {
    const std::string root = corpusRoot();
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus";
        return {};
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const std::string corpus = fs_io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
    auto& fs = fs_io::realFileSystem();
    const auto head = fs.readFile(corpus + "/.git/HEAD");
    if (!head || head->substr(0, 40) != sha) {
        ADD_FAILURE() << corpus << " is missing or not detached at its pin (python tools/corpora.py verify)";
        return {};
    }
    return corpus;
}

std::string clangdPath() {
    auto found = fl::findClangd(fs_io::realFileSystem(), fl::defaultClangdSearch());
    EXPECT_TRUE(found.has_value()) << found.error().describe();
    return found ? found->path : std::string();
}

// Whether `file` belongs to the first-party FrostEngine C/C++ translation units (`Frost/`,
// `Frost-Editor/`, `Frost-Runtime/`, `Frost-Services/`), excluding `.cache/cpm/`, `ThirdParty/`,
// `build/` generated sources, `Frost-Tests/`, and non-C/C++ `.rc`/`.asm` files.
[[nodiscard]] bool isFirstPartyEngineTranslationUnit(std::string_view file) noexcept {
    if (file.ends_with(".rc") || file.ends_with(".asm")) {
        return false;
    }
    std::string normalized(file);
    std::ranges::replace(normalized, '\\', '/');
    if (normalized.find("/.cache/") != std::string::npos || normalized.find("/ThirdParty/") != std::string::npos ||
        normalized.find("/build/") != std::string::npos || normalized.find("/Frost-Tests/") != std::string::npos) {
        return false;
    }
    return normalized.find("/Frost/") != std::string::npos || normalized.find("/Frost-Editor/") != std::string::npos ||
           normalized.find("/Frost-Runtime/") != std::string::npos ||
           normalized.find("/Frost-Services/") != std::string::npos;
}

class FixedCompileCommandsProvider final : public fl::ICompileCommandsProvider {
public:
    explicit FixedCompileCommandsProvider(fl::CompileCommandsLocation location) : m_location(std::move(location)) {}

    frost::Result<std::optional<fl::CompileCommandsLocation>> locate() override { return m_location; }

private:
    fl::CompileCommandsLocation m_location;
};

TEST(ClangdIndexCorpus, FullBackgroundIndexUnderGovernorAndConcurrentBuildStaysWithinBaseline) {
    frost::test::disableCrashDialogs();
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());

    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const std::string lockPath =
        fs_io::normalizePath(corpusRoot() + "/frostengine-" + sha.substr(0, 12) + ".build.lock").value_or("");
    ASSERT_FALSE(lockPath.empty());

    auto& fs = fs_io::realFileSystem();
    const std::string buildDir =
        fs_io::normalizePath(corpus + "/build/" + FROST_CORPUS_FROSTENGINE_PRESET).value_or("");
    const std::string rawCdbPath = fs_io::joinPath(buildDir, "compile_commands.json").value_or("");
    ASSERT_TRUE(fs.exists(rawCdbPath).value_or(false))
        << "the preset was never configured: python tools/corpora.py engine --configure";

    // Configure ResourceGovernor with the corpus build lock file and ADR 0084 RSS caps.
    fsched::ResourceGovernorOptions govOptions;
    govOptions.lockFilePath = lockPath;
    govOptions.clangdRssCapBytes = 4096ULL * kMiB;
    govOptions.combinedRssCapBytes = 8192ULL * kMiB;
    govOptions.maxClangdJobs = 12;
    govOptions.throttledClangdJobs = 8;
    govOptions.maxBuildJobs = 4;
    auto governor = std::make_shared<fsched::ResourceGovernor>(govOptions);

    // Acquire the Build leg lease (locks <corpora>/frostengine-<sha12>.build.lock and throttles
    // clangd + cmake --build so both fit comfortably under the RAM cap).
    auto buildLease = governor->acquireHeavyLeg(fsched::HeavyLegKind::Build, 30min);
    ASSERT_TRUE(buildLease.has_value()) << buildLease.error().describe();
    EXPECT_TRUE(governor->isLegActive(fsched::HeavyLegKind::Build));

    // Read the engine's compile_commands.json and materialize the owned CDB in a fresh scratch
    // directory so `.cache/clangd/index` is built cold on every run and never pollutes the clone.
    const auto rawCdbText = fs.readFile(rawCdbPath);
    ASSERT_TRUE(rawCdbText.has_value()) << rawCdbText.error().describe();
    const auto parsedCdb = fl::parseJson(*rawCdbText);
    ASSERT_TRUE(parsedCdb.has_value()) << parsedCdb.error().describe();
    ASSERT_TRUE(parsedCdb->isArray());

    fl::Json filteredEntries = fl::Json::array();
    std::size_t tuCount = 0;
    for (const fl::Json& item : parsedCdb->items()) {
        const std::string_view file = item.stringAt("file");
        if (isFirstPartyEngineTranslationUnit(file)) {
            filteredEntries.push(item);
            ++tuCount;
        }
    }
    ASSERT_GE(tuCount, 1200U) << "expected all ~1,289 first-party FrostEngine translation units";

    frost::test::ScratchDir scratch("cpp4-clangd-index-corpus");
    const std::string ownedCdbDir = scratch.join("cdb");
    const std::string ownedCdbFile = scratch.join("cdb/compile_commands.json");
    ASSERT_TRUE(fs.createDirectories(ownedCdbDir).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(ownedCdbFile, filteredEntries.dump() + "\n").has_value());

    auto reactor = fs_io::Reactor::create(fs_io::ReactorOptions{1, "corpus-cpp4-index"});
    ASSERT_TRUE(reactor.has_value()) << reactor.error().describe();
    fsched::ThreadPool pool(fsched::ThreadPoolOptions{.workerCount = 4});
    const fs_io::AsyncContext context{**reactor, pool};

    const auto runTask = [&]<class T>(fsched::Task<T> task) {
        return pool.spawn(fsched::Lane::UserVisible, std::move(task)).join();
    };

    fl::ClangdServiceOptions serviceOptions;
    serviceOptions.clangdPath = clangdPath();
    serviceOptions.workspaceRoot = corpus;
    serviceOptions.launch.backgroundIndex = true;
    serviceOptions.launch.jobs = 0; // Governed by ResourceGovernor.
    serviceOptions.compileCommands = std::make_shared<FixedCompileCommandsProvider>(fl::CompileCommandsLocation{
        .directory = ownedCdbDir,
        .file = ownedCdbFile,
        .source = fl::CompileCommandsSource::CMakeCopy,
        .origin = buildDir,
    });

    std::atomic<int> lastLoggedBucket{-1};
    serviceOptions.onProgress = [&lastLoggedBucket](const fl::ClangdProgress& p) {
        if (p.kind == fl::ClangdProgress::Kind::Begin) {
            std::printf("[CPP-4 progress] begin title=%s msg=%s\n", p.title.c_str(), p.message.c_str());
            std::fflush(stdout);
        } else if (p.kind == fl::ClangdProgress::Kind::Report && p.percentage.has_value()) {
            const int bucket = *p.percentage / 10;
            const int prev = lastLoggedBucket.exchange(bucket);
            if (bucket != prev) {
                std::printf("[CPP-4 progress] %d%% (%s)\n", *p.percentage, p.message.c_str());
                std::fflush(stdout);
            }
        } else if (p.kind == fl::ClangdProgress::Kind::End) {
            std::printf("[CPP-4 progress] end\n");
            std::fflush(stdout);
        }
    };

    auto service =
        std::make_unique<fl::ClangdService>(fs_io::realProcessLauncher(), context, std::move(serviceOptions));
    fl::setResourceGovernor(*service, governor);
    ASSERT_TRUE(service->start().has_value());

    const auto waitDeadline = std::chrono::steady_clock::now() + 120s;
    while (service->state() != fl::ClangdState::Running && std::chrono::steady_clock::now() < waitDeadline) {
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_EQ(service->state(), fl::ClangdState::Running);

    // Open an engine source to trigger compilation database loading and background indexing, while
    // simultaneously launching a real `cmake --build` under the held build lock.
    const std::string entryFile = fs_io::normalizePath(corpus + "/Frost/src/Frost/Core/Application.cpp").value_or("");
    const auto entryBytes = fs.readFile(entryFile);
    ASSERT_TRUE(entryBytes.has_value()) << entryBytes.error().describe();
    auto entryDoc = frost::text::Document::fromUtf8(*entryBytes);
    ASSERT_TRUE(entryDoc.has_value()) << entryDoc.error().describe();
    ASSERT_TRUE(service->openDocument(entryFile, entryDoc->text()).has_value());

    // Launch a concurrent `cmake --build` on the engine build directory under the governor's build job cap.
    fs_io::ProcessSpec buildSpec;
    buildSpec.executable = FROST_TEST_CMAKE;
    buildSpec.arguments = {
        "--build", buildDir, "--target", "FrostFoundation", "-j", std::to_string(governor->buildJobs()),
    };
    buildSpec.workingDirectory = corpus;
    buildSpec.stdoutMode = fs_io::Stdio::Null;
    buildSpec.stderrMode = fs_io::Stdio::Null;
    buildSpec.killOnClose = true;

    auto buildProcResult = fs_io::realProcessLauncher().launchAsync(buildSpec, context);
    ASSERT_TRUE(buildProcResult.has_value()) << buildProcResult.error().describe();
    std::shared_ptr<fs_io::IAsyncProcess> buildProc(std::move(*buildProcResult));
    const auto buildPid = static_cast<std::int64_t>(buildProc->pid());
    if (buildPid > 0) {
        (void)governor->sampleProcessRss(buildPid, fsched::HeavyLegKind::Build);
    }

    // Poll until background indexing finishes while sampling both clangd and cmake --build RSS.
    const auto indexDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kAdr0084BaselineBudgetMs * 2);
    while (std::chrono::steady_clock::now() < indexDeadline) {
        if (buildPid > 0) {
            (void)governor->sampleProcessRss(buildPid, fsched::HeavyLegKind::Build);
        }
        const fl::IndexMetrics m = fl::indexMetrics(*service);
        if (m.completed && m.shardCount > 0) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    // Reap the concurrent `cmake --build` process.
    const auto buildExit = runTask(buildProc->exited());
    governor->unregisterProcess(buildPid);
    ASSERT_TRUE(buildExit.has_value()) << buildExit.error().describe();
    EXPECT_EQ(buildExit->exitCode, 0);

    const fl::IndexMetrics metrics = fl::indexMetrics(*service);
    std::printf("[CPP-4 corpus] TUs=%zu completed=%d elapsedMs=%llu shards=%zu clangdPeakRssMiB=%.1f "
                "buildPeakRssMiB=%.1f combinedPeakRssMiB=%.1f capMiB=%.1f\n",
                tuCount, metrics.completed ? 1 : 0, static_cast<unsigned long long>(metrics.elapsedMs),
                metrics.shardCount, static_cast<double>(metrics.peakRssBytes) / static_cast<double>(kMiB),
                static_cast<double>(governor->peakLegRssBytes(fsched::HeavyLegKind::Build)) / static_cast<double>(kMiB),
                static_cast<double>(governor->peakCombinedRssBytes()) / static_cast<double>(kMiB),
                static_cast<double>(metrics.governorRssCapBytes) / static_cast<double>(kMiB));
    std::fflush(stdout);

    // 1. Background index finishes within the ADR 0084 baseline time budget.
    EXPECT_TRUE(metrics.completed);
    EXPECT_GT(metrics.elapsedMs, 0ULL);
    EXPECT_LE(metrics.elapsedMs, kAdr0084BaselineBudgetMs);

    // 2. Shards are written on disk and workspace/symbol resolves cross-TU engine symbols.
    EXPECT_GE(metrics.shardCount, kAdr0084MinExpectedShards);

    // Query a symbol defined in another indexed TU (UUID.cpp / Assert.cpp / Input.cpp, not Application.cpp).
    bool foundCrossTuSymbol = false;
    for (int attempt = 0; attempt < 20 && !foundCrossTuSymbol; ++attempt) {
        const auto symbols = runTask(fl::requestWorkspaceSymbols(*service, "UUID"));
        ASSERT_TRUE(symbols.has_value()) << symbols.error().describe();
        for (const fl::WorkspaceSymbol& sym : *symbols) {
            if (sym.name.find("UUID") != std::string::npos && !sym.location.path.empty()) {
                foundCrossTuSymbol = true;
                break;
            }
        }
        if (!foundCrossTuSymbol) {
            std::this_thread::sleep_for(100ms);
        }
    }
    EXPECT_TRUE(foundCrossTuSymbol) << "workspace/symbol did not resolve Frost::UUID from the background index";

    // 3. Peak clangd RSS and combined index + build RSS stay strictly under the ResourceGovernor caps.
    EXPECT_GT(metrics.peakRssBytes, 0ULL);
    EXPECT_LT(metrics.peakRssBytes, governor->clangdRssCapBytes());
    EXPECT_LT(governor->peakCombinedRssBytes(), governor->combinedRssCapBytes());
    if (const auto clangdPid = service->serverPid()) {
        EXPECT_FALSE(governor->isProcessOverRssCap(static_cast<std::int64_t>(*clangdPid)));
    }
    EXPECT_FALSE(governor->isCombinedOverRssCap());

    ASSERT_TRUE(service->closeDocument(entryFile).has_value());
    runTask(service->stop());
    service.reset();
    buildLease->release();
    (*reactor)->shutdown();
    pool.shutdown();
}

} // namespace
