// SPDX-License-Identifier: Apache-2.0
// Later mandatory public-corpus oracle: actual SDK evaluation and owned RoslynHost over MoreLINQ.
#include "frost/build/managed_evaluation.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/vcxproj.hpp"
#include "frost/sched/thread_pool.hpp"

#include "support/managed_profile_guard.hpp"
#include "support/managed_service_shutdown.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <thread>
namespace {
using namespace frost;
using namespace std::chrono_literals;
sched::Task<Result<lang::Json>> ask(lang::ManagedService& service, std::string method, lang::Json params) {
    co_return co_await service.request(std::move(method), std::move(params));
}
std::error_code copyManagedCorpus(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(from, error), end; !error && it != end; it.increment(error)) {
        const auto relative = it->path().lexically_relative(from);
        const auto first = *relative.begin();
        // Older fetches populated this untracked cache even for the MoreLINQ corpus.
        if (first == ".git" || first == ".github" || relative == std::filesystem::path(".cache/cpm")) {
            it.disable_recursion_pending();
            continue;
        }
        const auto status = it->symlink_status(error);
        if (error)
            return error;
        if (std::filesystem::is_symlink(status))
            return std::make_error_code(std::errc::operation_not_supported);
        const auto destination = to / relative;
        if (std::filesystem::is_directory(status))
            std::filesystem::create_directories(destination, error);
        else {
            std::filesystem::create_directories(destination.parent_path(), error);
            if (!error)
                std::filesystem::copy_file(it->path(), destination, std::filesystem::copy_options::overwrite_existing,
                                           error);
        }
        if (error)
            return error;
    }
    return error;
}

TEST(ManagedCorpus, ScratchCopyExcludesLegacyEngineCache) {
    test::ScratchDir scratch("managed-corpus-copy");
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(scratch.join("source/.cache/cpm/fmt/hash")));
    ASSERT_TRUE(fs.createDirectories(scratch.join("source/MoreLinq")));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("source/.cache/cpm/fmt/hash/cache.txt"), "package"));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("source/.cache/keep.txt"), "keep"));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("source/MoreLinq/Source.cs"), "class Source {}"));

    const auto source = io::toStdPath(scratch.join("source")).value();
    const auto destination = io::toStdPath(scratch.join("copy")).value();
    const auto error = copyManagedCorpus(source, destination);
    ASSERT_FALSE(error) << error.message();
    EXPECT_FALSE(std::filesystem::exists(destination / ".cache/cpm"));
    EXPECT_EQ(fs.readFile(scratch.join("copy/.cache/keep.txt")).value(), "keep");
    EXPECT_EQ(fs.readFile(scratch.join("copy/MoreLinq/Source.cs")).value(), "class Source {}");
    EXPECT_EQ(fs.readFile(scratch.join("source/.cache/cpm/fmt/hash/cache.txt")).value(), "package");
}

TEST(ManagedCorpus, ScratchCopyReportsDestinationCollision) {
    test::ScratchDir scratch("managed-corpus-collision");
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(scratch.join("source")));
    ASSERT_TRUE(fs.createDirectories(scratch.join("copy/Source.cs")));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("source/Source.cs"), "class Source {}"));
    const auto source = io::toStdPath(scratch.join("source")).value();
    const auto destination = io::toStdPath(scratch.join("copy")).value();

    EXPECT_TRUE(copyManagedCorpus(source, destination));
    EXPECT_TRUE(std::filesystem::is_directory(destination / "Source.cs"));
    EXPECT_EQ(fs.readFile(scratch.join("source/Source.cs")).value(), "class Source {}");
}

