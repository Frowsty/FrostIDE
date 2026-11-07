// SPDX-License-Identifier: Apache-2.0
//
// The corpus tier (label `corpus`; verify -Corpus only, ADR 0077 decision 7, ADR 0078): the R0 exit
// criterion on the pinned FrostEngine clone (tests/corpus/corpora.lock.json, fetched by
// tools/corpora.py into <corpora root>/frostengine-<sha12>). Opening that folder as a trusted CMake
// project, with the real cmake, ninja, MSVC environment (Windows) and the pinned clangd, must record
// no GUI stall above 250 ms (the interim stall policy of FND-2), start exactly one clangd process
// and negotiate the LSP 3.17 position encoding (clangd answers capabilities.positionEncoding).
//
// A missing corpus, or one that is not at its pin, fails the test: the tier never skips. Stalls are
// measured with the product's own StallWatchdog and StallHooks on this GUI thread (the hooks the
// frost program installs), from the moment the folder is opened, inside a real blocking event loop,
// until configure, clangd and the first opened engine source have settled. The corpus is the only
// tree read; the configure writes its build tree and CPM download cache inside the clone, which
// tools/corpora.py verify allows (untracked files).

#include "frost/app/diagnostics_qt.hpp"
#include "frost/build/cmake_service.hpp"
#include "frost/core/clock.hpp"
#include "frost/diag/stall_watchdog.hpp"
#include "frost/diag/thread_sampler.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/rpc/json.hpp"

#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

// The longest GUI stall the R0 exit allows (PLAN.md section 7, R0; FND-2's interim policy).
constexpr auto kStallLimit = 250ms;

// <root>/frostengine-<sha12>: root is $FROST_CORPORA_ROOT, else tools/corpora.py's default.
std::string corpusDirectory() {
    std::string root = qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString();
    if (root.empty()) {
        if constexpr (frost::io::kNativePathStyle == frost::io::PathStyle::Windows)
            root = "E:/FrostBuilds/corpora";
        else
            root = qEnvironmentVariable("HOME").toStdString() + "/FrostBuilds/corpora";
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    return frost::io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
}

// The real launcher, counting the clangd processes it starts.
class CountingLauncher final : public frost::io::IProcessLauncher {
public:
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        count(spec);
        return frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        count(spec);
        return frost::io::realProcessLauncher().launchAsync(spec, context);
    }
    [[nodiscard]] int clangdStarts() const { return m_clangd.load(); }
    [[nodiscard]] std::string describe() const {
        const std::scoped_lock lock(m_mutex);
        std::string text;
        for (const std::string& exe : m_executables)
            text += "\n  " + exe;
        return text;
    }

private:
    void count(const frost::io::ProcessSpec& spec) {
        const std::string_view name = frost::io::fileName(spec.executable);
        if (name == "clangd" || name == "clangd.exe")
            ++m_clangd;
        const std::scoped_lock lock(m_mutex);
        m_executables.push_back(spec.executable);
    }
    std::atomic<int> m_clangd{0};
    mutable std::mutex m_mutex;
    std::vector<std::string> m_executables;
};

/*
 * Runs a real, blocking event loop until `done` or `timeout`, checking `done` every 50 ms from a
 * timer (as the frost program's loop would run). Unlike waitFor's processEvents polling, the loop
 * blocks between events, so the stall watchdog sees idle time as idle and a long handler as a stall.
 * Returns the evaluation that ended the loop.
 */
bool runUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    QEventLoop loop;
    bool reached = false;
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done()) {
            reached = true;
            loop.quit();
        }
    });
    QTimer::singleShot(timeout, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return reached;
}

