// SPDX-License-Identifier: Apache-2.0
// Bounded, streaming download into an owner-private temporary file.
#pragma once
#include <QObject>
#include <QPointer>
#include <QTemporaryFile>
#include <frost/update/manifest.hpp>

#include <functional>
#include <memory>
class QNetworkAccessManager;
class QNetworkReply;
namespace frost::app {
class UpdateDownload final : public QObject {
public:
    using Completion = std::function<void(Result<std::unique_ptr<QTemporaryFile>>)>;
    UpdateDownload(QNetworkAccessManager& manager, update::Release release, std::function<bool()> ready,
                   Completion completion, QObject* parent = nullptr);
    ~UpdateDownload() override;
    void start();

private:
    void fetch(const QUrl& url, int redirects);
    void finish(Result<std::unique_ptr<QTemporaryFile>> result);
    QNetworkAccessManager& m_manager;
    update::Release m_release;
    std::function<bool()> m_ready;
    Completion m_completion;
    std::unique_ptr<QTemporaryFile> m_file;
    std::uint64_t m_received = 0;
    bool m_started = false;
    bool m_finished = false;
    QPointer<QNetworkReply> m_reply;
};
} // namespace frost::app
