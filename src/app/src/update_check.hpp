// SPDX-License-Identifier: Apache-2.0
// Asynchronous GitHub Releases discovery and authenticated manifest check.
#pragma once
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <frost/update/http_client.hpp>
#include <frost/update/manifest.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;
namespace frost::app {
class UpdateCheck final : public QObject {
public:
    using Completion = std::function<void(Result<update::AuthenticatedRelease>)>;
    // Manager is supplied by the application (or an offline fixture), and must outlive this object.
    // Call only after the application has admitted outbound updates and the user has consented.
    UpdateCheck(QNetworkAccessManager& manager, update::PublicKey key, update::ReleasePolicy policy,
                std::function<bool()> ready, std::function<std::uint64_t()> clock, Completion completion,
                QObject* parent = nullptr);
    UpdateCheck(std::shared_ptr<update::IHttpClient> httpClient, update::PublicKey key, update::ReleasePolicy policy,
                std::function<bool()> ready, std::function<std::uint64_t()> clock, Completion completion,
                QObject* parent = nullptr);
    ~UpdateCheck() override;
    void start();

private:
    using Received = std::function<void(Result<QByteArray>)>;
    void fetch(QUrl url, qsizetype limit, int redirects, Received received);
    void finish(Result<update::AuthenticatedRelease> result);
    void discover(const QByteArray& body);
    void fetchPage();
    void fetchCandidate();
    void selectCandidate(update::AuthenticatedRelease candidate);
    void finishScan();
    QNetworkAccessManager* m_manager = nullptr;
    std::shared_ptr<update::IHttpClient> m_httpClient;
    std::shared_ptr<std::atomic<bool>> m_cancelled = std::make_shared<std::atomic<bool>>(false);
    update::PublicKey m_key;
    std::string m_channel;
    std::string m_platform;
    std::string m_architecture;
    std::string m_installedVersion;
    std::string m_acceptedVersion;
    std::string m_acceptedDigest;
    update::ReleasePolicy m_policy;
    std::function<bool()> m_ready;
    std::function<std::uint64_t()> m_clock;
    Completion m_completion;
    QByteArray m_manifest;
    QUrl m_signatureUrl;
    struct Candidate {
        QUrl manifest;
        QUrl signature;
    };
    std::vector<Candidate> m_candidates;
    std::vector<update::AuthenticatedRelease> m_verified;
    std::size_t m_nextCandidate = 0;
    std::size_t m_bestCandidate = 0;
    unsigned m_page = 1;
    unsigned m_authenticCandidates = 0;
    std::optional<Error> m_discoveryError;
    bool m_started = false;
    bool m_finished = false;
    QPointer<QNetworkReply> m_reply;
    std::vector<std::thread> m_workers;
};
} // namespace frost::app
