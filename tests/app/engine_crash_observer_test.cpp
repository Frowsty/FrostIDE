// SPDX-License-Identifier: Apache-2.0
// FER-16 real observer holder/revocation semantics over bounded local artifact format fixtures.
#include "debug/debug_test_util.hpp"
#include "engine_crash_observer.hpp"
#include "support/crash_artifact_fixture.hpp"
#include <QCoreApplication>
#include <QThread>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <latch>
#include <thread>

namespace frost::app {
namespace {
using namespace std::chrono_literals;

class ObservedFs final : public io::IFileSystem {
public:
    explicit ObservedFs(io::MemFs& inner) : m_inner(inner), m_gui(std::this_thread::get_id()) {}
    std::function<void(std::string_view)> onRead, onList;
    std::atomic<bool> guiAccess{false};
    void accessed() {
        if (std::this_thread::get_id() == m_gui)
            guiAccess.store(true);
    }
    Result<std::string> readFile(std::string_view path) override {
        accessed();
        if (onRead)
            onRead(path);
        return m_inner.readFile(path);
    }
    Result<std::string> readFileSnapshot(std::string_view path, std::size_t maxBytes) override {
        accessed();
        if (onRead)
            onRead(path);
        return m_inner.readFileSnapshot(path, maxBytes);
    }
    Result<void> writeFileAtomic(std::string_view p, std::string_view bytes) override {
        accessed();
        return m_inner.writeFileAtomic(p, bytes);
    }
    Result<io::FileStatus> status(std::string_view p) override {
        accessed();
        return m_inner.status(p);
    }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view p) override {
        accessed();
        if (onList)
            onList(p);
        return m_inner.listDirectory(p);
    }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view p) override {
        accessed();
        return m_inner.listDirectoryWithDiagnostics(p);
    }
    Result<void> createDirectories(std::string_view p) override {
        accessed();
        return m_inner.createDirectories(p);
    }
    Result<void> removeFile(std::string_view p) override {
        accessed();
        return m_inner.removeFile(p);
    }
    Result<void> removeDirectory(std::string_view p) override {
        accessed();
        return m_inner.removeDirectory(p);
    }
    Result<bool> exists(std::string_view p) override {
        accessed();
        return m_inner.exists(p);
    }
    Result<std::string> canonicalPath(std::string_view p) override {
        accessed();
        return m_inner.canonicalPath(p);
    }

private:
    io::MemFs& m_inner;
    std::thread::id m_gui;
};
struct WorkerGate {
    std::latch gate{1};
    std::atomic<bool> entered{false}, once{false};
    bool released = false;
    void wait() {
        entered.store(true);
        gate.wait();
    }
    void release() {
        if (!released) {
            released = true;
            gate.count_down();
        }
    }
};
struct ReleaseWorkerGate {
    std::shared_ptr<WorkerGate> value;
    ~ReleaseWorkerGate() { value->release(); }
};

class EngineCrashObserverTest : public testing::Test {
protected:
    void SetUp() override {
        root = debug::test::workPath("crash-observer-source");
        project = debug::test::workPath("crash-observer-project");
        ASSERT_TRUE(memory.createDirectories(root));
        ASSERT_TRUE(memory.createDirectories(project));
        identity.instanceId = std::string(32, 'b');
        identity.generation = std::string(32, 'c');
        identity.projectPath = project;
        identity.processId = 4242;
        identity.connectionGeneration = 1;
        identity.crashArtifactSource = frost::test::syntheticCrashSource(root, project);
        holder.authenticated(identity);
        EngineDumpControllerOptions dumpOptions;
        dumpOptions.notifications = &notifications;
        dumpOptions.work = work;
        dumps = std::make_unique<EngineDumpController>(std::move(dumpOptions));
        observer = std::make_unique<EngineCrashObserver>(
            EngineCrashObserverOptions{&fs, &clock, work, dumps.get(), &notifications});
    }
    void TearDown() override {
        if (observer)
            observer->shutdown();
        observer.reset();
        if (dumps)
            dumps->shutdown();
        dumps.reset();
        work->drain();
    }
    EngineCrashObservationSource source() {
        const auto lease = holder.snapshot();
        EngineCrashObservationSource value;
        value.identity = lease->identity();
        value.live = [lease] { return lease->live(); };
        value.admission.projectRoot = project;
        value.admission.current = [lease, trusted = trusted]() -> Result<void> {
            if (!lease->admitted() || !trusted->load())
                return fail(ErrorCode::Untrusted, "The fixture source was explicitly revoked");
            return {};
        };
        value.admission.validatePhysical = [this, current = value.admission.current]() -> Result<void> {
            if (QThread::currentThread() == QCoreApplication::instance()->thread())
                physicalOnGui.store(true);
            auto valid = current();
            if (!valid)
                return valid;
            auto physical = fs.canonicalPath(project);
            if (!physical)
                return std::unexpected(physical.error());
            return current();
        };
        return value;
    }
    bool phase(EngineCrashObservationPhase expected) {
        return debug::test::waitUntil(
            [&] {
                observer->poll();
                QCoreApplication::processEvents();
                return observer->status().phase == expected;
            },
            3s);
    }
    void loss() {
        EXPECT_TRUE(holder.lost(identity, true));
        observer->unexpectedLoss(identity);
    }
    ManualClock clock{{}, IClock::SystemTime(1001000ms)};
    io::MemFs memory{clock};
    ObservedFs fs{memory};
    EngineCrashSessionHolder holder;
    engine::EngineIdentity identity;
    std::string root, project;
    std::shared_ptr<std::atomic<bool>> trusted = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> physicalOnGui{false};
    std::shared_ptr<EngineDumpWorkBatch> work = std::make_shared<EngineDumpWorkBatch>();
    ui::NotificationCenter notifications;
    std::unique_ptr<EngineDumpController> dumps;
    std::unique_ptr<EngineCrashObserver> observer;
};