std::string ms(frost::IClock::Duration duration) {
    return std::format("{} ms", std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

TEST(CorpusFrostEngine, OpeningThePinnedCloneRecordsNoStallOver250msStartsOneClangdAndNegotiatesPositionEncoding) {
    frost::test::disableCrashDialogs(); // clangd and the compilers inherit the error mode.
    const std::string corpus = corpusDirectory();
    auto& fs = frost::io::realFileSystem();
    ASSERT_TRUE(fs.exists(corpus + "/CMakePresets.json").value_or(false))
        << "the pinned FrostEngine corpus is missing at " << corpus
        << " (python tools/corpora.py fetch); the corpus tier fails instead of skipping";
    const auto head = fs.readFile(corpus + "/.git/HEAD");
    ASSERT_TRUE(head.has_value()) << corpus << " is not a git clone";
    ASSERT_EQ(head->substr(0, 40), std::string(FROST_CORPUS_FROSTENGINE_SHA))
        << corpus << " is not detached at its pin (python tools/corpora.py verify)";
    auto found = frost::lang::findClangd(fs, frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();

    CountingLauncher launcher;
    RigOptions options;
    options.workbench.clangd = true;
    options.launcher = &launcher;
    WorkDir profileDir("corpus-frostengine-profile");
    // Freeze reports outlive the test (the work directory is removed), so a stall can be read back.
    const std::string freezes =
        frost::io::normalizePath(std::string(FROST_APP_WORK_ROOT) + "/corpus-freezes").value_or("");
    frost::test::removeTree(fs, freezes); // Only this run's reports.
    ASSERT_TRUE(fs.createDirectories(freezes).has_value()) << freezes;
    frost::diag::StallStats stats;
    std::optional<frost::diag::StallWatchdog::ReportInfo> report;
    bool measured = false;
    // The stall verdict, also when a later step fails: what was measured until then still counts.
    struct StallVerdict {
        const frost::diag::StallStats& stats;
        const std::optional<frost::diag::StallWatchdog::ReportInfo>& report;
        const bool& measured;
        ~StallVerdict() {
            if (!measured)
                return;
            std::cout << std::format("[corpus] GUI stalls over {} while opening FrostEngine: {}, longest {}, "
                                     "unreliable {}, reports {} (dropped {})\n",
                                     ms(frost::diag::kStallThreshold), stats.stalls, ms(stats.longest),
                                     stats.unreliable, stats.reportsWritten, stats.reportsDropped);
            EXPECT_LE(stats.longest, kStallLimit)
                << "a GUI stall of " << ms(stats.longest) << " while opening the corpus; the latest freeze report: "
                << (report ? report->path : std::string("none"));
        }
    } verdict{stats, report, measured};
    {
        WorkbenchRig rig(profileDir, options);
        ASSERT_TRUE(rig.trust->trust(corpus).has_value());
        rig.show();

        // The product's stall instrumentation on this GUI thread, as the frost program installs it,
        // started after the window exists (start-up is B1's business, not this criterion's).
        auto sampler = frost::diag::createSamplerForCurrentThread();
        ASSERT_TRUE(sampler.has_value()) << sampler.error().describe();
        frost::diag::StallWatchdog::Options watch;
        watch.freezesDirectory = freezes;
        watch.version = "corpus-test";
        auto started =
            frost::diag::StallWatchdog::start(std::move(watch), std::move(*sampler), fs, frost::systemClock());
        ASSERT_TRUE(started.has_value()) << started.error().describe();
        std::unique_ptr<frost::diag::StallWatchdog> watchdog = std::move(*started);
        auto hooks = std::make_unique<frost::app::StallHooks>(*QCoreApplication::instance(), *watchdog);
        frost::diag::StallWatchdog::install(watchdog.get()); // ScopedUiIdle sections count as idle.
        const auto stopWatching = [&] {
            if (!watchdog)
                return;
            frost::diag::StallWatchdog::install(nullptr);
            hooks.reset();
            watchdog->stop();
            stats = watchdog->stats();
            report = watchdog->lastReport();
            measured = true;
            watchdog.reset();
        };
        struct StopAtExit {
            std::function<void()> stop;
            ~StopAtExit() { stop(); }
        } stopAtExit{stopWatching};

        // Arm the detector (its first aboutToBlock), then open the folder from inside the loop.
        static_cast<void>(runUntil([] { return false; }, 200ms));
        frost::Result<void> opened = frost::fail(frost::ErrorCode::Cancelled, "not opened");
        QTimer::singleShot(0, [&] { opened = rig.workbench->openPath(corpus); });
        ASSERT_TRUE(runUntil([&] { return rig.workbench->projectRoot() == corpus; }, 60s)) << "the open did not run";
        ASSERT_TRUE(opened.has_value()) << opened.error().describe();

        auto* service = rig.workbench->cmakeService();
        ASSERT_NE(service, nullptr) << "the engine folder opens as a CMake project";
        const auto active = service->activeProfile();
        ASSERT_TRUE(active.has_value());
        EXPECT_EQ(active->name, FROST_CORPUS_FROSTENGINE_PRESET) << "the default profile is the pinned preset";
        const std::string buildDir = service->activeBuildDirectory();
        // A first configure downloads the engine's pinned dependencies (CPM) and captures the MSVC
        // environment; later ones reuse both. clangd waits for the database the configure writes.
        const bool configured = runUntil(
            [&] {
                const auto* clangd = rig.workbench->clangd();
                const auto location = clangd ? clangd->compileCommands() : std::nullopt;
                return clangd && clangd->state() == frost::lang::ClangdState::Running && location &&
                       location->directory == buildDir && service->state() == frost::build::BuildState::Idle;
            },
            40min);
        const auto outcome = service->lastConfigure();
        ASSERT_TRUE(configured) << "clangd never ran on " << buildDir << "; configure: "
                                << (outcome ? (outcome->error ? outcome->error->describe() : "succeeded") : "none")
                                << "; launches:" << launcher.describe();
        ASSERT_TRUE(outcome.has_value() && outcome->succeeded())
            << (outcome && outcome->error ? outcome->error->describe() : "no configure outcome");

        // Open an engine source as the user would and let clangd parse it (semantic tokens arrive).
        const std::string source =
            frost::io::normalizePath(corpus + "/Frost/src/Frost/Core/Application.cpp").value_or("");
        ASSERT_TRUE(fs.exists(source).value_or(false)) << source;
        const std::string& file = source;
        frost::Result<void> shown = frost::fail(frost::ErrorCode::Cancelled, "not opened");
        QTimer::singleShot(0, [&] { shown = rig.workbench->openPath(file); });
        ASSERT_TRUE(runUntil(
            [&] {
                auto* view = rig.workbench->currentView();
                return view && view->path() == file && !view->editor().semanticTokens().empty();
            },
            10min))
            << "clangd sent no semantic tokens for " << file << "; the current tab is "
            << (rig.workbench->currentView() ? rig.workbench->currentView()->path() : std::string("none"));
        ASSERT_TRUE(shown.has_value()) << shown.error().describe();
        // Queued model and state events, and clangd's background indexing, for a while longer: a late
        // restart or a stall they cause is counted too.
        static_cast<void>(runUntil([] { return false; }, 20s));

        // Exactly one clangd for the whole open, and it negotiated the LSP 3.17 position encoding.
        EXPECT_EQ(launcher.clangdStarts(), 1) << "launches:" << launcher.describe();
        auto* clangd = rig.workbench->clangd();
        ASSERT_NE(clangd, nullptr);
        const auto capabilities = clangd->serverCapabilities();
        ASSERT_TRUE(capabilities.has_value());
        const auto* encoding = capabilities->raw.find("positionEncoding");
        ASSERT_NE(encoding, nullptr) << "clangd's capabilities name no positionEncoding: " << capabilities->raw.dump();
        ASSERT_NE(encoding->asString(), nullptr);
        EXPECT_EQ(*encoding->asString(), frost::lang::toString(clangd->encoding()))
            << "the client did not adopt the encoding clangd answered";
        EXPECT_EQ(*encoding->asString(), "utf-8") << "FrostIDE offers utf-8 first; clangd 23 supports it";

        stopWatching(); // Before the rig's shutdown, which is not part of the open.
    }
}

} // namespace
