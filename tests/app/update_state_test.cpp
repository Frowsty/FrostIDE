// SPDX-License-Identifier: Apache-2.0
// Real-file update floor retry, concurrency and corruption boundaries; no network.
#include "../../src/app/src/update_state.hpp"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>
#include <QThread>
#ifdef _WIN32
#include <QSaveFile>

#include <windows.h>
#else
#include <sys/stat.h>

#include <unistd.h>
#endif
#include <gtest/gtest.h>

#include <sodium.h>

namespace {
class UpdateState : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        ASSERT_GE(sodium_init(), 0);
        ASSERT_EQ(crypto_sign_keypair(key.data(), secret.data()), 0);
    }
    void TearDown() override { sodium_memzero(secret.data(), secret.size()); }
    frost::update::AuthenticatedRelease release(std::uint64_t build = 42, std::string version = "1.2.3",
                                                std::string digest = std::string(64, 'a'),
                                                std::string channel = "stable") {
        const auto json = "{\"schema\":1,\"channel\":\"" + channel +
                          "\",\"platform\":\"windows\",\"arch\":\"x64\",\"version\":\"" + version +
                          "\",\"build\":" + std::to_string(build) +
                          ",\"expires\":2000,\"asset\":{\"url\":\"https://github.com/Frowsty/FrostIDE/releases/"
                          "download/v1/Frost.msi\",\"size\":7,\"sha256\":\"" +
                          digest + "\"}}";
        frost::update::AuthenticatedRelease result;
        result.manifest.assign(reinterpret_cast<const std::byte*>(json.data()),
                               reinterpret_cast<const std::byte*>(json.data() + json.size()));
        EXPECT_EQ(crypto_sign_detached(result.signature.data(), nullptr,
                                       reinterpret_cast<const unsigned char*>(json.data()),
                                       static_cast<unsigned long long>(json.size()), secret.data()),
                  0);
        return result;
    }
    QByteArray contents() {
        QFile input(directory.path() + "/accepted-release.json");
        if (!input.open(QIODevice::ReadOnly))
            return {};
        return input.readAll();
    }
    QTemporaryDir directory;
    frost::update::PublicKey key{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    frost::update::ReleasePolicy policy{"stable", "windows", "x64", 1000, 40, 0, "1.2.2", "", ""};
};
TEST_F(UpdateState, PersistsSignedIdentityAndAllowsOnlyExactRetry) {
    auto initial = frost::app::readUpdateFloor(directory.path());
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial->build, 0U);
    auto envelope = release();
    envelope.release.build = 999; // Unauthenticated convenience fields never become state.
    auto accepted = frost::app::acceptUpdate(directory.path(), envelope, key, policy);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->build, 42U);
    const auto before = contents();
    ASSERT_FALSE(before.isEmpty());
    ASSERT_TRUE(frost::app::acceptUpdate(directory.path(), release(), key, policy));
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), release(42, "1.2.3", std::string(64, 'b')), key, policy));
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), release(42, "1.2.3+changed"), key, policy));
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), release(41), key, policy));
    EXPECT_EQ(contents(), before);
    auto reopened = frost::app::readUpdateFloor(directory.path());
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->build, 42U);
}
TEST_F(UpdateState, ReloadsHigherFloorAndChannelSwitchCannotDowngrade) {
    const auto pending = release(42);
    ASSERT_TRUE(frost::app::acceptUpdate(directory.path(), release(43, "1.3.0"), key, policy));
    const auto before = contents();
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), pending, key, policy));
    policy.channel = "eap";
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), release(44, "1.3.0-eap.1", std::string(64, 'a'), "eap"),
                                          key, policy));
    EXPECT_EQ(contents(), before);
}
TEST_F(UpdateState, HeldLockAndMalformedStateFailClosed) {
    QLockFile lock(directory.path() + "/accepted-release.lock");
    lock.setStaleLockTime(0);
    ASSERT_TRUE(lock.tryLock(0));
    EXPECT_FALSE(frost::app::readUpdateFloor(directory.path()));
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), release(), key, policy));
    lock.unlock();
    for (const QByteArray& bad :
         {QByteArray(), QByteArray("{}"), QByteArray(1025, 'x'),
          QByteArray("{\"schema\":1,\"version\":\"1.2.3\",\"build\":1,\"sha256\":\"invalid\"}")}) {
        QFile file(directory.path() + "/accepted-release.json");
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(bad), bad.size());
        file.close();
        EXPECT_FALSE(frost::app::readUpdateFloor(directory.path()));
        EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), release(), key, policy));
        EXPECT_EQ(contents(), bad);
    }
}
TEST_F(UpdateState, InvalidSignatureAndExpiredEnvelopeLeaveFloorUnchanged) {
    ASSERT_TRUE(frost::app::acceptUpdate(directory.path(), release(), key, policy));
    const auto before = contents();
    auto bad = release(43);
    bad.signature[0] ^= 1;
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), bad, key, policy));
    policy.nowUnix = 2000;
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), release(43), key, policy));
    EXPECT_EQ(contents(), before);
}
TEST_F(UpdateState, ReadOnlyStateRefusesPublicationAndPreservesPreviousFloor) {
    ASSERT_TRUE(frost::app::acceptUpdate(directory.path(), release(), key, policy));
    const auto before = contents();
    QFile file(directory.path() + "/accepted-release.json");
    ASSERT_TRUE(file.setPermissions(QFileDevice::ReadOwner));
    const auto accepted = frost::app::acceptUpdate(directory.path(), release(43), key, policy);
    const auto after = contents();
    // Restore the fixture before assertions, including on an unexpected acceptance.
    EXPECT_TRUE(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    EXPECT_FALSE(accepted);
    EXPECT_EQ(after, before);
}
TEST_F(UpdateState, IndependentProcessChild) {
    const auto mode = qEnvironmentVariable("FROST_UPDATE_STATE_CHILD_MODE");
    if (mode.isEmpty())
        GTEST_SKIP() << "Invoked by the independent-process fixture";
    const auto shared = qEnvironmentVariable("FROST_UPDATE_STATE_CHILD_DIR");
    ASSERT_FALSE(shared.isEmpty());
    // Public deterministic fixture key only; never used by application builds.
    std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
    ASSERT_EQ(crypto_sign_seed_keypair(key.data(), secret.data(), seed.data()), 0);
    if (mode == "lower") {
        auto pending = release(42);
        auto initial = frost::app::readUpdateFloor(shared);
        ASSERT_TRUE(initial);
        ASSERT_EQ(initial->build, 0U);
        QFile ready(shared + "/lower-ready");
        ASSERT_TRUE(ready.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        ready.close();
        QElapsedTimer timer;
        timer.start();
        while (!QFileInfo::exists(shared + "/continue-lower") && timer.elapsed() < 10000)
            QThread::msleep(5);
        ASSERT_TRUE(QFileInfo::exists(shared + "/continue-lower"));
        EXPECT_FALSE(frost::app::acceptUpdate(shared, pending, key, policy));
    } else {
        ASSERT_EQ(mode, "higher");
        ASSERT_TRUE(frost::app::acceptUpdate(shared, release(43, "1.3.0"), key, policy));
    }
    auto final = frost::app::readUpdateFloor(shared);
    ASSERT_TRUE(final);
    EXPECT_EQ(final->build, 43U);
    EXPECT_EQ(final->version, "1.3.0");
}
#ifdef _WIN32
TEST_F(UpdateState, CommitSharingViolationPreservesPreviousFloor) {
    ASSERT_TRUE(frost::app::acceptUpdate(directory.path(), release(), key, policy));
    const auto before = contents();
    const auto path = directory.path() + "/accepted-release.json";
    struct HeldFile {
        HANDLE handle;
        ~HeldFile() {
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
        }
    } held{CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    ASSERT_NE(held.handle, INVALID_HANDLE_VALUE);
    // Allow normal reads/writes but deny replacement. Establish that the actual
    // QSaveFile failure is at commit, after opening and writing its temporary file.
    QSaveFile probe(path);
    probe.setDirectWriteFallback(false);
    ASSERT_TRUE(probe.open(QIODevice::WriteOnly));
    ASSERT_EQ(probe.write(before), before.size());
    EXPECT_FALSE(probe.commit());
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), release(43), key, policy));
    EXPECT_EQ(contents(), before);
}
#endif
#ifndef _WIN32
TEST_F(UpdateState, FifoIsRejectedWithoutWaitingForAWriter) {
    const auto path = QFile::encodeName(directory.path() + "/accepted-release.json");
    ASSERT_EQ(mkfifo(path.constData(), 0600), 0);
    QElapsedTimer timer;
    timer.start();
    EXPECT_FALSE(frost::app::readUpdateFloor(directory.path()));
    EXPECT_FALSE(frost::app::acceptUpdate(directory.path(), release(), key, policy));
    EXPECT_LT(timer.elapsed(), 1000);
}
TEST_F(UpdateState, InaccessibleFileDoesNotBecomeAnEmptyFloor) {
    if (geteuid() == 0)
        GTEST_SKIP() << "Requires an ordinary user to exercise file permissions";
    ASSERT_TRUE(frost::app::acceptUpdate(directory.path(), release(), key, policy));
    const auto before = contents();
    const auto path = QFile::encodeName(directory.path() + "/accepted-release.json");
    ASSERT_EQ(chmod(path.constData(), 0000), 0);
    const auto read = frost::app::readUpdateFloor(directory.path());
    const auto accepted = frost::app::acceptUpdate(directory.path(), release(43), key, policy);
    EXPECT_EQ(chmod(path.constData(), 0600), 0);
    EXPECT_FALSE(read);
    EXPECT_FALSE(accepted);
    EXPECT_EQ(contents(), before);
}
#endif
} // namespace