TEST_F(EngineCrashObserverTest,
       AuthenticatedHolderPreservesHistoryOnlyForExactUnexpectedLossAndNeverRevivesReplacedLease) {
    const auto first = holder.snapshot();
    auto wrong = identity;
    ++wrong.connectionGeneration;
    EXPECT_FALSE(holder.lost(wrong, true));
    EXPECT_TRUE(first->live());
    EXPECT_TRUE(holder.lost(identity, true));
    EXPECT_FALSE(first->live());
    EXPECT_TRUE(first->admitted());
    holder.authenticated(wrong);
    const auto second = holder.snapshot();
    EXPECT_FALSE(first->admitted());
    EXPECT_TRUE(second->live());
    holder.revoke();
    EXPECT_FALSE(second->admitted());
    EXPECT_FALSE(holder.snapshot());
    holder.authenticated(identity);
    EXPECT_FALSE(first->admitted());
    EXPECT_TRUE(holder.lost(identity, false));
    EXPECT_FALSE(holder.snapshot()->admitted());
}

TEST_F(EngineCrashObserverTest, LiveBaselineIgnoresExistingReportsAndOffersNewImmutableArtifactAfterActualLossOnly) {
    ASSERT_TRUE(frost::test::writeSyntheticCrashReport(memory, *identity.crashArtifactSource, "20261111T120000Z-4242",
                                                       1001000));
    ASSERT_TRUE(observer->observe(source()));
    ASSERT_TRUE(phase(EngineCrashObservationPhase::Armed));
    EXPECT_TRUE(notifications.log().empty());
    clock.advance(1s);
    ASSERT_TRUE(frost::test::writeSyntheticCrashReport(memory, *identity.crashArtifactSource, "20261111T120001Z-4242"));
    loss();
    ASSERT_TRUE(phase(EngineCrashObservationPhase::Offered));
    ASSERT_EQ(notifications.log().size(), 1U);
    EXPECT_TRUE(notifications.log().front().notification.text.contains(QStringLiteral("previously authenticated")));
    EXPECT_FALSE(holder.snapshot()->live());
    EXPECT_TRUE(holder.snapshot()->admitted());
    EXPECT_FALSE(fs.guiAccess.load());
    EXPECT_FALSE(physicalOnGui.load());
    EXPECT_LE(observer->status().attempts, 32U);
    const auto action = notifications.log().front().notification.actions.front().run;
    observer->revoke();
    action();
    ASSERT_TRUE(debug::test::waitUntil([&] { return work->pending() == 0; }, 3s));
    EXPECT_EQ(notifications.log().back().notification.title, QStringLiteral("Could not open crash dump"));
}

TEST_F(EngineCrashObserverTest, LossDuringBaselineCaptureDiscardsTheLateBaseline) {
    const auto gate = std::make_shared<WorkerGate>();
    ReleaseWorkerGate release{gate};
    fs.onList = [gate](std::string_view) {
        if (!gate->once.exchange(true))
            gate->wait();
    };
    ASSERT_TRUE(observer->observe(source()));
    ASSERT_TRUE(debug::test::waitUntil([&] { return gate->entered.load(); }, 3s));
    loss();
    gate->release();
    ASSERT_TRUE(phase(EngineCrashObservationPhase::Failed));
    ASSERT_TRUE(observer->status().error);
    EXPECT_EQ(observer->status().error->code(), ErrorCode::Cancelled);
    EXPECT_TRUE(std::none_of(notifications.log().begin(), notifications.log().end(), [](const auto& notice) {
        return notice.notification.title == QStringLiteral("FrostEngine crash dump");
    }));
}

