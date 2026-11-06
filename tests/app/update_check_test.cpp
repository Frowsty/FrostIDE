// SPDX-License-Identifier: Apache-2.0
// Offline GitHub protocol fixtures and live update-readiness tests.
#include "../../src/app/src/update_check.hpp"
#include "../../src/app/src/update_download.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <gtest/gtest.h>

#include <cstring>

#include <sodium.h>

namespace {
class Reply final : public QNetworkReply {
public:
    bool aborted = false;
    Reply(const QNetworkRequest& request, QByteArray data, QObject* parent, std::shared_ptr<bool> emitting,
          bool redirect = false, int delay = 0, QByteArray finalBytes = {})
        : QNetworkReply(parent), m_data(std::move(data)), m_emitting(std::move(emitting)) {
        setRequest(request);
        setUrl(request.url());
        setOpenMode(ReadOnly | Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, redirect ? 302 : 200);
        if (redirect)
            setAttribute(QNetworkRequest::RedirectionTargetAttribute,
                         QUrl(QStringLiteral("https://release-assets.githubusercontent.com/asset")));
        QTimer::singleShot(delay, this, [this, finalBytes = std::move(finalBytes)] {
            if (!isFinished()) {
                emit readyRead();
                if (!isFinished()) {
                    m_data.append(finalBytes);
                    setFinished(true);
                    finishSignal();
                }
            }
        });
    }
    void abort() override {
        aborted = true;
        setError(OperationCanceledError, QStringLiteral("cancelled"));
        setFinished(true);
        finishSignal();
    }
    qint64 bytesAvailable() const override { return m_data.size() - m_offset + QNetworkReply::bytesAvailable(); }

protected:
    qint64 readData(char* out, qint64 max) override {
        const auto count = std::min(max, m_data.size() - m_offset);
        if (count <= 0)
            return -1;
        std::memcpy(out, m_data.constData() + m_offset, static_cast<std::size_t>(count));
        m_offset += count;
        return count;
    }

private:
    void finishSignal() {
        auto emitting = m_emitting;
        *emitting = true;
        emit finished();
        *emitting = false;
    }
    QByteArray m_data;
    std::shared_ptr<bool> m_emitting;
    qint64 m_offset = 0;
};
class Manager final : public QNetworkAccessManager {
public:
    QByteArray listing, manifest, signature, finalBytes;
    QMap<QString, QByteArray> responses;
    bool redirect = false;
    int delay = 0;
    int requests = 0;
    QPointer<Reply> lastReply;
    std::shared_ptr<bool> emitting = std::make_shared<bool>(false);

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& req, QIODevice*) override {
        ++requests;
        const auto path = req.url().path();
        auto data = path.contains(QStringLiteral("/repos/")) ? listing
                    : path.endsWith(QStringLiteral(".sig"))  ? signature
                                                             : manifest;
        if (const auto found = responses.constFind(req.url().toString()); found != responses.cend())
            data = found.value();
        lastReply = new Reply(req, std::move(data), this, emitting, redirect, delay, finalBytes);
        return lastReply;
    }
};
TEST(UpdateLifetime, DestructionAbortsWithoutCompletionOrLaterReadinessCalls) {
    QStandardPaths::setTestModeEnabled(true);
    for (bool download : {false, true}) {
        Manager manager;
        manager.delay = 500;
        int readinessCalls = 0, completions = 0;
        auto ready = [&] {
            ++readinessCalls;
            return true;
        };
        QObject* operation = nullptr;
        if (download) {
            frost::update::Release release;
            release.url = "https://github.com/Frowsty/FrostIDE/releases/download/v1/Frost.msi";
            release.size = 1;
            auto* task = new frost::app::UpdateDownload(manager, release, ready, [&](auto) { ++completions; });
            operation = task;
            task->start();
            task->start();
        } else {
            auto* task = new frost::app::UpdateCheck(
                manager, {}, {}, ready, [] { return std::uint64_t{1000}; }, [&](auto) { ++completions; });
            operation = task;
            task->start();
            task->start();
        }
        ASSERT_EQ(manager.requests, 1);
        ASSERT_TRUE(manager.lastReply);
        delete operation;
        ASSERT_TRUE(manager.lastReply);
        EXPECT_TRUE(manager.lastReply->aborted);
        const int beforePump = readinessCalls;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 150)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        EXPECT_EQ(readinessCalls, beforePump);
        EXPECT_EQ(completions, 0);
    }
}
TEST(UpdateLifetime, CompletionMayDeleteOperationAndItsParent) {
    for (bool download : {false, true}) {
        for (bool deleteParent : {false, true}) {
            Manager manager;
            auto* owner = new QObject;
            QPointer<QObject> guardedOwner(owner);
            QObject* operation = nullptr;
            int completions = 0;
            auto completed = [&](auto) {
                ++completions;
                if (deleteParent)
                    delete owner;
                else
                    delete operation;
                operation = nullptr;
            };
            if (download) {
                auto* task = new frost::app::UpdateDownload(manager, {}, [] { return false; }, completed, owner);
                operation = task;
                task->start();
            } else {
                auto* task = new frost::app::UpdateCheck(
                    manager, {}, {}, [] { return false; }, [] { return std::uint64_t{1000}; }, completed, owner);
                operation = task;
                task->start();
            }
            QElapsedTimer timer;
            timer.start();
            while (completions == 0 && timer.elapsed() < 2000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            EXPECT_EQ(completions, 1);
            EXPECT_EQ(manager.requests, 0);
            if (guardedOwner)
                delete owner;
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
}
TEST(UpdateLifetime, ReadinessRevocationFinishesOnceWithoutRedirect) {
    QStandardPaths::setTestModeEnabled(true);
    for (bool download : {false, true}) {
        Manager manager;
        manager.delay = 500;
        manager.redirect = true;
        bool ready = true;
        int completions = 0;
        auto completed = [&](auto result) {
            ++completions;
            EXPECT_FALSE(result);
        };
        if (download) {
            frost::update::Release release;
            release.url = "https://github.com/Frowsty/FrostIDE/releases/download/v1/Frost.msi";
            release.size = 1;
            auto* task = new frost::app::UpdateDownload(manager, release, [&] { return ready; }, completed);
            task->start();
        } else {
            auto* task = new frost::app::UpdateCheck(
                manager, {}, {}, [&] { return ready; }, [] { return std::uint64_t{1000}; }, completed);
            task->start();
        }
        ready = false;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 250)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        EXPECT_EQ(completions, 1);
        EXPECT_EQ(manager.requests, 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}
TEST(UpdateLifetime, AsyncCompletionMayDestroyActualManagerAfterReplyUnwinds) {
    QStandardPaths::setTestModeEnabled(true);
    for (bool download : {false, true}) {
        for (bool cancel : {false, true}) {
            auto* manager = new Manager;
            QPointer<Manager> guardedManager(manager);
            auto emitting = manager->emitting;
            manager->delay = cancel ? 500 : 0;
            manager->manifest = "payload";
            manager->listing = "invalid-json";
            bool ready = true;
            int completions = 0;
            auto completed = [&](auto result) {
                ++completions;
                EXPECT_FALSE(*emitting);
                EXPECT_EQ(result.has_value(), download && !cancel);
                delete manager;
            };
            if (download) {
                frost::update::Release release;
                release.url = "https://github.com/Frowsty/FrostIDE/releases/download/v1/Frost.msi";
                release.size = 7;
                const auto digest = QCryptographicHash::hash(manager->manifest, QCryptographicHash::Sha256);
                std::memcpy(release.digest.data(), digest.constData(), release.digest.size());
                auto* task =
                    new frost::app::UpdateDownload(*manager, release, [&] { return ready; }, completed, manager);
                task->start();
            } else {
                auto* task = new frost::app::UpdateCheck(
                    *manager, {}, {}, [&] { return ready; }, [] { return std::uint64_t{1000}; }, completed, manager);
                task->start();
            }
            if (cancel)
                ready = false;
            QElapsedTimer timer;
            timer.start();
            while (completions == 0 && timer.elapsed() < 2000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            EXPECT_EQ(completions, 1);
            EXPECT_FALSE(guardedManager);
            if (guardedManager)
                delete manager;
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
}
TEST(UpdateCheck, VerifiesFixtureAndRechecksReadinessBeforeEachRequest) {
    ASSERT_GE(sodium_init(), 0);
    frost::update::PublicKey key{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    ASSERT_EQ(crypto_sign_keypair(key.data(), secret.data()), 0);
    Manager manager;
    manager.listing =
        R"([{"draft":false,"prerelease":false,"tag_name":"v1.2.3","assets":[{"name":"frost-update-stable-windows-x64.json","browser_download_url":"https://github.com/Frowsty/FrostIDE/releases/download/v1.2.3/frost-update-stable-windows-x64.json"},{"name":"frost-update-stable-windows-x64.json.sig","browser_download_url":"https://github.com/Frowsty/FrostIDE/releases/download/v1.2.3/frost-update-stable-windows-x64.json.sig"}]}])";
    manager.manifest =
        R"({"schema":1,"channel":"stable","platform":"windows","arch":"x64","version":"1.2.3","build":42,"expires":2000,"asset":{"url":"https://github.com/Frowsty/FrostIDE/releases/download/v1.2.3/Frost.msi","size":15,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})";
    manager.signature.resize(64);
    ASSERT_EQ(crypto_sign_detached(reinterpret_cast<unsigned char*>(manager.signature.data()), nullptr,
                                   reinterpret_cast<const unsigned char*>(manager.manifest.constData()),
                                   static_cast<unsigned long long>(manager.manifest.size()), secret.data()),
              0);
    frost::update::ReleasePolicy policy{"stable", "windows", "x64", 1000, 40, 41, "1.2.2", "1.2.2", ""};
    bool done = false, succeeded = false, ready = true;
    auto* check = new frost::app::UpdateCheck(
        manager, key, policy, [&] { return ready; }, [] { return std::uint64_t{1000}; },
        [&](auto result) {
            done = true;
            succeeded = result.has_value();
        });
    check->start();
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_TRUE(done);
    EXPECT_TRUE(succeeded);
    EXPECT_EQ(manager.requests, 3);
    done = false;
    ready = false;
    check = new frost::app::UpdateCheck(
        manager, key, policy, [&] { return ready; }, [] { return std::uint64_t{1000}; },
        [&](auto result) {
            done = true;
            succeeded = result.has_value();
        });
    check->start();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_TRUE(done);
    EXPECT_FALSE(succeeded);
    EXPECT_EQ(manager.requests, 3);
    done = false;
    ready = true;
    const int initialRequests = manager.requests;
    check = new frost::app::UpdateCheck(
        manager, key, policy, [&] { return ready && manager.requests == initialRequests; },
        [] { return std::uint64_t{1000}; },
        [&](auto result) {
            done = true;
            succeeded = result.has_value();
        });
    check->start();
    QElapsedTimer secondTimer;
    secondTimer.start();
    while (!done && secondTimer.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_TRUE(done);
    EXPECT_FALSE(succeeded);
    EXPECT_EQ(manager.requests, initialRequests + 1);
    sodium_memzero(secret.data(), secret.size());
}
class UpdateDiscovery : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_GE(sodium_init(), 0);
        ASSERT_EQ(crypto_sign_keypair(key.data(), secret.data()), 0);
    }
    void TearDown() override { sodium_memzero(secret.data(), secret.size()); }
    QJsonObject candidate(const QString& tag, std::uint64_t build, const QString& version, int expires = 2000) {
        const auto base = QStringLiteral("https://github.com/Frowsty/FrostIDE/releases/download/") + tag + '/';
        const auto name = QStringLiteral("frost-update-stable-windows-x64.json");
        const QJsonObject asset{{"url", base + "Frost.msi"}, {"size", 7}, {"sha256", QString(64, 'a')}};
        const QJsonObject contents{{"schema", 1},        {"channel", "stable"}, {"platform", "windows"},
                                   {"arch", "x64"},      {"version", version},  {"build", static_cast<qint64>(build)},
                                   {"expires", expires}, {"asset", asset}};
        const auto manifest = QJsonDocument(contents).toJson(QJsonDocument::Compact);
        QByteArray signature(64, '\0');
        EXPECT_EQ(crypto_sign_detached(reinterpret_cast<unsigned char*>(signature.data()), nullptr,
                                       reinterpret_cast<const unsigned char*>(manifest.constData()),
                                       static_cast<unsigned long long>(manifest.size()), secret.data()),
                  0);
        manager.responses.insert(base + name, manifest);
        manager.responses.insert(base + name + ".sig", signature);
        return {{"draft", false},
                {"prerelease", false},
                {"tag_name", tag},
                {"assets",
                 QJsonArray{QJsonObject{{"name", name}, {"browser_download_url", base + name}},
                            QJsonObject{{"name", name + ".sig"}, {"browser_download_url", base + name + ".sig"}}}}};
    }
    frost::Result<frost::update::AuthenticatedRelease> run(
        const QJsonArray& listing, std::function<bool()> ready = [] { return true; },
        std::function<std::uint64_t()> clock = [] { return std::uint64_t{1000}; }) {
        manager.listing = QJsonDocument(listing).toJson(QJsonDocument::Compact);
        std::optional<frost::Result<frost::update::AuthenticatedRelease>> result;
        auto* check = new frost::app::UpdateCheck(manager, key, policy, std::move(ready), std::move(clock),
                                                  [&](auto value) { result.emplace(std::move(value)); });
        check->start();
        QElapsedTimer timer;
        timer.start();
        while (!result && timer.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!result) {
            delete check;
            ADD_FAILURE() << "Update scan did not finish";
            return frost::fail(frost::ErrorCode::Timeout);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return std::move(*result);
    }
    Manager manager;
    frost::update::PublicKey key{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    frost::update::ReleasePolicy policy{"stable", "windows", "x64", 1000, 40, 0, "1.2.0", "", ""};
};
TEST_F(UpdateDiscovery, PicksNewestSignedBuildRegardlessOfListingOrder) {
    const auto old = candidate("old", 39, "1.1.0");
    const auto newer = candidate("new", 42, "1.2.2");
    const auto newest = candidate("newest", 45, "1.3.0");
    for (const auto& listing : {QJsonArray{old, newer, newest}, QJsonArray{newest, newer, old}}) {
        auto result = run(listing);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->release.build, 45U);
    }
    EXPECT_EQ(manager.requests, 14);
}
TEST_F(UpdateDiscovery, InvalidAndExpiredCandidatesNeverHideValidLaterRelease) {
    const auto expired = candidate("expired", 41, "1.2.1", 999);
    const auto bad = candidate("bad", 42, "1.2.2");
    const QString badSignature =
        "https://github.com/Frowsty/FrostIDE/releases/download/bad/frost-update-stable-windows-x64.json.sig";
    manager.responses[badSignature][0] ^= 1;
    const auto good = candidate("good", 43, "1.2.3");
    auto result = run({expired, bad, good});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->release.build, 43U);
    result = run({expired, bad});
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().code(), frost::ErrorCode::NotFound);
}
TEST_F(UpdateDiscovery, AuthenticCurrentReleaseIsNotAValidationError) {
    const auto badOld = candidate("old-retired-key", 39, "1.1.0");
    const QString badOldSignature =
        "https://github.com/Frowsty/FrostIDE/releases/download/old-retired-key/frost-update-stable-windows-x64.json.sig";
    manager.responses[badOldSignature][0] ^= 1;
    auto result = run({candidate("current", 40, "1.2.0"), badOld});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::NotFound);
    EXPECT_EQ(result.error().message(), "No newer signed update is available");
}
TEST_F(UpdateDiscovery, FindsStableReleaseOnSecondPageAndRefusesTruncatedSearch) {
    QJsonArray page;
    for (int index = 0; index < 100; ++index)
        page.append(QJsonObject{{"draft", false}, {"prerelease", true}, {"tag_name", "nightly-fixture"}});
    const auto good = candidate("good", 43, "1.2.3");
    const QString second = "https://api.github.com/repos/Frowsty/FrostIDE/releases?per_page=100&page=2";
    manager.responses[second] = QJsonDocument(QJsonArray{good}).toJson(QJsonDocument::Compact);
    auto result = run(page);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->release.build, 43U);
    EXPECT_EQ(manager.requests, 4);
    manager.responses.remove(second); // Subsequent pages deliberately repeat a full page.
    result = run(page);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::OutOfRange);
    EXPECT_EQ(manager.requests, 8);
}
TEST_F(UpdateDiscovery, RejectsAmbiguousAssetsAndConflictingSignedBuilds) {
    auto duplicate = candidate("duplicate", 42, "1.2.2");
    auto assets = duplicate["assets"].toArray();
    assets.append(assets[0]);
    duplicate["assets"] = assets;
    auto result = run({duplicate});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Corrupt);
    const auto a = candidate("a", 42, "1.2.2");
    const auto b = candidate("b", 42, "1.2.3");
    for (const auto& listing : {QJsonArray{a, b}, QJsonArray{b, a}}) {
        result = run(listing);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), frost::ErrorCode::Corrupt);
    }
    result = run({candidate("higher-version", 43, "1.4.0"), candidate("higher-build", 44, "1.3.0")});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Corrupt);
}
TEST_F(UpdateDiscovery, RefusesCandidateBudgetExhaustionBeforeDownloading) {
    QJsonArray listing;
    for (unsigned index = 0; index < 33; ++index)
        listing.append(candidate(QStringLiteral("v%1").arg(index), 41 + index, "1.2.1"));
    auto result = run(listing);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::OutOfRange);
    EXPECT_EQ(manager.requests, 1);
}
TEST_F(UpdateDiscovery, GateRevocationAndWinnerExpiryPreventOffer) {
    const auto newer = candidate("newer", 45, "1.3.0", 1001);
    const auto older = candidate("older", 42, "1.2.2");
    auto result = run({newer, older}, [&] { return manager.requests < 3; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Cancelled);
    EXPECT_EQ(manager.requests, 3);
    unsigned clockCalls = 0;
    result = run({newer, older}, [] { return true; }, [&] { return ++clockCalls < 3 ? 1000U : 1001U; });
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().code(), frost::ErrorCode::NotFound);
}
TEST(UpdateDownload, StreamsFixtureAndRejectsDigestMismatch) {
    QStandardPaths::setTestModeEnabled(true);
    Manager manager;
    manager.manifest = "fixture payload";
    frost::update::Release release;
    release.url = "https://github.com/Frowsty/FrostIDE/releases/download/v1/Frost.msi";
    release.size = static_cast<std::uint64_t>(manager.manifest.size());
    const auto digest = QCryptographicHash::hash(manager.manifest, QCryptographicHash::Sha256);
    std::memcpy(release.digest.data(), digest.constData(), release.digest.size());
    bool done = false, succeeded = false;
    std::unique_ptr<QTemporaryFile> file;
    auto run = [&] {
        auto* download = new frost::app::UpdateDownload(
            manager, release, [] { return true; },
            [&](auto result) {
                done = true;
                succeeded = result.has_value();
                if (result)
                    file = std::move(*result);
            });
        download->start();
        QElapsedTimer timer;
        timer.start();
        while (!done && timer.elapsed() < 2000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    };
    run();
    ASSERT_TRUE(done);
    ASSERT_TRUE(succeeded);
    ASSERT_TRUE(file->seek(0));
    EXPECT_EQ(file->readAll(), manager.manifest);
    file.reset();
    done = false;
    release.digest[0] ^= 1;
    run();
    EXPECT_TRUE(done);
    EXPECT_FALSE(succeeded);
}
TEST(UpdateDownload, DrainsFinalBytesAndRejectsTrailingDataAtCompletion) {
    QStandardPaths::setTestModeEnabled(true);
    const QByteArray payload("fixture payload");
    for (bool trailing : {false, true}) {
        Manager manager;
        manager.manifest = trailing ? payload : QByteArray();
        manager.finalBytes = trailing ? QByteArray("x") : payload;
        frost::update::Release release;
        release.url = "https://github.com/Frowsty/FrostIDE/releases/download/v1/Frost.msi";
        release.size = static_cast<std::uint64_t>(payload.size());
        const auto digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
        std::memcpy(release.digest.data(), digest.constData(), release.digest.size());
        bool done = false;
        std::unique_ptr<QTemporaryFile> file;
        auto* download = new frost::app::UpdateDownload(
            manager, release, [] { return true; },
            [&](auto result) {
                done = true;
                EXPECT_EQ(result.has_value(), !trailing);
                if (result)
                    file = std::move(*result);
            });
        download->start();
        QElapsedTimer timer;
        timer.start();
        while (!done && timer.elapsed() < 2000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        ASSERT_TRUE(done);
        EXPECT_EQ(manager.requests, 1);
        if (!trailing) {
            ASSERT_TRUE(file);
            ASSERT_TRUE(file->seek(0));
            EXPECT_EQ(file->readAll(), payload);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}
TEST(UpdateDownload, RejectsOversizedRedirectWithoutFollowingIt) {
    QStandardPaths::setTestModeEnabled(true);
    Manager manager;
    manager.redirect = true;
    manager.manifest = QByteArray(65537, 'x');
    frost::update::Release release;
    release.url = "https://github.com/Frowsty/FrostIDE/releases/download/v1/Frost.msi";
    release.size = 100000;
    bool done = false, succeeded = false;
    auto* download = new frost::app::UpdateDownload(
        manager, release, [] { return true; },
        [&](auto result) {
            done = true;
            succeeded = result.has_value();
        });
    download->start();
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_TRUE(done);
    EXPECT_FALSE(succeeded);
    EXPECT_EQ(manager.requests, 1);
}
TEST(UpdateCheck, CancelsInflightRequestWhenGateCloses) {
    Manager manager;
    manager.delay = 500;
    bool ready = true, done = false, succeeded = false;
    frost::update::PublicKey key{};
    frost::update::ReleasePolicy policy{"stable", "windows", "x64", 1000, 0, 0, "0.0.1", "", ""};
    auto* check = new frost::app::UpdateCheck(
        manager, key, policy, [&] { return ready; }, [] { return std::uint64_t{1000}; },
        [&](auto result) {
            done = true;
            succeeded = result.has_value();
        });
    check->start();
    QTimer::singleShot(20, [&] { ready = false; });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_TRUE(done);
    EXPECT_FALSE(succeeded);
    EXPECT_EQ(manager.requests, 1);
}
TEST(UpdateCheck, RejectsOversizedRedirectWithoutFollowingIt) {
    Manager manager;
    manager.redirect = true;
    manager.listing = QByteArray(1024 * 1024 + 1, 'x');
    frost::update::PublicKey key{};
    frost::update::ReleasePolicy policy{"stable", "windows", "x64", 1000, 0, 0, "0.0.1", "", ""};
    bool done = false, succeeded = false;
    auto* check = new frost::app::UpdateCheck(
        manager, key, policy, [] { return true; }, [] { return std::uint64_t{1000}; },
        [&](auto result) {
            done = true;
            succeeded = result.has_value();
        });
    check->start();
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_TRUE(done);
    EXPECT_FALSE(succeeded);
    EXPECT_EQ(manager.requests, 1);
}
} // namespace
