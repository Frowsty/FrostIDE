// SPDX-License-Identifier: Apache-2.0
//
// The corpus tier (label `corpus`; verify -Corpus only, ADR 0077 decision 7, ADR 0078): the R0 exit
// criterion on the pinned FrostEngine clone (tests/corpus/corpora.lock.json, fetched by
// tools/corpora.py into <corpora root>/frostengine-<sha12>). Opening that folder as a trusted CMake
// project, with the real cmake, ninja, MSVC environment (Windows) and the pinned clangd, must record
// no GUI stall above 250 ms (the interim stall policy of FND-2), start exactly one clangd process
// and negotiate the LSP 3.17 position encoding (clangd answers capabilities.positionEncoding).
// Cloning a real repository (that clone's whole history, over git's file:// transport, with the git
// the product resolves) through the workbench's Clone, until the clone is open, must record no GUI
// stall above 250 ms either (FND-9a's acceptance).
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
#include <QUrl>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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

/*
 * <root>/frostengine-<sha12>, where root is $FROST_CORPORA_ROOT. The corpus tier of the verify
 * scripts sets it from `tools/corpora.py root`, the one place that knows the default root, so this
 * test names no developer machine path (FND-10). Empty when the variable is unset; the test then
 * fails and says how to run it.
 */
std::string corpusDirectory() {
    const std::string root = qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString();
    if (root.empty())
        return {};
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

/*
 * The product's stall instrumentation on this GUI thread, as the frost program installs it
 * (StallWatchdog with this thread's sampler, StallHooks, the ScopedUiIdle hook), writing freeze
 * reports into <work root>/<freezesName> (emptied first and kept after the test, so a stall can be
 * read back). stop() ends the measurement and keeps its statistics. The verdict (no stall over
 * kStallLimit) is checked when the meter is destroyed after a measurement, also when a later step
 * of the test failed: what was measured until then still counts.
 */
class StallMeter {
public:
    StallMeter(frost::io::IFileSystem& fs, std::string what, const std::string& freezesName)
        : m_what(std::move(what)),
          m_freezes(frost::io::normalizePath(std::string(FROST_APP_WORK_ROOT) + "/" + freezesName).value_or("")) {
        frost::test::removeTree(fs, m_freezes); // Only this run's reports.
        if (!fs.createDirectories(m_freezes).has_value()) {
            ADD_FAILURE() << "cannot create " << m_freezes;
            return;
        }
        auto sampler = frost::diag::createSamplerForCurrentThread();
        if (!sampler) {
            ADD_FAILURE() << sampler.error().describe();
            return;
        }
        frost::diag::StallWatchdog::Options watch;
        watch.freezesDirectory = m_freezes;
        watch.version = "corpus-test";
        auto started =
            frost::diag::StallWatchdog::start(std::move(watch), std::move(*sampler), fs, frost::systemClock());
        if (!started) {
            ADD_FAILURE() << started.error().describe();
            return;
        }
        m_watchdog = std::move(*started);
        m_hooks = std::make_unique<frost::app::StallHooks>(*QCoreApplication::instance(), *m_watchdog);
        frost::diag::StallWatchdog::install(m_watchdog.get()); // ScopedUiIdle sections count as idle.
    }
    StallMeter(const StallMeter&) = delete ("owns the installed watchdog");
    StallMeter& operator=(const StallMeter&) = delete ("owns the installed watchdog");
    ~StallMeter() {
        stop();
        if (!m_measured)
            return;
        std::cout << std::format("[corpus] GUI stalls over {} while {}: {}, longest {}, unreliable {}, reports {} "
                                 "(dropped {})\n",
                                 ms(frost::diag::kStallThreshold), m_what, m_stats.stalls, ms(m_stats.longest),
                                 m_stats.unreliable, m_stats.reportsWritten, m_stats.reportsDropped);
        EXPECT_LE(m_stats.longest, kStallLimit)
            << "a GUI stall of " << ms(m_stats.longest) << " while " << m_what
            << "; the latest freeze report: " << (m_report ? m_report->path : std::string("none"));
    }

    [[nodiscard]] bool running() const { return m_watchdog != nullptr; }

    // Ends the measurement (call it before a rig's shutdown, which is not part of what is measured).
    void stop() {
        if (!m_watchdog)
            return;
        frost::diag::StallWatchdog::install(nullptr);
        m_hooks.reset();
        m_watchdog->stop();
        m_stats = m_watchdog->stats();
        m_report = m_watchdog->lastReport();
        m_measured = true;
        m_watchdog.reset();
    }

private:
    std::string m_what;
    std::string m_freezes;
    std::unique_ptr<frost::diag::StallWatchdog> m_watchdog;
    std::unique_ptr<frost::app::StallHooks> m_hooks;
    frost::diag::StallStats m_stats;
    std::optional<frost::diag::StallWatchdog::ReportInfo> m_report;
    bool m_measured = false;
};

// git writes its object files read-only, which Windows refuses to delete: makes the tree at `root`
// writable when destroyed, before the WorkDir holding it removes it.
struct WritableOnExit {
    std::string root;
    ~WritableOnExit() {
        auto path = frost::io::toStdPath(root);
        if (!path || root.empty())
            return;
        std::error_code error;
        for (auto it = std::filesystem::recursive_directory_iterator(*path, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            std::error_code ignored;
            if (!it->is_symlink(ignored))
                std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write,
                                             std::filesystem::perm_options::add, ignored);
        }
    }
};

// The pinned corpus, checked: present, a git clone, detached at its pin. "" (with a failure added)
// otherwise: the corpus tier fails instead of skipping.
std::string checkedCorpus() {
    const std::string corpus = corpusDirectory();
    if (corpus.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus or "
                         "tools/verify.sh --corpus, which set it from tools/corpora.py root";
        return {};
    }
    auto& fs = frost::io::realFileSystem();
    if (!fs.exists(corpus + "/CMakePresets.json").value_or(false)) {
        ADD_FAILURE() << "the pinned FrostEngine corpus is missing at " << corpus
                      << " (python tools/corpora.py fetch); the corpus tier fails instead of skipping";
        return {};
    }
    const auto head = fs.readFile(corpus + "/.git/HEAD");
    if (!head) {
        ADD_FAILURE() << corpus << " is not a git clone";
        return {};
    }
    if (head->substr(0, 40) != std::string(FROST_CORPUS_FROSTENGINE_SHA)) {
        ADD_FAILURE() << corpus << " is not detached at its pin (python tools/corpora.py verify)";
        return {};
    }
    return corpus;
}

TEST(CorpusFrostEngine, OpeningThePinnedCloneRecordsNoStallOver250msStartsOneClangdAndNegotiatesPositionEncoding) {
    frost::test::disableCrashDialogs(); // clangd and the compilers inherit the error mode.
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());
    auto& fs = frost::io::realFileSystem();
    auto found = frost::lang::findClangd(fs, frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();

    CountingLauncher launcher;
    RigOptions options;
    options.workbench.clangd = true;
    options.launcher = &launcher;
    WorkDir profileDir("corpus-frostengine-profile");
    {
        WorkbenchRig rig(profileDir, options);
        ASSERT_TRUE(rig.trust->trust(corpus).has_value());
        rig.show();

        // Started after the window exists (start-up is B1's business, not this criterion's).
        StallMeter meter(fs, "opening FrostEngine", "corpus-freezes");
        ASSERT_TRUE(meter.running());

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

        meter.stop(); // Before the rig's shutdown, which is not part of the open.
    }
}

/*
 * FND-9a: cloning a real repository records no GUI stall above 250 ms. The repository is the pinned
 * corpus clone (FrostEngine's history, about 70 MB packed and 6000 files), cloned over git's file://
 * transport, which runs the pack protocol a network clone runs (upload-pack, index-pack, checkout)
 * instead of hard-linking objects; the git is the one the product resolves (vcs::findGit). The
 * measurement runs from Clone until the clone has opened as a project (an untrusted CMake folder:
 * nothing runs, the trust prompt shows) and the queued open events have settled.
 */
TEST(CorpusFrostEngine, CloningARealRepositoryRecordsNoStallOver250msAndOpensTheClone) {
    frost::test::disableCrashDialogs(); // git and its helpers inherit the error mode.
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());
    auto& fs = frost::io::realFileSystem();
    QString url = QUrl::fromLocalFile(QString::fromStdString(corpus)).toString(QUrl::FullyEncoded);
    ASSERT_TRUE(url.startsWith(QStringLiteral("file://"))) << url.toStdString();

    WorkDir dir("corpus-clone");
    const std::string target = dir.join("FrostEngine");
    const WritableOnExit writable{target};
    {
        WorkbenchRig rig(dir);
        rig.show();
        StallMeter meter(fs, "cloning FrostEngine", "corpus-clone-freezes");
        ASSERT_TRUE(meter.running());
        static_cast<void>(runUntil([] { return false; }, 200ms)); // Arms the detector.
        frost::Result<void> started = frost::fail(frost::ErrorCode::Cancelled, "not started");
        QTimer::singleShot(0, [&] { started = rig.workbench->cloneRepository(url.toStdString(), target); });
        ASSERT_TRUE(runUntil([&] { return rig.workbench->cloneRunning() || !started.has_value(); }, 60s));
        ASSERT_TRUE(started.has_value()) << started.error().describe();
        ASSERT_TRUE(runUntil([&] { return !rig.workbench->cloneRunning(); }, 20min)) << "the clone never ended";
        const auto& log = rig.window->notifications().log();
        ASSERT_EQ(rig.workbench->projectRoot(), target)
            << "the clone did not open; the last notification: "
            << (log.empty() ? std::string("none") : log.back().notification.text.toStdString());
        // git's own checkout, compared with the source's tracked files at the pin.
        EXPECT_EQ(fs.readFile(target + "/CMakePresets.json").value_or("<missing>"),
                  fs.readFile(corpus + "/CMakePresets.json").value_or("<missing in the corpus>"));
        const auto head = fs.readFile(target + "/.git/HEAD");
        ASSERT_TRUE(head.has_value());
        EXPECT_TRUE(head->starts_with("ref: ") || head->starts_with(FROST_CORPUS_FROSTENGINE_SHA)) << *head;
        // The open's queued model, explorer and trust-prompt events, for a while longer.
        static_cast<void>(runUntil([] { return false; }, 5s));
        meter.stop(); // Before the rig's shutdown, which is not part of the clone.
    }
}

} // namespace
