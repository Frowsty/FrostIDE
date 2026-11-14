// SPDX-License-Identifier: Apache-2.0
// Bounded HTTPS release discovery; GitHub metadata does not authorize updates.
#include "update_check.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace frost::app {
namespace {
struct AsyncFetchState {
    std::mutex mutex;
    std::optional<Result<update::HttpResponse>> response;
    bool done = false;
};
constexpr auto kApi = "https://api.github.com/repos/Frowsty/FrostIDE/releases?per_page=100";
bool allowed(const QUrl& url) {
    const auto host = url.host().toLower();
    return url.isValid() && url.scheme() == QStringLiteral("https") && (url.port() == -1 || url.port() == 443) &&
           url.userInfo().isEmpty() && url.fragment().isEmpty() &&
           (host == QStringLiteral("api.github.com") || host == QStringLiteral("github.com") ||
            host == QStringLiteral("release-assets.githubusercontent.com") ||
            host == QStringLiteral("objects.githubusercontent.com"));
}
bool releaseAsset(const QUrl& url, const QString& name) {
    if (!allowed(url) || url.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) != 0 ||
        !url.query().isEmpty())
        return false;
    const auto path = url.path(QUrl::FullyEncoded);
    constexpr auto prefix = "/Frowsty/FrostIDE/releases/download/";
    if (!path.startsWith(QLatin1String(prefix)) || !path.endsWith(QLatin1Char('/') + name))
        return false;
    const auto tag = path.mid(static_cast<qsizetype>(sizeof("/Frowsty/FrostIDE/releases/download/") - 1),
                              path.size() - static_cast<qsizetype>(sizeof("/Frowsty/FrostIDE/releases/download/") - 1) -
                                  name.size() - 1);
    if (tag.isEmpty() || tag == QStringLiteral(".") || tag == QStringLiteral(".."))
        return false;
    for (auto c : tag)
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('.') && c != QLatin1Char('-') &&
            c != QLatin1Char('_'))
            return false;
    return true;
}
} // namespace
UpdateCheck::UpdateCheck(QNetworkAccessManager& manager, update::PublicKey key, update::ReleasePolicy policy,
                         std::function<bool()> ready, std::function<std::uint64_t()> clock, Completion completion,
                         QObject* parent)
    : QObject(parent), m_manager(&manager), m_key(key), m_channel(policy.channel), m_platform(policy.platform),
      m_architecture(policy.architecture), m_installedVersion(policy.installedVersion),
      m_acceptedVersion(policy.acceptedVersion), m_acceptedDigest(policy.acceptedDigest), m_policy(policy),
      m_ready(std::move(ready)), m_clock(std::move(clock)), m_completion(std::move(completion)) {
    m_policy.channel = m_channel;
    m_policy.platform = m_platform;
    m_policy.architecture = m_architecture;
    m_policy.installedVersion = m_installedVersion;
    m_policy.acceptedVersion = m_acceptedVersion;
    m_policy.acceptedDigest = m_acceptedDigest;
}
UpdateCheck::UpdateCheck(std::shared_ptr<update::IHttpClient> httpClient, update::PublicKey key,
                         update::ReleasePolicy policy, std::function<bool()> ready,
                         std::function<std::uint64_t()> clock, Completion completion, QObject* parent)
    : QObject(parent), m_httpClient(std::move(httpClient)), m_key(key), m_channel(policy.channel),
      m_platform(policy.platform), m_architecture(policy.architecture), m_installedVersion(policy.installedVersion),
      m_acceptedVersion(policy.acceptedVersion), m_acceptedDigest(policy.acceptedDigest), m_policy(policy),
      m_ready(std::move(ready)), m_clock(std::move(clock)), m_completion(std::move(completion)) {
    m_policy.channel = m_channel;
    m_policy.platform = m_platform;
    m_policy.architecture = m_architecture;
    m_policy.installedVersion = m_installedVersion;
    m_policy.acceptedVersion = m_acceptedVersion;
    m_policy.acceptedDigest = m_acceptedDigest;
}
UpdateCheck::~UpdateCheck() {
    m_finished = true;
    m_cancelled->store(true, std::memory_order_relaxed);
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
    }
    for (auto& worker : m_workers) {
        if (worker.joinable())
            worker.join();
    }
}
void UpdateCheck::start() {
    if (m_started)
        return;
    m_started = true;
    QTimer::singleShot(120000, this, [this] {
        if (!m_finished)
            finish(fail(ErrorCode::Timeout, "Update search exceeded its time limit; result is incomplete"));
    });
    fetchPage();
}
void UpdateCheck::fetchPage() {
    auto url = QString::fromLatin1(kApi);
    if (m_page > 1)
        url += QStringLiteral("&page=%1").arg(m_page);
    fetch(QUrl(url), 1024 * 1024, 0, [this](Result<QByteArray> body) {
        if (!body) {
            finish(std::unexpected(body.error()));
            return;
        }
        discover(*body);
    });
}
void UpdateCheck::finish(Result<update::AuthenticatedRelease> result) {
    if (m_finished)
        return;
    m_finished = true;
    m_cancelled->store(true, std::memory_order_relaxed);
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
    auto completion = std::move(m_completion);
    // Deliver after the network reply's signal stack unwinds: the recipient may
    // destroy this operation, its parent, or the network manager and its replies.
    QTimer::singleShot(0, this, [this, completion = std::move(completion), result = std::move(result)]() mutable {
        deleteLater();
        if (completion)
            completion(std::move(result));
    });
}
// NOLINTNEXTLINE(performance-unnecessary-value-param): URL snapshot survives readiness callbacks.
void UpdateCheck::fetch(QUrl url, qsizetype limit, int redirects, Received received) {
    if (m_finished)
        return;
    if (!m_ready || !m_ready()) {
        received(fail(ErrorCode::Cancelled, "Interactive readiness changed"));
        return;
    }
    if (!allowed(url) || redirects > 5) {
        received(fail(ErrorCode::PermissionDenied, "Update URL or redirect rejected"));
        return;
    }
    if (m_httpClient) {
        update::HttpRequest req;
        req.method = update::HttpMethod::Get;
        req.url = url.toString(QUrl::FullyEncoded).toStdString();
        req.headers = {{"Accept", "application/vnd.github+json"}};
        req.userAgent = "FrostIDE-Updater";
        req.timeout = std::chrono::milliseconds{30000};
        req.maxResponseBytes = static_cast<std::size_t>(limit);
        req.maxRedirects = static_cast<unsigned>(std::max(0, 5 - redirects));
        req.redirectValidator = [](std::string_view nextUrl) {
            return allowed(QUrl(QString::fromUtf8(nextUrl.data(), static_cast<qsizetype>(nextUrl.size()))));
        };
        auto cancelled = m_cancelled;
        req.isCancelled = [cancelled]() { return cancelled->load(std::memory_order_relaxed); };

        auto state = std::make_shared<AsyncFetchState>();
        m_workers.emplace_back([client = m_httpClient, req = std::move(req), state]() {
            auto resp = client->request(req);
            std::lock_guard lock(state->mutex);
            state->response = std::move(resp);
            state->done = true;
        });

        auto* pollTimer = new QTimer(this);
        pollTimer->setInterval(15);
        connect(pollTimer, &QTimer::timeout, this,
                [this, pollTimer, state, limit, received = std::move(received)]() mutable {
                    if (m_finished) {
                        pollTimer->stop();
                        pollTimer->deleteLater();
                        return;
                    }
                    if (!m_ready || !m_ready())
                        m_cancelled->store(true, std::memory_order_relaxed);
                    std::optional<Result<update::HttpResponse>> resp;
                    {
                        std::lock_guard lock(state->mutex);
                        if (!state->done)
                            return;
                        resp = std::move(state->response);
                    }
                    pollTimer->stop();
                    pollTimer->deleteLater();
                    if (m_cancelled->load(std::memory_order_relaxed) || !m_ready || !m_ready()) {
                        received(fail(ErrorCode::Cancelled, "Interactive readiness changed"));
                        return;
                    }
                    if (!resp || !*resp) {
                        if (resp && (resp->error().code() == ErrorCode::Cancelled ||
                                     resp->error().code() == ErrorCode::PermissionDenied)) {
                            received(std::unexpected(resp->error()));
                        } else {
                            received(fail(ErrorCode::IoError, "Update request failed or exceeded its limit"));
                        }
                        return;
                    }
                    if ((*resp)->statusCode == 403 || (*resp)->statusCode == 404) {
                        received(fail(ErrorCode::PermissionDenied,
                                      "GitHub release access is unavailable (repository access or rate limit)."));
                        return;
                    }
                    if ((*resp)->statusCode != 200 || (*resp)->body.size() > static_cast<std::size_t>(limit)) {
                        received(fail(ErrorCode::IoError, "Update request failed or exceeded its limit"));
                        return;
                    }
                    QByteArray bytes(reinterpret_cast<const char*>((*resp)->body.data()),
                                     static_cast<qsizetype>((*resp)->body.size()));
                    received(std::move(bytes));
                });
        pollTimer->start();
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "FrostIDE-Updater");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto* reply = m_manager->get(request);
    m_reply = reply;
    reply->setReadBufferSize(64 * 1024);
    auto bytes = std::make_shared<QByteArray>();
    auto oversized = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes, oversized, limit] {
        if (reply->bytesAvailable() > limit - bytes->size()) {
            *oversized = true;
            reply->abort();
            return;
        }
        bytes->append(reply->readAll());
    });
    QTimer::singleShot(30000, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
    auto* gateTimer = new QTimer(reply);
    gateTimer->setInterval(100);
    connect(gateTimer, &QTimer::timeout, this, [this, reply] {
        if (reply->isRunning() && (!m_ready || !m_ready()))
            reply->abort();
    });
    gateTimer->start();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, bytes, oversized, limit, redirects, received = std::move(received)]() mutable {
                m_reply.clear();
                const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
                if (!redirect.isEmpty() && !*oversized && reply->error() == QNetworkReply::NoError &&
                    reply->bytesAvailable() <= limit - bytes->size()) {
                    const auto target = reply->url().resolved(redirect);
                    reply->deleteLater();
                    fetch(target, limit, redirects + 1, std::move(received));
                    return;
                }
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 403 || status == 404) {
                    reply->deleteLater();
                    received(fail(ErrorCode::PermissionDenied,
                                  "GitHub release access is unavailable (repository access or rate limit)."));
                    return;
                }
                if (*oversized || reply->error() != QNetworkReply::NoError || status != 200 ||
                    reply->bytesAvailable() > limit - bytes->size()) {
                    reply->deleteLater();
                    received(fail(ErrorCode::IoError, "Update request failed or exceeded its limit"));
                    return;
                }
                bytes->append(reply->readAll());
                reply->deleteLater();
                received(std::move(*bytes));
            });
}
void UpdateCheck::discover(const QByteArray& body) {
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        finish(fail(ErrorCode::Corrupt, "Invalid GitHub releases response"));
        return;
    }
    if (doc.array().size() > 100) {
        finish(fail(ErrorCode::OutOfRange, "Update listing exceeded its page limit"));
        return;
    }
    const QString channel = QString::fromUtf8(m_policy.channel.data(), static_cast<qsizetype>(m_policy.channel.size()));
    const QString platform =
        QString::fromUtf8(m_policy.platform.data(), static_cast<qsizetype>(m_policy.platform.size()));
    const QString arch =
        QString::fromUtf8(m_policy.architecture.data(), static_cast<qsizetype>(m_policy.architecture.size()));
    const auto manifestName = QStringLiteral("frost-update-%1-%2-%3.json").arg(channel, platform, arch);
    const auto signatureName = manifestName + QStringLiteral(".sig");
    for (const auto& value : doc.array()) {
        const auto release = value.toObject();
        if (release.value(QStringLiteral("draft")).toBool(true))
            continue;
        const bool pre = release.value(QStringLiteral("prerelease")).toBool();
        const auto tag = release.value(QStringLiteral("tag_name")).toString();
        if ((channel == QStringLiteral("stable") && pre) ||
            (channel == QStringLiteral("eap") && (!pre || !tag.startsWith(QStringLiteral("eap-")))) ||
            (channel == QStringLiteral("nightly") && (!pre || !tag.startsWith(QStringLiteral("nightly-")))))
            continue;
        QUrl manifestUrl, signatureUrl;
        unsigned manifestCount = 0, signatureCount = 0;
        for (const auto& item : release.value(QStringLiteral("assets")).toArray()) {
            const auto asset = item.toObject();
            const auto name = asset.value(QStringLiteral("name")).toString();
            const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString());
            if (name == manifestName) {
                ++manifestCount;
                if (releaseAsset(url, name) && url.path().section(QLatin1Char('/'), -2, -2) == tag)
                    manifestUrl = url;
            }
            if (name == signatureName) {
                ++signatureCount;
                if (releaseAsset(url, name) && url.path().section(QLatin1Char('/'), -2, -2) == tag)
                    signatureUrl = url;
            }
        }
        if (manifestCount == 0 && signatureCount == 0)
            continue;
        if (manifestCount != 1 || signatureCount != 1 || manifestUrl.isEmpty() || signatureUrl.isEmpty()) {
            m_discoveryError = Error(ErrorCode::Corrupt, "Release metadata has an invalid or ambiguous asset pair");
            continue;
        }
        const bool duplicate = std::ranges::any_of(m_candidates, [&](const Candidate& candidate) {
            return candidate.manifest == manifestUrl && candidate.signature == signatureUrl;
        });
        if (!duplicate)
            m_candidates.push_back({manifestUrl, signatureUrl});
        if (m_candidates.size() > 32) {
            finish(fail(ErrorCode::OutOfRange, "Too many update candidates; search is incomplete"));
            return;
        }
    }
    if (doc.array().size() == 100) {
        if (m_page == 4) {
            finish(fail(ErrorCode::OutOfRange, "Update search exceeded its page budget; result is incomplete"));
            return;
        }
        ++m_page;
        fetchPage();
        return;
    }
    fetchCandidate();
}
void UpdateCheck::fetchCandidate() {
    if (m_nextCandidate == m_candidates.size()) {
        finishScan();
        return;
    }
    const auto candidate = m_candidates[m_nextCandidate++];
    m_signatureUrl = candidate.signature;
    fetch(candidate.manifest, 65536, 0, [this](Result<QByteArray> manifest) {
        if (!manifest) {
            m_discoveryError = manifest.error();
            if (manifest.error().code() == ErrorCode::Cancelled) {
                finish(std::unexpected(manifest.error()));
                return;
            }
            fetchCandidate();
            return;
        }
        m_manifest = std::move(*manifest);
        fetch(m_signatureUrl, 64, 0, [this](Result<QByteArray> signature) {
            if (!signature || signature->size() != 64) {
                m_discoveryError =
                    signature ? Error(ErrorCode::Corrupt, "Missing update signature") : signature.error();
                if (!signature && signature.error().code() == ErrorCode::Cancelled) {
                    finish(std::unexpected(signature.error()));
                    return;
                }
                fetchCandidate();
                return;
            }
            update::Signature sig{};
            std::memcpy(sig.data(), signature->constData(), sig.size());
            auto bytes = std::span(reinterpret_cast<const std::byte*>(m_manifest.constData()),
                                   static_cast<std::size_t>(m_manifest.size()));
            if (!m_ready || !m_ready() || !m_clock) {
                finish(fail(ErrorCode::Cancelled, "Interactive readiness changed"));
                return;
            }
            m_policy.nowUnix = m_clock();
            auto parsed = update::eligibleRelease(bytes, sig, m_key, m_policy);
            if (!parsed) {
                m_discoveryError = parsed.error();
            } else {
                ++m_authenticCandidates;
                if (*parsed) {
                    selectCandidate({std::move(**parsed), std::vector<std::byte>(bytes.begin(), bytes.end()), sig});
                    if (m_finished)
                        return;
                }
            }
            fetchCandidate();
        });
    });
}
void UpdateCheck::selectCandidate(update::AuthenticatedRelease candidate) {
    for (const auto& prior : m_verified) {
        const auto& a = prior.release;
        const auto& b = candidate.release;
        if (a.build == b.build) {
            if (a.version != b.version || a.digest != b.digest || a.url != b.url || a.size != b.size) {
                finish(fail(ErrorCode::Corrupt, "Signed releases disagree about the same build"));
                return;
            }
        } else {
            const auto& higher = a.build > b.build ? prior : candidate;
            const auto& lower = a.build < b.build ? prior : candidate;
            auto ordered = m_policy;
            ordered.acceptedBuild = lower.release.build;
            ordered.acceptedVersion = lower.release.version;
            if (!update::verifyRelease(higher.manifest, higher.signature, m_key, ordered)) {
                finish(fail(ErrorCode::Corrupt, "Signed release build and version order disagree"));
                return;
            }
        }
    }
    if (m_verified.empty() || candidate.release.build > m_verified[m_bestCandidate].release.build)
        m_bestCandidate = m_verified.size();
    m_verified.push_back(std::move(candidate));
}
void UpdateCheck::finishScan() {
    if (!m_ready || !m_ready() || !m_clock) {
        finish(fail(ErrorCode::Cancelled, "Interactive readiness changed"));
        return;
    }
    if (!m_verified.empty()) {
        auto winner = std::move(m_verified[m_bestCandidate]);
        m_policy.nowUnix = m_clock();
        auto checked = update::verifyRelease(winner.manifest, winner.signature, m_key, m_policy);
        if (!checked) {
            finish(std::unexpected(checked.error()));
            return;
        }
        winner.release = std::move(*checked);
        finish(std::move(winner));
    } else if (m_authenticCandidates > 0) {
        finish(fail(ErrorCode::NotFound, "No newer signed update is available"));
    } else if (m_discoveryError) {
        finish(std::unexpected(*m_discoveryError));
    } else {
        finish(fail(ErrorCode::NotFound, "No matching signed release was found"));
    }
}
} // namespace frost::app
