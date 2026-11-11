// SPDX-License-Identifier: Apache-2.0
//
// The corpus tier (label `corpus`; verify -Corpus only) for R1's UX-23 and FEC-10a exit lines
// (PLAN.md section 7, R1) on the pinned FrostEngine clone:
//   - UX-23: Find in Files' scanned files, the project tree's files (the project index behind Go
//     to File) and the polling watcher's watched files over the clone, with the project's directory
//     roles, each equal `git ls-files --cached --others --exclude-standard` (git is the oracle),
//     plus, for the watcher, the files git lists as ignored outside build/ and .cache/ (the watcher
//     follows directory roles, not .gitignore, so an open ignored file still gets external-change
//     detection). A spying file system records every folder the search and the project scan list:
//     none is under build/, .cache/ or .git/. The native watcher reports a change to a tracked file
//     and nothing written below build/ or .cache/.
//   - FEC-10a: the engine's generated scripting reference gives the globals; real LuaLS on scripts
//     of the clone's MiniMMO sample reports no undefined-global for them, while a misspelled global
//     is still flagged, both in the clone (the script's text plus one misspelled line, opened in
//     the editor buffer only, the disk untouched) and in a temp copy of the sample on disk.
//   - LUA-3/LUA-13 (R4): in a temp copy of the sample, configured through the host registry with
//     FrostEngine's provider, a module created after LuaLS started resolves in require without a
//     restart, and a runtime change reaches the same LuaLS process.
//
// A missing corpus fails the test (the tier never skips). The clone is only read, apart from two
// probe files written below its git-ignored build/ and .cache/ folders and removed again, and one
// tracked file's modification time, set back afterwards (its content never changes).

#include "frost/app/workbench.hpp"
#include "frost/document/load_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/frost_engine_assets.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/lua_host.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/directory_roles.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/task.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/search/find_in_files.hpp"
#include "frost/search/search_scope.hpp"
#include "frost/settings/store.hpp"
#include "frost/text/document.hpp"
#include "frost/ui/nav/project_files.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/vcs/git_locator.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <QCoreApplication>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;

// <root>/frostengine-<sha12> under $FROST_CORPORA_ROOT (as corpus_test.cpp), checked to be the pinned
// clone; "" with a failure recorded otherwise.
std::string checkedCorpus() {
    const std::string root = qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString();
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus or "
                         "tools/verify.sh --corpus, which set it from tools/corpora.py root";
        return {};
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    std::string corpus = frost::io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
    auto& fs = frost::io::realFileSystem();
    const auto head = fs.readFile(corpus + "/.git/HEAD");
    if (!fs.exists(corpus + "/CMakePresets.json").value_or(false) || !head || head->substr(0, 40) != sha) {
        ADD_FAILURE() << "the pinned FrostEngine corpus is missing or not at its pin at " << corpus
                      << " (python tools/corpora.py fetch / verify); the corpus tier fails instead of skipping";
        return {};
    }
    return corpus;
}

std::string join(const std::string& base, std::string_view relative) {
    std::string native(relative);
    if constexpr (kWindows)
        std::ranges::replace(native, '/', '\\');
    return frost::io::joinPath(base, native).value_or("");
}

// The '/'-separated path of `path` relative to `root`, or "" outside it.
std::string relativeTo(const std::string& root, const std::string& path) {
    if (path.size() <= root.size() + 1 || !path.starts_with(root))
        return {};
    std::string relative = path.substr(root.size() + 1);
    std::ranges::replace(relative, '\\', '/');
    return relative;
}

// `git <arguments>` in `directory` with the git the product resolves; its NUL-separated output.
std::vector<std::string> gitList(const std::string& directory, std::vector<std::string> arguments) {
    auto git = frost::vcs::findGit(frost::io::realFileSystem(), frost::vcs::defaultGitSearch());
    if (!git) {
        ADD_FAILURE() << "git: " << git.error().describe();
        return {};
    }
    frost::io::ProcessSpec spec;
    spec.executable = git->path;
    spec.arguments = {"-C", directory};
    spec.arguments.insert(spec.arguments.end(), arguments.begin(), arguments.end());
    auto run = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 300s);
    if (!run || run->exitCode != 0) {
        ADD_FAILURE() << "git " << arguments.front()
                      << " failed: " << (run ? run->stderrBytes : run.error().describe());
        return {};
    }
    std::vector<std::string> entries;
    std::string_view rest = run->stdoutBytes;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\0');
        if (end != 0)
            entries.emplace_back(rest.substr(0, end));
        if (end == std::string_view::npos)
            break;
        rest.remove_prefix(end + 1);
    }
    return entries;
}

// The files git lists as tracked or untracked and not ignored: the oracle (paths '/'-separated).
std::set<std::string> gitProjectFiles(const std::string& corpus) {
    const auto listed = gitList(corpus, {"ls-files", "-z", "--cached", "--others", "--exclude-standard"});
    return {listed.begin(), listed.end()};
}

