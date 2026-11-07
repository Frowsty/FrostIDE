// SPDX-License-Identifier: Apache-2.0
// Functional tests for managed build/query/source-check sequencing and cancellation.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/rpc/json.hpp"

#include "../../src/app/src/managed_debug_preparation.hpp"
#include "../../src/app/src/managed_source_snapshot.hpp"
#include "../build/build_test_util.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <gtest/gtest.h>

#include <optional>
#include <utility>

namespace frost::app {
namespace {

bool waitForResult(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    // Returns the evaluation that ended the wait (as tests/app/workbench_rig.hpp does): evaluating
    // again after the loop could see a state changed in between (R0 follow-up 16).
    for (;;) {
        if (ready())
            return true;
        if (timer.elapsed() >= 10000)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}

rpc::Json manifest(std::string_view source) {
    const std::string hash(64, 'a');
    return rpc::Json::object()
        .set("moduleId", "adc9a846-ff68-4338-95ab-732080f453c5")
        .set("assemblySha256", hash)
        .set("pdbSha256", hash)
        .set("sources", rpc::Json::array({rpc::Json::object().set("path", source).set("sha256", hash)}));
}

std::string sha256(std::string_view bytes) {
    auto hash = QCryptographicHash::hash(QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size())),
                                         QCryptographicHash::Sha256)
                    .toHex();
    return hash.toStdString();
}

TEST(ManagedSourceSnapshot, CapturesVerifiedBytesAndRejectsChanges) {
    build::test::MemTree tree;
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("data")));
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("project/bin")));
    auto trust = project::TrustStore::open(tree.fs, build::test::MemTree::path("data/trust.txt"));
    ASSERT_TRUE(trust);
    const auto root = build::test::MemTree::path("project");
    const auto assembly = build::test::MemTree::path("project/bin/App.dll");
    const auto pdb = build::test::MemTree::path("project/bin/App.pdb");
    const auto source = build::test::MemTree::path("project/Program.cs");
    ASSERT_TRUE((*trust)->trust(root));
    ASSERT_TRUE(tree.fs.writeFileAtomic(assembly, "assembly"));
    ASSERT_TRUE(tree.fs.writeFileAtomic(pdb, "symbols"));
    ASSERT_TRUE(tree.fs.writeFileAtomic(source, "source"));
    PreparedManagedDebug prepared;
    prepared.target.assembly = assembly;
    prepared.moduleId = "adc9a846-ff68-4338-95ab-732080f453c5";
    prepared.assemblySha256 = sha256("assembly");
    prepared.pdbSha256 = sha256("symbols");
    prepared.sources.push_back({source, sha256("source")});
    auto snapshot = captureManagedSources(tree.fs, **trust, root, prepared);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->sources.at(source), "source");
    auto cancelled = captureManagedSources(tree.fs, **trust, root, prepared, [] { return true; });
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
    ASSERT_TRUE(tree.fs.writeFileAtomic(source, "changed"));
    EXPECT_EQ(snapshot->sources.at(source), "source");
    EXPECT_FALSE(captureManagedSources(tree.fs, **trust, root, prepared));
    ASSERT_TRUE((*trust)->untrust(root));
    EXPECT_FALSE(captureManagedSources(tree.fs, **trust, root, prepared));
}

TEST(ManagedDebugPreparation, BuildsQueriesAndChecksEveryReportedSource) {
    build::test::MemTree tree;
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("data")));
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("project/bin")));
    auto trust = project::TrustStore::open(tree.fs, build::test::MemTree::path("data/trust.txt"));
    ASSERT_TRUE(trust);
    const std::string root = build::test::MemTree::path("project");
    ASSERT_TRUE((*trust)->trust(root));
    const std::string assembly = build::test::MemTree::path("project/bin/App.dll");
    const std::string source = build::test::MemTree::path("project/Program.cs");
    const std::string dotnet = build::test::MemTree::path("sdk/dotnet");
    const std::string guard = build::test::MemTree::path("sdk/PdbGuard.dll");
    auto props = rpc::Json::object()
                     .set("TargetPath", assembly)
                     .set("TargetFramework", "net10.0")
                     .set("TargetFrameworks", "")
                     .set("OutputType", "Exe")
                     .set("RuntimeIdentifier", "");
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = dotnet});
    launcher.expect({.expectExecutable = dotnet, .stdoutBytes = rpc::Json::object().set("Properties", props).dump()});
    launcher.expect({.expectExecutable = dotnet, .stdoutBytes = manifest(source).dump()});
    ManagedDebugPreparation preparation(launcher, **trust, systemClock());
    build::ManagedProjectRequest request{.dotnet = dotnet,
                                         .project = build::test::MemTree::path("project/App.csproj"),
                                         .configuration = "Debug",
                                         .targetFramework = "net10.0"};
    std::optional<Result<PreparedManagedDebug>> answer;
    ASSERT_TRUE(preparation.start(
        request, {root}, guard, [] { return true; },
        [&](Result<PreparedManagedDebug> result) { answer = std::move(result); }));
    ASSERT_TRUE(waitForResult([&] { return answer.has_value(); }));
    ASSERT_TRUE(*answer);
    EXPECT_EQ((*answer)->target.assembly, assembly);
    ASSERT_EQ((*answer)->sources.size(), 1U);
    EXPECT_EQ((*answer)->sources.front().path, source);
    EXPECT_EQ((*answer)->sources.front().sha256, std::string(64, 'a'));
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 3U);
    EXPECT_EQ(launches[0].spec.arguments[0], "build");
    EXPECT_EQ(launches[1].spec.arguments[0], "msbuild");
    EXPECT_EQ(launches[2].spec.arguments[0], guard);
    EXPECT_EQ(launches[2].spec.arguments.back(), "--manifest");
    EXPECT_FALSE(preparation.active());
}