TEST(ManagedCorpus, PinnedMoreLinqNet10ScratchEvaluatesBuildsAndExposesTheSameRoslynSourceItems) {
    ASSERT_TRUE(test::sdkTestMayStart()) << "SDK profile is not prepared";
    const auto env = [](const char* name) { return project::processEnvironmentVariable(name).value_or(""); };
    const auto root = env("FROST_CORPORA_ROOT"), dotnet = env("FROST_TEST_CSHARP_DOTNET"),
               server = env("FROST_TEST_CSHARP_SERVER"), packages = env("FROST_TEST_CSHARP_PACKAGES");
    ASSERT_TRUE(io::isAbsolutePath(root));
    ASSERT_TRUE(io::isAbsolutePath(dotnet));
    ASSERT_TRUE(io::isAbsolutePath(server));
    ASSERT_TRUE(io::isAbsolutePath(packages))
        << "Provide the pre-provisioned local package cache; the test downloads nothing";
    const auto corpus = io::joinPath(root, "managed-" + std::string(FROST_MANAGED_CORPUS_SHA).substr(0, 12)).value();
    auto& fs = io::realFileSystem();
    auto head = fs.readFile(io::joinPath(corpus, ".git/HEAD").value());
    ASSERT_TRUE(head);
    ASSERT_TRUE(head->starts_with(FROST_MANAGED_CORPUS_SHA)) << "Run the explicit corpus provisioning step first";
    auto license = fs.readFile(io::joinPath(corpus, "COPYING.txt").value());
    ASSERT_TRUE(license);
    ASSERT_NE(license->find("Apache License"), std::string::npos);
    test::ScratchDir scratch("managed-public-corpus");
    const auto copy = scratch.join("MoreLINQ");
    ASSERT_TRUE(fs.createDirectories(copy));
    const auto copyError = copyManagedCorpus(io::toStdPath(corpus).value(), io::toStdPath(copy).value());
    ASSERT_FALSE(copyError) << copyError.message();
    const auto projectFile = io::joinPath(copy, "MoreLinq/MoreLinq.csproj").value();
    const auto projectDirectory = std::string(io::parentPath(projectFile));
    ASSERT_TRUE(
        fs.writeFileAtomic(io::joinPath(copy, "global.json").value(),
                           R"({"sdk":{"version":"10.0.403","rollForward":"disable","allowPrerelease":false}})"));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("NuGet.Config"),
                                   "<configuration><packageSources><clear /></packageSources></configuration>"));
    auto options = lang::managedServiceOptions(dotnet, server, copy, true);
    options.arguments.insert(options.arguments.end(),
                             {"--solution", projectFile, "--configuration", "Release", "--framework", "netstandard2.1",
                              "--project-context", projectFile});
    for (const auto* name : {"DOTNET_CLI_HOME", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA",
                             "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH"})
        options.environment.emplace_back(name, scratch.join(std::string("profile/") + name));
    options.environment.emplace_back("NUGET_PACKAGES", packages);
    io::ProcessSpec restore;
    restore.executable = dotnet;
    restore.workingDirectory = projectDirectory;
    restore.environment = options.environment;
    restore.arguments = {"restore",
                         projectFile,
                         "--configfile",
                         scratch.join("NuGet.Config"),
                         "--packages",
                         packages,
                         "-property:TargetFramework=netstandard2.1"};
    auto restored = io::runToCompletion(io::realProcessLauncher(), restore, {}, 2min);
    ASSERT_TRUE(restored);
    ASSERT_EQ(restored->exitCode, 0) << restored->stdoutBytes << restored->stderrBytes;
    build::ManagedProjectRequest request{
        .dotnet = dotnet, .project = projectFile, .configuration = "Release", .targetFramework = "netstandard2.1"};
    auto query = build::managedProjectEvaluationSpec(request);
    ASSERT_TRUE(query);
    query->environment = options.environment;
    auto output = io::runToCompletion(io::realProcessLauncher(), *query, {}, 2min);
    ASSERT_TRUE(output);
    ASSERT_EQ(output->exitCode, 0) << output->stdoutBytes << output->stderrBytes;
    auto evaluation = build::parseManagedProjectEvaluation(request, output->stdoutBytes);
    ASSERT_TRUE(evaluation) << evaluation.error().describe();
    auto reactor = io::Reactor::create();
    ASSERT_TRUE(reactor);
    sched::ThreadPool pool(sched::ThreadPoolOptions{.workerCount = 2});
    lang::ManagedService service(io::realProcessLauncher(), {**reactor, pool}, options);
    test::ManagedServiceShutdown shutdown{service, **reactor, pool};
    ASSERT_TRUE(service.start());
    const auto deadline = std::chrono::steady_clock::now() + 45s;
    while (service.state() != lang::LuaState::Running && service.state() != lang::LuaState::Failed &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(50ms);
    const auto startupError = service.lastError();
    std::string startupOutput;
    for (const auto& line : service.recentStderr())
        startupOutput += line + '\n';
    ASSERT_EQ(service.state(), lang::LuaState::Running) << (startupError ? startupError->describe() : "") << '\n'
                                                        << startupOutput;
    auto model = pool.spawn(sched::Lane::UserVisible, ask(service, "frost/projectModel", lang::Json::object())).join();
    ASSERT_TRUE(model) << model.error().describe();
    ASSERT_TRUE(model->isArray());
    std::set<std::string> actual;
    for (const auto& project : model->items())
        if (const auto* compile = project.find("compile"))
            for (const auto& source : compile->items())
                actual.insert(lang::documentKey(source.stringAt("path")));
    ASSERT_FALSE(actual.empty());
    for (const auto& item : evaluation->items.at("Compile")) {
        const auto full = item.metadata.find("FullPath");
        ASSERT_NE(full, item.metadata.end());
        EXPECT_TRUE(actual.contains(lang::documentKey(full->second))) << item.identity;
    }
    shutdown.stop();
    // Existing real REF1 journey supplies cross-project rename/Undo/Redo; corpus compilation is
    // an independent real compiler oracle over the exact evaluated selected framework.
    auto build = build::managedProjectBuildSpec(request);
    ASSERT_TRUE(build);
    build->environment = options.environment;
    build->arguments.push_back("--no-restore");
    // The pinned corpus's SDK 9 style policy promotes SDK 10 analyzer additions to errors.
    // This scratch-retargeted build checks compilation of the evaluated sources.
    build->arguments.push_back("-p:EnforceCodeStyleInBuild=false");
    auto built = io::runToCompletion(io::realProcessLauncher(), *build, {}, 5min);
    ASSERT_TRUE(built);
    EXPECT_EQ(built->exitCode, 0) << built->stdoutBytes << built->stderrBytes;
}
} // namespace