bool underGenerated(std::string_view relative) {
    return relative.starts_with("build/") || relative.starts_with(".cache/") || relative.starts_with(".git/") ||
           relative == "build" || relative == ".cache" || relative == ".git";
}

// The ignored files git reports outside build/ and .cache/ (an ignored folder counts its files).
std::size_t ignoredFilesOutsideBuildFolders(const std::string& corpus) {
    std::size_t count = 0;
    for (const std::string& entry :
         gitList(corpus, {"ls-files", "-z", "--others", "--ignored", "--exclude-standard", "--directory"})) {
        if (underGenerated(entry) || entry == "build/" || entry == ".cache/")
            continue;
        if (!entry.ends_with('/')) {
            ++count;
            continue;
        }
        std::error_code error;
        const auto folder = frost::io::toStdPath(join(corpus, entry.substr(0, entry.size() - 1)));
        for (auto it = std::filesystem::recursive_directory_iterator(*folder, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
            if (it->is_regular_file(error))
                ++count;
    }
    return count;
}

// The real file system, recording every folder listed and every file read.
class SpyFs final : public frost::io::IFileSystem {
public:
    frost::Result<std::string> readFile(std::string_view path) override {
        record(m_read, path);
        return inner.readFile(path);
    }
    frost::Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        return inner.writeFileAtomic(path, data);
    }
    frost::Result<frost::io::FileStatus> status(std::string_view path) override { return inner.status(path); }
    frost::Result<std::vector<frost::io::DirEntry>> listDirectory(std::string_view path) override {
        record(m_listed, path);
        return inner.listDirectory(path);
    }
    frost::Result<frost::io::DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        record(m_listed, path);
        return inner.listDirectoryWithDiagnostics(path);
    }
    frost::Result<void> createDirectories(std::string_view path) override { return inner.createDirectories(path); }
    frost::Result<void> removeFile(std::string_view path) override { return inner.removeFile(path); }
    frost::Result<void> removeDirectory(std::string_view path) override { return inner.removeDirectory(path); }
    frost::Result<bool> exists(std::string_view path) override { return inner.exists(path); }
    frost::Result<std::string> canonicalPath(std::string_view path) override { return inner.canonicalPath(path); }

    [[nodiscard]] std::vector<std::string> listed() const {
        const std::scoped_lock lock(m_mutex);
        return m_listed;
    }

private:
    void record(std::vector<std::string>& into, std::string_view path) {
        const std::scoped_lock lock(m_mutex);
        into.emplace_back(path);
    }
    frost::io::IFileSystem& inner = frost::io::realFileSystem();
    mutable std::mutex m_mutex;
    std::vector<std::string> m_listed;
    std::vector<std::string> m_read;
};

// The folders of `listed` below build/, .cache/ or .git/ of `corpus` (relative), for the message.
std::vector<std::string> generatedListings(const std::string& corpus, const std::vector<std::string>& listed) {
    std::vector<std::string> bad;
    for (const std::string& folder : listed)
        if (const std::string relative = relativeTo(corpus, folder); underGenerated(relative))
            bad.push_back(relative);
    return bad;
}

// The configure-created folders must exist, or "nothing under build/ is traversed" proves nothing.
bool hasConfiguredBuildTree(const std::string& corpus) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(*frost::io::toStdPath(join(corpus, "build")), error))
        if (std::filesystem::exists(entry.path() / "CMakeCache.txt", error))
            return true;
    return false;
}