TEST(ManagedDebugPreparation, CancellationTerminatesTheOwnedBuild) {
    build::test::MemTree tree;
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("data")));
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("project")));
    auto trust = project::TrustStore::open(tree.fs, build::test::MemTree::path("data/trust.txt"));
    ASSERT_TRUE(trust);
    const std::string root = build::test::MemTree::path("project");
    ASSERT_TRUE((*trust)->trust(root));
    const std::string dotnet = build::test::MemTree::path("sdk/dotnet");
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = dotnet, .neverExits = true});
    ManagedDebugPreparation preparation(launcher, **trust, systemClock());
    build::ManagedProjectRequest request{.dotnet = dotnet,
                                         .project = build::test::MemTree::path("project/App.csproj"),
                                         .configuration = "Debug",
                                         .targetFramework = "net10.0"};
    std::optional<Result<PreparedManagedDebug>> answer;
    ASSERT_TRUE(preparation.start(
        request, {root}, build::test::MemTree::path("sdk/PdbGuard.dll"), [] { return true; },
        [&](Result<PreparedManagedDebug> result) { answer = std::move(result); }));
    preparation.cancel();
    ASSERT_TRUE(waitForResult([&] { return answer.has_value(); }));
    ASSERT_FALSE(*answer);
    EXPECT_EQ(answer->error().code(), ErrorCode::Cancelled);
    ASSERT_EQ(launcher.launches().size(), 1U);
    EXPECT_TRUE(launcher.launches().front().terminated);
}

TEST(ManagedDebugPreparation, WorkspaceChangeSuppressesTheNextProcess) {
    build::test::MemTree tree;
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("data")));
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("project")));
    auto trust = project::TrustStore::open(tree.fs, build::test::MemTree::path("data/trust.txt"));
    ASSERT_TRUE(trust);
    const std::string root = build::test::MemTree::path("project");
    ASSERT_TRUE((*trust)->trust(root));
    const std::string dotnet = build::test::MemTree::path("sdk/dotnet");
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = dotnet});
    ManagedDebugPreparation preparation(launcher, **trust, systemClock());
    build::ManagedProjectRequest request{.dotnet = dotnet,
                                         .project = build::test::MemTree::path("project/App.csproj"),
                                         .configuration = "Debug",
                                         .targetFramework = "net10.0"};
    bool current = true;
    std::optional<Result<PreparedManagedDebug>> answer;
    ASSERT_TRUE(preparation.start(
        request, {root}, build::test::MemTree::path("sdk/PdbGuard.dll"), [&] { return current; },
        [&](Result<PreparedManagedDebug> result) { answer = std::move(result); }));
    current = false;
    ASSERT_TRUE(waitForResult([&] { return answer.has_value(); }));
    ASSERT_FALSE(*answer);
    EXPECT_EQ(answer->error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(launcher.launches().size(), 1U);
}

TEST(ManagedDebugPreparation, LargeBuildOutputDoesNotLoseCompletion) {
    build::test::MemTree tree;
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("data")));
    ASSERT_TRUE(tree.fs.createDirectories(build::test::MemTree::path("project/bin")));
    auto trust = project::TrustStore::open(tree.fs, build::test::MemTree::path("data/trust.txt"));
    ASSERT_TRUE(trust);
    const std::string root = build::test::MemTree::path("project");
    ASSERT_TRUE((*trust)->trust(root));
    const std::string dotnet = build::test::MemTree::path("sdk/dotnet");
    const std::string assembly = build::test::MemTree::path("project/bin/App.dll");
    const std::string source = build::test::MemTree::path("project/Program.cs");
    const auto props = rpc::Json::object()
                           .set("TargetPath", assembly)
                           .set("TargetFramework", "net10.0")
                           .set("TargetFrameworks", "")
                           .set("OutputType", "Exe")
                           .set("RuntimeIdentifier", "");
    std::string flood;
    for (int i = 0; i < 10000; ++i)
        flood += "build output\n";
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = dotnet, .stdoutBytes = flood});
    launcher.expect({.expectExecutable = dotnet, .stdoutBytes = rpc::Json::object().set("Properties", props).dump()});
    launcher.expect({.expectExecutable = dotnet, .stdoutBytes = manifest(source).dump()});
    ManagedDebugPreparation preparation(launcher, **trust, systemClock());
    build::ManagedProjectRequest request{.dotnet = dotnet,
                                         .project = build::test::MemTree::path("project/App.csproj"),
                                         .configuration = "Debug",
                                         .targetFramework = "net10.0"};
    std::optional<Result<PreparedManagedDebug>> answer;
    ASSERT_TRUE(preparation.start(
        request, {root}, build::test::MemTree::path("sdk/PdbGuard.dll"), [] { return true; },
        [&](Result<PreparedManagedDebug> result) { answer = std::move(result); },
        [](build::OutputStream, std::string_view) {}));
    ASSERT_TRUE(waitForResult([&] { return answer.has_value(); }));
    ASSERT_TRUE(*answer);
    EXPECT_EQ(launcher.launches().size(), 3U);
}

} // namespace
} // namespace frost::app
