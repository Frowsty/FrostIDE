// SPDX-License-Identifier: Apache-2.0
// Make's real application backend over scripted children: admission, Save All and failure outcomes.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "../../src/app/src/standalone_build_backend.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <gtest/gtest.h>

namespace frost::app {
namespace {
std::string path(std::string_view name = {}) {
    return io::joinPath(io::kNativePathStyle == io::PathStyle::Windows ? "C:/make-test" : "/make-test", name).value();
}
bool pumpUntil(const std::function<bool()>& predicate) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}
TEST(StandaloneBuildBackend, CancelledSaveDoesNotLaunchAndReleasesReservation) {
    io::MemFs fs(systemClock());
    io::ScriptedProcessLauncher launcher;
    auto trust = project::TrustStore::open(fs, path("data/trust.txt"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(path()));
    auto admission = build::BuildService::create(fs, launcher, **trust);
    StandaloneBuildBackend backend({.root = path(), .makefile = path("Makefile")}, fs, launcher, **trust, *admission);
    build::SaveAllCompletion completeSave;
    backend.setSaveAllHook([&](auto done) { completeSave = std::move(done); });
    ASSERT_TRUE(backend.build(build::BuildAction::Build, {}));
    EXPECT_EQ(backend.state(), build::BuildState::SavingDocuments);
    EXPECT_FALSE(admission->reserveRun());
    backend.cancel();
    completeSave({});
    QCoreApplication::processEvents();
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_EQ(backend.state(), build::BuildState::Idle);
    ASSERT_TRUE(admission->reserveRun());
    admission->releaseRun();
}
TEST(StandaloneBuildBackend, FailedRebuildSecondStepCannotReportCleanSuccess) {
    io::MemFs fs(systemClock());
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = path("make"), .exitCode = 0});
    launcher.expect({.expectExecutable = path("make"), .launchError = Error(ErrorCode::NotFound, "compiler missing")});
    auto trust = project::TrustStore::open(fs, path("data/trust.txt"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(path()));
    auto admission = build::BuildService::create(fs, launcher, **trust);
    std::optional<build::BuildOutcome> outcome;
    build::BuildServiceCallbacks callbacks;
    callbacks.onFinished = [&](const auto& value) { outcome = value; };
    StandaloneBuildBackend backend({.root = path(), .makefile = path("Makefile")}, fs, launcher, **trust, *admission,
                                   callbacks, []() -> Result<std::string> { return path("make"); });
    ASSERT_TRUE(backend.build(build::BuildAction::Rebuild, {}));
    ASSERT_TRUE(pumpUntil([&] { return outcome.has_value(); }));
    EXPECT_FALSE(outcome->succeeded());
    EXPECT_FALSE(outcome->result.success);
    ASSERT_TRUE(outcome->error);
    EXPECT_EQ(outcome->error->code(), ErrorCode::NotFound);
    ASSERT_EQ(launcher.launches().size(), 2u);
    EXPECT_EQ(launcher.launches()[0].spec.arguments.back(), "clean");
    EXPECT_TRUE(launcher.launches()[0].spec.killOnClose);
    EXPECT_EQ(backend.state(), build::BuildState::Idle);
}
} // namespace
} // namespace frost::app