TEST(CorpusFrostEngine, FindInFilesTheProjectTreeAndTheWatcherCoverExactlyWhatGitListsAndNeverEnterBuildFolders) {
    frost::test::disableCrashDialogs();
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());
    ASSERT_TRUE(hasConfiguredBuildTree(corpus))
        << "no configured build tree (build/<preset>/CMakeCache.txt) in " << corpus
        << "; the corpus tier's CorpusFrostEngine.OpeningThePinnedClone... test configures it";
    const std::set<std::string> oracle = gitProjectFiles(corpus);
    ASSERT_GT(oracle.size(), 1000U) << "git listed too few files in " << corpus;

    SpyFs spy;
    auto roles = frost::project::DirectoryRoles::load(spy, corpus);
    ASSERT_TRUE(roles.has_value()) << roles.error().describe();
    EXPECT_TRUE((*roles)->presetsProblem().empty()) << (*roles)->presetsProblem();
    EXPECT_FALSE((*roles)->generatedDirectories().empty()) << "the presets name build/<preset> binary folders";
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 4});

    // Find in Files over the project folder with the project's roles (the Directory scope the Find
    // popup uses for a CMake folder).
    {
        frost::document::LoadServiceOptions loadOptions;
        frost::document::LoadService loader(spy, loadOptions);
        frost::search::SearchScope scope;
        scope.kind = frost::search::ScopeKind::Directory;
        scope.directory = corpus;
        scope.ignoreRoot = corpus;
        scope.roles = *roles;
        auto target = frost::search::resolveScope(scope, nullptr, {});
        ASSERT_TRUE(target.has_value()) << target.error().describe();
        frost::search::FindRequest request;
        request.query.pattern = "frost-ide-corpus-needle-that-no-file-contains";
        request.target = std::move(*target);
        auto summary = frost::sched::syncWait(frost::search::findInFiles(pool, loader, std::move(request), {}));
        ASSERT_TRUE(summary.has_value()) << summary.error().describe();
        std::cout << std::format("[corpus] Find in Files: {} files in scope, {} folders listed; git lists {}\n",
                                 summary->filesInScope, summary->directoriesListed, oracle.size());
        EXPECT_EQ(summary->filesInScope, oracle.size());
        EXPECT_EQ(summary->matches, 0U);
        const auto bad = generatedListings(corpus, spy.listed());
        EXPECT_TRUE(bad.empty()) << "Find in Files listed " << bad.size() << " folders such as " << bad.front();
    }

    // The project tree (the File System view and Go to File's index).
    {
        SpyFs treeSpy;
        auto treeRoles = frost::project::DirectoryRoles::load(treeSpy, corpus);
        ASSERT_TRUE(treeRoles.has_value());
        auto scan = frost::sched::syncWait(frost::ui::nav::scanProject(treeSpy, pool, corpus, {}, *treeRoles));
        ASSERT_TRUE(scan.has_value()) << scan.error().describe();
        frost::ui::nav::ProjectFiles files(corpus);
        files.setScan(*scan);
        std::set<std::string> scanned;
        for (const auto& entry : scan->entries)
            if (entry.type == frost::io::FileType::Regular && !entry.ignored)
                scanned.insert(entry.relative);
        std::vector<std::string> missing;
        std::ranges::set_difference(oracle, scanned, std::back_inserter(missing));
        std::vector<std::string> extra;
        std::ranges::set_difference(scanned, oracle, std::back_inserter(extra));
        std::cout << std::format("[corpus] project tree: {} files, {} folders listed\n", files.fileCount(),
                                 scan->directoriesListed);
        EXPECT_EQ(files.fileCount(), oracle.size());
        EXPECT_TRUE(missing.empty()) << missing.size() << " git-listed files are not in the tree, such as "
                                     << missing.front();
        EXPECT_TRUE(extra.empty()) << extra.size() << " tree files git does not list, such as " << extra.front();
        const auto bad = generatedListings(corpus, treeSpy.listed());
        EXPECT_TRUE(bad.empty()) << "the project scan listed " << bad.size() << " folders such as " << bad.front();
        // The build and cache folders themselves are in the tree (to mark them again), empty.
        for (const char* folder : {"build", ".cache"})
            if (const auto node = files.find(join(corpus, folder)))
                EXPECT_TRUE(files.node(*node).children.empty()) << folder;
    }

    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value()) << reactor.error().describe();
    auto watcher = frost::io::RealFileWatcher::create(**reactor, pool);
    ASSERT_TRUE(watcher.has_value()) << watcher.error().describe();
    const auto watchOptions = [&](bool polling) {
        frost::io::WatchOptions options;
        options.forcePolling = polling;
        options.pollInterval = 5s;
        options.excludedBelow = [roles = *roles](std::string_view path) { return roles->belowUntraversed(path); };
        return options;
    };

    // The watcher's watched files: the polling snapshot (the real watcher's own walk).
    {
        auto watch = frost::sched::syncWait((*watcher)->watch(corpus, watchOptions(true)));
        ASSERT_TRUE(watch.has_value()) << watch.error().describe();
        const std::size_t ignoredOutside = ignoredFilesOutsideBuildFolders(corpus);
        const auto status = (*watch)->status();
        std::cout << std::format("[corpus] watcher (polling): {} files watched; git: {} listed + {} ignored outside "
                                 "build/ and .cache/\n",
                                 status.watchedFiles, oracle.size(), ignoredOutside);
        EXPECT_EQ(status.watchedFiles, oracle.size() + ignoredOutside);
        watch->reset();
    }

    // The native watcher: a change below build/ or .cache/ is never reported, a tracked file's is.
    {
        auto watch = frost::sched::syncWait((*watcher)->watch(corpus, watchOptions(false)));
        ASSERT_TRUE(watch.has_value()) << watch.error().describe();
        auto& fs = frost::io::realFileSystem();
        const std::string probeName = std::format("frost-ide-watch-probe-{}.txt", QCoreApplication::applicationPid());
        const std::string buildProbe = join(corpus, "build/" + probeName);
        const std::string cacheProbe = join(corpus, ".cache/" + probeName);
        ASSERT_TRUE(fs.writeFileAtomic(buildProbe, "probe").has_value());
        ASSERT_TRUE(fs.writeFileAtomic(cacheProbe, "probe").has_value());
        // A tracked file's modification time moves (its content does not): the change to wait for.
        const std::string tracked = join(corpus, "README.md");
        const auto stdTracked = *frost::io::toStdPath(tracked);
        const auto before = std::filesystem::last_write_time(stdTracked);
        std::filesystem::last_write_time(stdTracked, before + 2s);
        bool seen = false;
        std::vector<std::string> reported;
        const auto deadline = std::chrono::steady_clock::now() + 60s;
        while (!seen && std::chrono::steady_clock::now() < deadline) {
            std::stop_source stop;
            auto batch = frost::sched::syncWait((*watch)->next(stop.get_token()));
            ASSERT_TRUE(batch.has_value()) << batch.error().describe();
            for (const auto& change : batch->changes) {
                reported.push_back(change.path);
                seen = seen || change.path == tracked;
            }
        }
        std::filesystem::last_write_time(stdTracked, before);
        static_cast<void>(fs.removeFile(buildProbe));
        static_cast<void>(fs.removeFile(cacheProbe));
        EXPECT_TRUE(seen) << "the native watcher did not report " << tracked;
        for (const std::string& path : reported)
            EXPECT_FALSE(underGenerated(relativeTo(corpus, path)) &&
                         relativeTo(corpus, path).find('/') != std::string::npos)
                << "reported below a generated folder: " << path;
        watch->reset();
    }
    (*reactor)->shutdown();
    watcher->reset();
    pool.shutdown();
}

