// SPDX-License-Identifier: Apache-2.0
// Streaming size/hash validation; a verified staging file is not an installed update.
#include "update_download.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace frost::app {
namespace {
bool allowed(const QUrl& url) {
    const auto host = url.host().toLower();
    return url.isValid() && url.scheme() == QStringLiteral("https") && (url.port() == -1 || url.port() == 443) &&
           url.userInfo().isEmpty() && url.fragment().isEmpty() &&
           (host == QStringLiteral("github.com") || host == QStringLiteral("release-assets.githubusercontent.com") ||
            host == QStringLiteral("objects.githubusercontent.com"));
}
} // namespace
UpdateDownload::UpdateDownload(QNetworkAccessManager& manager, update::Release release, std::function<bool()> ready,
                               Completion completion, QObject* parent)
    : QObject(parent), m_manager(manager), m_release(std::move(release)), m_ready(std::move(ready)),
      m_completion(std::move(completion)) {}
UpdateDownload::~UpdateDownload() {
    m_finished = true;
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
    }
}
void UpdateDownload::start() {
    if (m_started)
        return;
    m_started = true;
    if (!m_ready || !m_ready()) {
        finish(fail(ErrorCode::Cancelled, "Interactive readiness changed"));
        return;
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty() || !QDir().mkpath(base + QStringLiteral("/updates"))) {
        finish(fail(ErrorCode::IoError, "Cannot create update staging directory"));
        return;
    }
    m_file = std::make_unique<QTemporaryFile>(base + QStringLiteral("/updates/staging-XXXXXX"));
    m_file->setAutoRemove(true);
    if (!m_file->open() || !m_file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        finish(fail(ErrorCode::IoError, "Cannot create private update staging file"));
        return;
    }
    fetch(QUrl(QString::fromStdString(m_release.url)), 0);
}
void UpdateDownload::finish(Result<std::unique_ptr<QTemporaryFile>> result) {
    if (m_finished)
        return;
    m_finished = true;
    auto completion = std::move(m_completion);
    // Completion can destroy the manager; never deliver it on a reply signal stack.
    QTimer::singleShot(0, this, [this, completion = std::move(completion), result = std::move(result)]() mutable {
        deleteLater();
        if (completion)
            completion(std::move(result));
    });
}
void UpdateDownload::fetch(const QUrl& url, int redirects) {
    if (!m_ready || !m_ready()) {
        finish(fail(ErrorCode::Cancelled, "Interactive readiness changed"));
        return;
    }
    if (!allowed(url) || redirects > 5) {
        finish(fail(ErrorCode::PermissionDenied, "Update URL rejected"));
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "FrostIDE-Updater");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto* reply = m_manager.get(request);
    m_reply = reply;
    reply->setReadBufferSize(64 * 1024);
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto failed = std::make_shared<bool>(false);
    auto redirectBytes = std::make_shared<std::uint64_t>(0);
    const auto drain = [this, reply, hash] {
        while (reply->bytesAvailable() > 0) {
            const QByteArray block = reply->read(64 * 1024);
            if (block.isEmpty())
                return false;
            const auto length = static_cast<std::uint64_t>(block.size());
            if (length > m_release.size - m_received || m_file->write(block) != block.size())
                return false;
            m_received += length;
            hash->addData(block);
        }
        return true;
    };
    connect(reply, &QNetworkReply::readyRead, this, [reply, failed, redirectBytes, drain] {
        if (!reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl().isEmpty()) {
            while (reply->bytesAvailable() > 0) {
                const auto block = reply->read(64 * 1024);
                if (block.isEmpty())
                    break;
                *redirectBytes += static_cast<std::uint64_t>(block.size());
                if (*redirectBytes > 64 * 1024) {
                    *failed = true;
                    reply->abort();
                    return;
                }
            }
            return;
        }
        if (!drain()) {
            *failed = true;
            reply->abort();
        }
    });
    auto* gate = new QTimer(reply);
    gate->setInterval(100);
    connect(gate, &QTimer::timeout, this, [this, reply] {
        if (reply->isRunning() && (!m_ready || !m_ready()))
            reply->abort();
    });
    gate->start();
    QTimer::singleShot(10 * 60 * 1000, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, hash, failed, redirectBytes, redirects, drain] {
        m_reply.clear();
        const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if (!redirect.isEmpty() && m_received == 0 && !*failed && reply->error() == QNetworkReply::NoError &&
            static_cast<std::uint64_t>(reply->bytesAvailable()) <= 64 * 1024 - *redirectBytes) {
            const auto target = reply->url().resolved(redirect);
            reply->deleteLater();
            fetch(target, redirects + 1);
            return;
        }
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // A finished reply may still contain bytes that did not trigger readyRead.
        // Validate those too, including a trailing byte beyond the signed size.
        const bool ok = !*failed && reply->error() == QNetworkReply::NoError && status == 200 && m_ready && m_ready() &&
                        drain() && m_received == m_release.size && m_file->flush() &&
                        hash->result() == QByteArray(reinterpret_cast<const char*>(m_release.digest.data()),
                                                     static_cast<qsizetype>(m_release.digest.size()));
        reply->deleteLater();
        if (!ok) {
            finish(fail(ErrorCode::Corrupt, "Update download failed verification"));
            return;
        }
        finish(std::move(m_file));
    });
}
} // namespace frost::app
