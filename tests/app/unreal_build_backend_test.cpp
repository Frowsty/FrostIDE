// SPDX-License-Identifier: Apache-2.0
// Checks Unreal build launch and cancellation across save and trust changes.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "../../src/app/src/unreal_build_backend.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <gtest/gtest.h>
namespace frost::app {
namespace {
std::string at(std::string_view name = {}) {
    const std::string root = io::kNativePathStyle == io::PathStyle::Windows ? "C:\\unreal-test" : "/unreal-test";
    return name.empty() ? root : io::joinPath(root, name).value();
}
project::UnrealProject oracleProject() {
    project::UnrealProject project;
    project.projectFile = at("Oracle.uproject");
    return project;
}
build::UnrealBuildRequest oracleBuild() {
    build::UnrealBuildRequest request;
    request.tool = at("UBT");
    request.projectFile = at("Oracle.uproject");
    request.target = "OracleEditor";
    return request;
}

bool pump(const std::function<bool()>& ready) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}
TEST(UnrealBuildBackend, ConfigurationAndOpenNeverLaunchAndCancelledSaveDropsLateCompletion) {
    io::MemFs fs(systemClock());
    io::ScriptedProcessLauncher launcher;
    auto trust = project::TrustStore::open(fs, at("data/trust.txt"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(at()));
    auto admission = build::BuildService::create(fs, launcher, **trust);
    UnrealBuildBackend backend(oracleProject(), fs, launcher, **trust, *admission);
    auto request = oracleBuild();
    ASSERT_TRUE(backend.configure(request));
    EXPECT_TRUE(launcher.launches().empty());
    build::SaveAllCompletion complete;
    backend.setSaveAllHook([&](auto done) { complete = std::move(done); });
    ASSERT_TRUE(backend.build(build::BuildAction::Build, {}));
    EXPECT_EQ(backend.state(), build::BuildState::SavingDocuments);
    EXPECT_FALSE(admission->reserveExternalLaunch());
    backend.cancel();
    complete({});
    QCoreApplication::processEvents();
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_EQ(backend.state(), build::BuildState::Idle);
    ASSERT_TRUE(admission->reserveRun());
    admission->releaseRun();
}
TEST(UnrealBuildBackend, TrustRevocationDuringSavePreventsUbtAndFailureUsesActualExit) {
    io::MemFs fs(systemClock());
    io::ScriptedProcessLauncher launcher;
    auto trust = project::TrustStore::open(fs, at("data/trust.txt"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(at()));
    auto admission = build::BuildService::create(fs, launcher, **trust);
    std::optional<build::BuildOutcome> outcome;
    build::BuildServiceCallbacks callbacks;
    callbacks.onFinished = [&](const auto& value) { outcome = value; };
    UnrealBuildBackend backend(oracleProject(), fs, launcher, **trust, *admission, callbacks);
    ASSERT_TRUE(backend.configure(oracleBuild()));
    build::SaveAllCompletion complete;
    backend.setSaveAllHook([&](auto done) { complete = std::move(done); });
    ASSERT_TRUE(backend.build(build::BuildAction::Build, {}));
    ASSERT_TRUE((*trust)->untrust(at()));
    complete({});
    ASSERT_TRUE(pump([&] { return outcome.has_value(); }));
    EXPECT_FALSE(outcome->succeeded());
    EXPECT_TRUE(launcher.launches().empty());
    ASSERT_TRUE((*trust)->trust(at()));
    outcome.reset();
    backend.setSaveAllHook({});
    launcher.expect({.expectExecutable = at("UBT"), .exitCode = 7});
    ASSERT_TRUE(backend.build(build::BuildAction::Build, {}));
    ASSERT_TRUE(pump([&] { return outcome.has_value(); }));
    EXPECT_FALSE(outcome->succeeded());
    EXPECT_EQ(outcome->result.exitCode, 7);
    ASSERT_EQ(launcher.launches().size(), 1U);
}
TEST(UnrealBuildBackend, RevokeAndRegrantDuringSaveDoesNotRestoreOldOperationAuthority) {
    io::MemFs fs(systemClock());
    io::ScriptedProcessLauncher launcher;
    auto trust = project::TrustStore::open(fs, at("data/trust.txt"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(at()));
    auto admission = build::BuildService::create(fs, launcher, **trust);
    std::optional<build::BuildOutcome> outcome;
    build::BuildServiceCallbacks callbacks;
    callbacks.onFinished = [&](const auto& value) { outcome = value; };
    UnrealBuildBackend backend(oracleProject(), fs, launcher, **trust, *admission, callbacks);
    ASSERT_TRUE(backend.configure(oracleBuild()));
    build::SaveAllCompletion complete;
    backend.setSaveAllHook([&](auto done) { complete = std::move(done); });
    ASSERT_TRUE(backend.build(build::BuildAction::Build, {}));
    ASSERT_TRUE((*trust)->untrust(at()));
    ASSERT_TRUE((*trust)->trust(at()));
    complete({});
    ASSERT_TRUE(pump([&] { return outcome.has_value(); }));
    EXPECT_FALSE(outcome->succeeded());
    ASSERT_TRUE(outcome->error);
    EXPECT_EQ(outcome->error->code(), ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
}
TEST(UnrealBuildBackend, ActiveChildIsCancelledWhenTrustRevisionChanges) {
    io::MemFs fs(systemClock());
    io::ScriptedProcessLauncher launcher;
    auto trust = project::TrustStore::open(fs, at("data/trust.txt"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(at()));
    auto admission = build::BuildService::create(fs, launcher, **trust);
    std::optional<build::BuildOutcome> outcome;
    build::BuildServiceCallbacks callbacks;
    callbacks.onFinished = [&](const auto& value) { outcome = value; };
    UnrealBuildBackend backend(oracleProject(), fs, launcher, **trust, *admission, callbacks);
    ASSERT_TRUE(backend.configure(oracleBuild()));
    launcher.expect({.expectExecutable = at("UBT"), .neverExits = true});
    ASSERT_TRUE(backend.build(build::BuildAction::Build, {}));
    ASSERT_TRUE(pump([&] { return backend.state() == build::BuildState::Running; }));
    ASSERT_TRUE((*trust)->untrust(at()));
    ASSERT_TRUE((*trust)->trust(at()));
    ASSERT_TRUE(pump([&] { return outcome.has_value(); }));
    EXPECT_FALSE(outcome->succeeded());
    ASSERT_EQ(launcher.launches().size(), 1U);
    EXPECT_TRUE(launcher.launches().front().terminated);
    auto next = admission->reserveExternalLaunch();
    ASSERT_TRUE(next);
    admission->releaseExternalLaunch(*next);
    EXPECT_TRUE(backend.diagnostics().empty());
}

TEST(UnrealBuildBackend, ChangingUbtProfileClearsExplicitCompilationDatabaseWithoutLaunchingTools) {
    io::MemFs fs(systemClock());
    io::ScriptedProcessLauncher launcher;
    auto trust = project::TrustStore::open(fs, at("trust.txt"));
    ASSERT_TRUE(trust);
    auto admission = build::BuildService::create(fs, launcher, **trust);
    UnrealBuildBackend backend(oracleProject(), fs, launcher, **trust, *admission);
    auto request = oracleBuild();
    EXPECT_FALSE(backend.selectCompileDatabase(at("compile_commands.json")));
    ASSERT_TRUE(backend.configure(request));
    const auto identity = backend.configurationIdentity();
    ASSERT_TRUE(backend.selectCompileDatabase(at("compile_commands.json")));
    EXPECT_FALSE(backend.compileDatabase().empty());
    request.configuration = build::UnrealConfiguration::DebugGame;
    ASSERT_TRUE(backend.configure(request));
    EXPECT_NE(identity, backend.configurationIdentity());
    EXPECT_TRUE(backend.compileDatabase().empty());
    EXPECT_TRUE(launcher.launches().empty());
}

} // namespace
} // namespace frost::app