// --- FEC-10a ---

// The pinned LuaLS, or "" after recording a failure.
std::string pinnedLuaLs() {
    auto path = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    if (!path) {
        ADD_FAILURE() << "the pinned LuaLS " << frost::lang::pinnedLuaLanguageServerVersion()
                      << " is required (tools/bootstrap.ps1 -Only luals): " << path.error().describe();
        return {};
    }
    return *path;
}

// The global names LuaLS reports as undefined-global in `file`'s last diagnostics.
std::set<std::string> undefinedGlobals(frost::lang::LuaService& service, const std::string& file) {
    std::set<std::string> names;
    if (auto set = service.diagnostics().forFile(file))
        for (const auto& diagnostic : set->diagnostics) {
            if (diagnostic.code != "undefined-global")
                continue;
            const std::size_t open = diagnostic.message.find('`');
            const std::size_t close = open == std::string::npos ? open : diagnostic.message.find('`', open + 1);
            names.insert(close == std::string::npos ? diagnostic.message
                                                    : diagnostic.message.substr(open + 1, close - open - 1));
        }
    return names;
}

constexpr std::string_view kMisspelled = "Nte"; // The engine's Net namespace, misspelled.
constexpr char kProbeLine[] = "\nlocal frostIdeProbe = Nte.IsServer()\n";

/*
 * Starts real LuaLS on `workspace` with `configuration`, opens `file` with `text`, and waits until
 * LuaLS flags the misspelled global (the sign it diagnosed this text). Returns the names it reports
 * as undefined globals in that file; a failure is recorded when it never flags the probe.
 */
std::set<std::string> diagnoseWithProbe(const std::string& server, const std::string& workspace,
                                        frost::lang::Json configuration, const std::string& file,
                                        const std::string& text, frost::test::ScratchDir& state) {
    auto reactor = frost::io::Reactor::create();
    EXPECT_TRUE(reactor.has_value());
    if (!reactor)
        return {};
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    frost::lang::LuaServiceOptions options;
    options.serverPath = server;
    options.workspaceRoot = workspace;
    options.trusted = true;
    options.configuration = std::move(configuration);
    static int run = 0;
    auto arguments = frost::lang::luaLanguageServerArguments(state.join(std::format("state-{}", ++run)));
    EXPECT_TRUE(arguments.has_value());
    if (arguments)
        options.arguments = std::move(*arguments);
    std::set<std::string> names;
    {
        frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool}, std::move(options));
        auto document = frost::text::Document::fromUtf8(text);
        EXPECT_TRUE(document.has_value());
        EXPECT_TRUE(service.openDocument(file, document->text()).has_value());
        EXPECT_TRUE(service.start().has_value());
        const auto deadline = std::chrono::steady_clock::now() + 180s;
        bool flagged = false;
        while (!(flagged = undefinedGlobals(service, file).contains(std::string(kMisspelled))) &&
               service.state() != frost::lang::LuaState::Failed && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(50ms);
        EXPECT_TRUE(flagged) << "LuaLS never flagged the misspelled global " << kMisspelled << " in " << file;
        if (auto error = service.lastError())
            ADD_FAILURE() << error->describe();
        names = undefinedGlobals(service, file);
        pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    }
    (*reactor)->shutdown();
    pool.shutdown();
    return names;
}