TEST_F(EngineCrashObserverTest, SourceReplacementWaitsForTheSharedWorkerAndDiscardsThePreviousCapture) {
    const auto gate = std::make_shared<WorkerGate>();
    ReleaseWorkerGate release{gate};
    fs.onList = [gate](std::string_view) {
        if (!gate->once.exchange(true))
            gate->wait();
    };
    ASSERT_TRUE(observer->observe(source()));
    ASSERT_TRUE(debug::test::waitUntil([&] { return gate->entered.load(); }, 3s));
    const auto oldLease = holder.snapshot();
    ++identity.connectionGeneration;
    holder.authenticated(identity);
    ASSERT_TRUE(observer->observe(source()));
    EXPECT_FALSE(oldLease->admitted());
    EXPECT_EQ(observer->status().phase, EngineCrashObservationPhase::Capturing);
    EXPECT_EQ(work->pending(), 1U);
    gate->release();
    ASSERT_TRUE(phase(EngineCrashObservationPhase::Armed));
    EXPECT_TRUE(notifications.log().empty());
    EXPECT_FALSE(fs.guiAccess.load());
}

TEST_F(EngineCrashObserverTest, ShutdownDuringDumpReadReturnsWithoutJoiningAndWorkerResultCannotOfferAfterDestruction) {
    ASSERT_TRUE(observer->observe(source()));
    ASSERT_TRUE(phase(EngineCrashObservationPhase::Armed));
    clock.advance(1s);
    ASSERT_TRUE(frost::test::writeSyntheticCrashReport(memory, *identity.crashArtifactSource));
    const auto gate = std::make_shared<WorkerGate>();
    ReleaseWorkerGate release{gate};
    fs.onRead = [gate](std::string_view path) {
        if (path.ends_with("crash.dmp") && !gate->once.exchange(true))
            gate->wait();
    };
    loss();
    ASSERT_TRUE(debug::test::waitUntil([&] { return gate->entered.load(); }, 3s));
    holder.revoke();
    observer.reset();
    EXPECT_EQ(work->pending(), 1U);
    gate->release();
    ASSERT_TRUE(debug::test::waitUntil([&] { return work->pending() == 0; }, 3s));
    QCoreApplication::processEvents();
    EXPECT_TRUE(notifications.log().empty());
    EXPECT_FALSE(fs.guiAccess.load());
}

TEST_F(EngineCrashObserverTest, OfflineTrustRevocationAndPhysicalFailureCannotPublishAnOffer) {
    ASSERT_TRUE(observer->observe(source()));
    ASSERT_TRUE(phase(EngineCrashObservationPhase::Armed));
    clock.advance(1s);
    ASSERT_TRUE(frost::test::writeSyntheticCrashReport(memory, *identity.crashArtifactSource));
    trusted->store(false);
    loss();
    EXPECT_EQ(observer->status().phase, EngineCrashObservationPhase::Idle);
    EXPECT_TRUE(notifications.log().empty());
    trusted->store(true);
    holder.authenticated(identity);
    fs.onList = {};
    memory.injectFault(io::FsOp::List, root, ErrorCode::IoError);
    ASSERT_TRUE(observer->observe(source()));
    ASSERT_TRUE(phase(EngineCrashObservationPhase::Failed));
    ASSERT_TRUE(observer->status().error);
    EXPECT_EQ(observer->status().error->code(), ErrorCode::IoError);
}

TEST_F(EngineCrashObserverTest, WrongIdentityCannotStartPollingAndMissingArtifactsExpireUnderTheManualClock) {
    ASSERT_TRUE(observer->observe(source()));
    ASSERT_TRUE(phase(EngineCrashObservationPhase::Armed));
    observer->unexpectedLoss(identity); // A factual loss must first clear the exact live lease.
    auto wrong = identity;
    ++wrong.connectionGeneration;
    observer->unexpectedLoss(wrong);
    EXPECT_EQ(observer->status().phase, EngineCrashObservationPhase::Armed);
    EXPECT_EQ(observer->status().attempts, 0U);
    loss();
    ASSERT_TRUE(debug::test::waitUntil([&] { return work->pending() == 0; }, 3s));
    observer->poll();
    clock.advance(6s);
    observer->poll();
    EXPECT_EQ(observer->status().phase, EngineCrashObservationPhase::Finished);
    ASSERT_TRUE(observer->status().error);
    EXPECT_EQ(observer->status().error->code(), ErrorCode::Timeout);
    EXPECT_LE(observer->status().attempts, 32U);
    EXPECT_TRUE(notifications.log().empty());
}
} // namespace
} // namespace frost::app