TEST(CorpusFrostEngine, LuaLsReportsNoUndefinedGlobalForEngineGlobalsAndStillFlagsAMisspelledOne) {
    frost::test::disableCrashDialogs();
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    auto& fs = frost::io::realFileSystem();

    // The engine checkout itself and its sample both learn the engine's globals from its reference.
    const auto engine = frost::lang::discoverFrostEngineLua(fs, corpus);
    EXPECT_TRUE(engine.frostEngine);
    EXPECT_TRUE(engine.problem.empty()) << engine.problem;
    EXPECT_EQ(engine.api.runtimeVersion, "Lua 5.4");
    const std::string sample = join(corpus, "Samples/MiniMMO");
    const auto profile = frost::lang::discoverFrostEngineLua(fs, sample);
    ASSERT_TRUE(profile.frostEngine);
    ASSERT_TRUE(profile.projectRoot);
    ASSERT_TRUE(profile.problem.empty()) << profile.problem;
    EXPECT_EQ(profile.reference, join(corpus, "Docs/ScriptingAPI.md"));
    ASSERT_GT(profile.api.globals.size(), 50U);
    EXPECT_FALSE(std::ranges::contains(profile.api.globals, std::string(kMisspelled)));
    std::cout << std::format("[corpus] {} engine globals from {}\n", profile.api.globals.size(), profile.reference);

    frost::test::ScratchDir state("corpus-luals");
    for (const char* script :
         {"Assets/Scripts/Hud.lua", "Assets/Scripts/LoginScreen.lua", "Assets/Scripts/PlayerAvatar.lua"}) {
        SCOPED_TRACE(script);
        const std::string file = join(sample, script);
        auto text = fs.readFile(file);
        ASSERT_TRUE(text.has_value()) << text.error().describe();
        // Without the engine's globals LuaLS flags engine names (the check below is not vacuous):
        // every name it flags, apart from the probe, is one the engine's reference declares.
        const auto baseline = diagnoseWithProbe(
            server, sample,
            frost::lang::frostEngineLuaDefaults("Lua 5.4",
                                                [] {
                                                    frost::lang::FrostEngineLuaProfile unreferenced;
                                                    unreferenced.frostEngine =
                                                        true; // A FrostEngine workspace whose reference was not found.
                                                    return unreferenced;
                                                }()),
            file, *text + kProbeLine, state);
        std::set<std::string> engineNames = baseline;
        engineNames.erase(std::string(kMisspelled));
        EXPECT_FALSE(engineNames.empty()) << "LuaLS flagged no engine global without the defaults";
        for (const std::string& name : engineNames)
            EXPECT_TRUE(std::ranges::contains(profile.api.globals, name)) << name << " is no engine global";
        // With the engine's defaults: only the misspelled probe.
        const auto withDefaults = diagnoseWithProbe(
            server, sample, frost::lang::frostEngineLuaDefaults("Lua 5.4", profile), file, *text + kProbeLine, state);
        EXPECT_EQ(withDefaults, std::set<std::string>{std::string(kMisspelled)});
    }

    // A temp copy on disk: the engine's reference and the sample's project files, one script with a
    // misspelled global; the copy finds the reference by the same lookup.
    frost::test::ScratchDir copy("corpus-lua-copy");
    const std::string copied = copy.join(
        frost::io::kNativePathStyle == frost::io::PathStyle::Windows ? "Samples\\MiniMMO" : "Samples/MiniMMO");
    for (const char* relative : {".feproject", ".luarc.json", "MiniMMO.frostproj"}) {
        auto bytes = fs.readFile(join(sample, relative));
        ASSERT_TRUE(bytes.has_value()) << relative;
        ASSERT_TRUE(fs.createDirectories(copied).has_value());
        ASSERT_TRUE(fs.writeFileAtomic(join(copied, relative), *bytes).has_value());
    }
    auto reference = fs.readFile(join(corpus, "Docs/ScriptingAPI.md"));
    ASSERT_TRUE(reference.has_value());
    ASSERT_TRUE(fs.createDirectories(copy.join("Docs")).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(join(copy.path(), "Docs/ScriptingAPI.md"), *reference).has_value());
    auto hud = fs.readFile(join(sample, "Assets/Scripts/Hud.lua"));
    ASSERT_TRUE(hud.has_value());
    const std::string copiedScript = join(copied, "Assets/Scripts/Hud.lua");
    ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(copiedScript)).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(copiedScript, *hud + kProbeLine).has_value());
    const auto copyProfile = frost::lang::discoverFrostEngineLua(fs, copied);
    ASSERT_TRUE(copyProfile.problem.empty()) << copyProfile.problem;
    EXPECT_EQ(copyProfile.api.globals, profile.api.globals);
    auto onDisk = fs.readFile(copiedScript);
    ASSERT_TRUE(onDisk.has_value());
    const auto copyNames = diagnoseWithProbe(
        server, copied, frost::lang::frostEngineLuaDefaults("Lua 5.4", copyProfile), copiedScript, *onDisk, state);
    EXPECT_EQ(copyNames, std::set<std::string>{std::string(kMisspelled)});
}

// --- LUA-3 and LUA-13 ---

/*
 * LUA-3's exit criterion on the clone's scripts: in a temp copy of the MiniMMO sample (with the
 * engine's reference beside it), configured through the LUA-13 host registry with FrostEngine's
 * provider, a module created after LuaLS started resolves in a script's require without a restart,
 * and a runtime change is re-sent as didChangeConfiguration to the same process. The clone itself
 * stays untouched (the module is created in the copy only).
 */
TEST(CorpusFrostEngine, ANewModuleInTheSampleResolvesWithoutARestartThroughTheHostApi) {
    frost::test::disableCrashDialogs();
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    auto& fs = frost::io::realFileSystem();
    const std::string sample = join(corpus, "Samples/MiniMMO");
    frost::test::ScratchDir copy("corpus-lua-new-module");
    const std::string copied = join(copy.path(), "Samples/MiniMMO");
    std::error_code error;
    std::filesystem::create_directories(*frost::io::toStdPath(copied), error);
    ASSERT_FALSE(error) << error.message();
    std::filesystem::copy(*frost::io::toStdPath(sample), *frost::io::toStdPath(copied),
                          std::filesystem::copy_options::recursive, error);
    ASSERT_FALSE(error) << error.message();
    auto reference = fs.readFile(join(corpus, "Docs/ScriptingAPI.md"));
    ASSERT_TRUE(reference.has_value());
    ASSERT_TRUE(fs.createDirectories(copy.join("Docs")).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(join(copy.path(), "Docs/ScriptingAPI.md"), *reference).has_value());

    frost::lang::LuaHostRegistry hosts;
    hosts.add(std::make_shared<frost::lang::FrostEngineLuaProvider>());
    const auto contributions = hosts.contributions(fs, copied);
    ASSERT_EQ(contributions.size(), 1U);
    EXPECT_EQ(contributions[0].provider, "FrostEngine");
    EXPECT_GT(contributions[0].globals.size(), 50U);

    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    frost::lang::LuaServiceOptions options;
    options.serverPath = server;
    options.workspaceRoot = copied;
    options.trusted = true;
    options.configuration = frost::lang::composeLuaConfiguration("Lua 5.4", contributions);
    auto arguments = frost::lang::luaLanguageServerArguments(copy.join("state"));
    ASSERT_TRUE(arguments.has_value());
    options.arguments = std::move(*arguments);
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool}, std::move(options));
    const std::string hud = join(copied, "Assets/Scripts/Hud.lua");
    auto text = fs.readFile(hud);
    ASSERT_TRUE(text.has_value());
    // The probe lines exist in the editor buffer only; the misspelled global makes LuaLS publish.
    const std::string probe = "local frostIdeProbeModule = require(\"FrostIdeProbeModule\")\n";
    // kProbeLine adds two line breaks, so the require line is the text's line count + 2 (0-based).
    const auto line = static_cast<std::int64_t>(std::ranges::count(*text, '\n')) + 2;
    auto document = frost::text::Document::fromUtf8(*text + kProbeLine + probe);
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(service.openDocument(hud, document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    const auto until = [](auto ready, std::chrono::seconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (;;) {
            if (ready())
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(50ms);
        }
    };
    ASSERT_TRUE(until([&] { return service.diagnostics().forFile(hud).has_value(); }, 180s))
        << "LuaLS never diagnosed Hud.lua";
    const auto pid = service.serverPid();
    const auto definition = [&] {
        const frost::lang::Json params =
            frost::lang::Json::object()
                .set("textDocument", frost::lang::Json::object().set("uri", frost::lang::pathToUri(hud).value()))
                .set("position", frost::lang::Json::object().set("line", line).set("character", 40));
        auto result =
            pool.spawn(frost::sched::Lane::UserVisible, service.request("textDocument/definition", params)).join();
        return result ? result->dump() : "error: " + result.error().describe();
    };
    EXPECT_EQ(definition().find("FrostIdeProbeModule.lua"), std::string::npos) << "no such module yet";

    const std::string module = join(copied, "Assets/Scripts/FrostIdeProbeModule.lua");
    ASSERT_TRUE(fs.writeFileAtomic(module, "local M = {}\nfunction M.Ping() return 1 end\nreturn M\n").has_value());
    const std::array changes{frost::lang::LuaFileChange{module, frost::lang::FileChangeType::Created}};
    ASSERT_TRUE(service.notifyFileChanges(changes).has_value());
    std::string last;
    EXPECT_TRUE(until(
        [&] {
            last = definition();
            return last.find("FrostIdeProbeModule.lua") != std::string::npos;
        },
        120s))
        << last;

    ASSERT_TRUE(
        service.changeConfiguration(frost::lang::composeLuaConfiguration("Lua 5.1", contributions), {}).has_value());
    EXPECT_EQ(service.configurationChangesSent(), 1U);
    // The engine's reference names its runtime (Lua 5.4), which outranks the user's (FEC-10a).
    EXPECT_EQ(*frost::lang::luaConfigurationSection(service.configuration(), "Lua.runtime.version").asString(),
              "Lua 5.4");
    EXPECT_EQ(service.restarts(), 0U);
    EXPECT_EQ(service.serverPid(), pid);
    EXPECT_EQ(service.state(), frost::lang::LuaState::Running);
    if (auto failure = service.lastError())
        ADD_FAILURE() << failure->describe();
    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    (*reactor)->shutdown();
    pool.shutdown();
}

// --- FEC-9a ---

class RecordingRealLauncher final : public frost::io::IProcessLauncher {
public:
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        {
            const std::scoped_lock lock(m_mutex);
            m_specs.push_back(spec);
        }
        return m_inner.launch(spec);
    }

    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        {
            const std::scoped_lock lock(m_mutex);
            m_specs.push_back(spec);
        }
        return m_inner.launchAsync(spec, context);
    }

    [[nodiscard]] std::vector<frost::io::ProcessSpec> specs() const {
        const std::scoped_lock lock(m_mutex);
        return m_specs;
    }

private:
    frost::io::IProcessLauncher& m_inner = frost::io::realProcessLauncher();
    mutable std::mutex m_mutex;
    std::vector<frost::io::ProcessSpec> m_specs;
};

class SilentCorpusPrompts final : public frost::app::IWorkbenchPrompts {
public:
    std::string chooseOpenPath(QWidget*, OpenKind) override { return {}; }
    bool confirmDiscard(QWidget*, const QString&, const QString&) override { return true; }
    bool confirmRerun(QWidget*, const QString&) override { return true; }
    bool confirmProjectRecovery(QWidget*, const QString&) override { return false; }
};

void copyTreeRecursive(frost::io::IFileSystem& fs, const std::string& src, const std::string& dst) {
    auto st = fs.status(src);
    ASSERT_TRUE(st.has_value()) << src << ": " << st.error().describe();
    if (st->type == frost::io::FileType::Regular) {
        auto bytes = fs.readFile(src);
        ASSERT_TRUE(bytes.has_value()) << src << ": " << bytes.error().describe();
        ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(dst)).has_value());
        ASSERT_TRUE(fs.writeFileAtomic(dst, *bytes).has_value());
        return;
    }
    if (st->type == frost::io::FileType::Directory) {
        ASSERT_TRUE(fs.createDirectories(dst).has_value());
        auto entries = fs.listDirectory(src);
        ASSERT_TRUE(entries.has_value()) << src << ": " << entries.error().describe();
        for (const frost::io::DirEntry& entry : *entries) {
            copyTreeRecursive(fs, join(src, entry.name), join(dst, entry.name));
        }
    }
}

TEST(CorpusFrostEngine, FmetaSafeRenameAndMovePreserveSidecarsAndWarnWithEveryOracleReference) {
    frost::test::disableCrashDialogs();
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());
    auto& fs = frost::io::realFileSystem();

    const std::string sampleInCorpus = join(corpus, "Samples/MiniMMO");
    auto editorExe = frost::lang::findFrostEditorExecutable(fs, sampleInCorpus);
    ASSERT_TRUE(editorExe.has_value()) << "FrostEditor CLI binary not found in pinned corpus: "
                                       << editorExe.error().describe();

    // Copy Samples/MiniMMO to a scratch directory so file rename/move operations never dirty the
    // pinned corpus checkout.
    frost::test::ScratchDir scratch("corpus-fmeta-refs");
    const std::string sampleCopy = join(scratch.path(), "MiniMMO");
    copyTreeRecursive(fs, sampleInCorpus, sampleCopy);

    // Run the real FrostEditor CLI directly as the oracle for `Scripts/Hud.lua`.
    frost::io::ProcessSpec oracleSpec;
    oracleSpec.executable = *editorExe;
    oracleSpec.arguments = {"assets.find-references", "--project", sampleCopy, "--asset", "Scripts/Hud.lua"};
    oracleSpec.workingDirectory = sampleCopy;
    auto oracleRun = frost::io::runToCompletion(frost::io::realProcessLauncher(), oracleSpec, {}, 60s);
    ASSERT_TRUE(oracleRun.has_value()) << oracleRun.error().describe();
    ASSERT_EQ(oracleRun->exitCode, 0) << oracleRun->stderrBytes;
    auto oracleReport = frost::lang::parseAssetReferenceReport(oracleRun->stdoutBytes);
    ASSERT_TRUE(oracleReport.has_value()) << oracleReport.error().describe();
    ASSERT_FALSE(oracleReport->references.empty()) << "Expected MiniMMO Hud.lua to have referencing assets";

    const std::string originalFmetaBytes =
        fs.readFile(join(sampleCopy, "Assets/Scripts/Hud.lua.fmeta")).value_or(std::string{});
    ASSERT_FALSE(originalFmetaBytes.empty());

    // Open the scratch MiniMMO project in Workbench, trust it, and rename `Assets/Scripts/Hud.lua`.
    RecordingRealLauncher launcher;
    SilentCorpusPrompts prompts;
    auto settings = frost::settings::SettingsStore::open(frost::settings::builtinRegistry(), fs,
                                                         {join(scratch.path(), "settings.json"), std::nullopt});
    ASSERT_TRUE(settings.has_value()) << settings.error().describe();
    auto trust = frost::project::TrustStore::open(fs, join(scratch.path(), "trusted-roots.txt"));
    ASSERT_TRUE(trust.has_value()) << trust.error().describe();
    ASSERT_TRUE((*trust)->trust(sampleCopy).has_value());

    frost::app::WorkbenchOptions options;
    options.frostEditorPath = *editorExe;
    options.clangd = false;
    options.projectHost = false;
    options.git = false;
    options.watchFiles = false;
    options.followActiveLayout = false;
    options.luaSearch = frost::lang::LuaSearch{};
    options.workers = 2;

    frost::app::Workbench workbench(
        frost::app::WorkbenchServices{fs, launcher, frost::systemClock(), **settings, **trust, prompts},
        std::move(options));
    frost::ui::ShellWindow window(workbench.environment());
    workbench.attach(window);
    window.show();
    ASSERT_TRUE(workbench.openPath(sampleCopy).has_value());

    const auto noticesBeforeRename = window.notifications().log().size();
    auto renamed = workbench.renameItem(join(sampleCopy, "Assets/Scripts/Hud.lua"), "HudRenamed.lua");
    ASSERT_TRUE(renamed.has_value()) << renamed.error().describe();

    // Verify that the .fmeta sidecar moved alongside the script with its handle intact.
    EXPECT_TRUE(fs.exists(join(sampleCopy, "Assets/Scripts/HudRenamed.lua")).value_or(false));
    EXPECT_TRUE(fs.exists(join(sampleCopy, "Assets/Scripts/HudRenamed.lua.fmeta")).value_or(false));
    EXPECT_FALSE(fs.exists(join(sampleCopy, "Assets/Scripts/Hud.lua")).value_or(true));
    EXPECT_FALSE(fs.exists(join(sampleCopy, "Assets/Scripts/Hud.lua.fmeta")).value_or(true));
    EXPECT_EQ(fs.readFile(join(sampleCopy, "Assets/Scripts/HudRenamed.lua.fmeta")).value_or(""), originalFmetaBytes);

    // Verify that `FrostEditor assets.find-references` ran via IProcessLauncher and the warning
    // lists every referencing asset reported by the oracle.
    const auto recordedSpecs = launcher.specs();
    const bool ranFindRefs = std::ranges::any_of(recordedSpecs, [&](const frost::io::ProcessSpec& s) {
        return s.executable == *editorExe && !s.arguments.empty() && s.arguments[0] == "assets.find-references";
    });
    EXPECT_TRUE(ranFindRefs);
    ASSERT_TRUE(workbench.lastAssetReferenceReport().has_value());
    EXPECT_EQ(workbench.lastAssetReferenceReport()->references, oracleReport->references);

    ASSERT_GT(window.notifications().log().size(), noticesBeforeRename);
    const auto& warning = window.notifications().log().back().notification;
    EXPECT_EQ(warning.kind, frost::ui::NotificationKind::Warning);
    for (const frost::lang::AssetReferenceEntry& ref : oracleReport->references) {
        EXPECT_TRUE(warning.text.contains(QString::fromStdString(ref.path)))
            << "Warning is missing referencing asset " << ref.path << ":\n"
            << warning.text.toStdString();
    }

    // Moving the renamed script into a subdirectory also moves its .fmeta sibling alongside it.
    const std::string subDir = join(sampleCopy, "Assets/Scripts/UI");
    ASSERT_TRUE(fs.createDirectories(subDir).has_value());
    auto* fsView = workbench.navigation()->fileSystemView();
    ASSERT_NE(fsView, nullptr);
    emit fsView->movePathsToDirectoryRequested(
        {QString::fromStdString(join(sampleCopy, "Assets/Scripts/HudRenamed.lua"))}, QString::fromStdString(subDir));

    EXPECT_TRUE(fs.exists(join(sampleCopy, "Assets/Scripts/UI/HudRenamed.lua")).value_or(false));
    EXPECT_TRUE(fs.exists(join(sampleCopy, "Assets/Scripts/UI/HudRenamed.lua.fmeta")).value_or(false));
    EXPECT_FALSE(fs.exists(join(sampleCopy, "Assets/Scripts/HudRenamed.lua")).value_or(true));
    EXPECT_FALSE(fs.exists(join(sampleCopy, "Assets/Scripts/HudRenamed.lua.fmeta")).value_or(true));
    EXPECT_EQ(fs.readFile(join(sampleCopy, "Assets/Scripts/UI/HudRenamed.lua.fmeta")).value_or(""), originalFmetaBytes);

    workbench.shutdown();
}

} // namespace
