// SPDX-License-Identifier: Apache-2.0
//
// FindSession (frost/ui/find/find_session.hpp): starts frost::search's engine on the scheduler and
// applies its batches on the GUI thread.

#include "frost/ui/find/find_session.hpp"

#include "frost/core/assert.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/project/solution_model.hpp"

#include "find_util.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QLocale>
#include <QMetaObject>

#include <algorithm>
#include <memory>
#include <utility>

namespace frost::ui {

using find_detail::filesText;
using find_detail::fromUtf8;
using find_detail::label;
using find_detail::matchesText;

FindSession::FindSession(FindSessionServices services, QObject* parent)
    : QObject(parent), m_services(std::move(services)) {
    FROST_PRE(m_services.scheduler != nullptr && m_services.documents != nullptr);
}

FindSession::~FindSession() { stopRunning(); }

sched::Task<void> FindSession::run(sched::IScheduler& scheduler, const document::LoadService& loader,
                                   search::FindRequest request, search::FindSink sink, std::stop_token token,
                                   FindSession* session, std::uint64_t generation) {
    Result<search::FindSummary> result =
        co_await search::findInFiles(scheduler, loader, std::move(request), std::move(sink), token);
    auto shared = std::make_shared<Result<search::FindSummary>>(std::move(result));
    QMetaObject::invokeMethod(
        session, [session, generation, shared] { session->onFinished(generation, std::move(*shared)); },
        Qt::QueuedConnection);
}

void FindSession::start(FindSettings settings) {
    stopRunning();
    ++m_generation;
    m_settings = std::move(settings);
    m_files.clear();
    m_excluded.clear();
    m_matchCount = 0;
    m_summary = {};
    m_error.reset();
    emit cleared();
    if (m_settings.query.pattern.empty()) {
        emit finished();
        return;
    }

    // The scope and the open documents' text are read here, on the GUI thread (the SaveManager
    // and the solution model are owner-thread objects); the engine only sees the copies.
    std::vector<search::OpenDocumentText> open = search::SaveManagerDocuments(*m_services.documents).openDocuments();
    std::vector<std::string> openPaths;
    openPaths.reserve(open.size());
    for (const search::OpenDocumentText& document : open) {
        openPaths.push_back(document.path);
    }
    const project::SolutionModel* solution = m_services.solution ? m_services.solution() : nullptr;
    Result<search::ScopeTarget> target = search::resolveScope(m_settings.scope, solution, openPaths);
    if (!target) {
        m_error = std::move(target.error());
        emit finished();
        return;
    }
    search::FindRequest request;
    request.query = m_settings.query;
    if (!m_settings.fileMaskEnabled) {
        request.query.fileMask.clear();
    }
    request.target = std::move(*target);
    request.openDocuments = std::move(open);
    request.limits = m_services.limits;

    m_stop = std::stop_source{};
    m_running = true;
    const std::uint64_t generation = m_generation;
    // Runs on the engine's thread: only posts. The session outlives every call (stopRunning()
    // joins the job before the session goes), and Qt drops posted calls of a deleted receiver.
    search::FindSink sink = [this, generation](search::FindBatch batch) {
        auto shared = std::make_shared<search::FindBatch>(std::move(batch));
        QMetaObject::invokeMethod(
            this, [this, generation, shared] { onBatch(generation, std::move(*shared)); }, Qt::QueuedConnection);
    };
    sched::IScheduler& scheduler = *m_services.scheduler;
    m_job.emplace(scheduler.spawn(m_services.limits.lane,
                                  run(scheduler, m_services.documents->loadService(), std::move(request),
                                      std::move(sink), m_stop.get_token(), this, generation)));
}

void FindSession::rerun() { start(m_settings); }

void FindSession::cancel() { m_stop.request_stop(); }

void FindSession::wait() {
    if (m_job) {
        m_job->join();
        m_job.reset();
    }
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
}

void FindSession::stopRunning() {
    if (m_job) {
        m_stop.request_stop();
        m_job->join();
        m_job.reset();
    }
    m_running = false;
}

void FindSession::onBatch(std::uint64_t generation, search::FindBatch batch) {
    if (generation != m_generation || batch.files.empty()) {
        return;
    }
    const int first = static_cast<int>(m_files.size());
    for (search::FileMatches& file : batch.files) {
        m_matchCount += file.matches.size();
        m_excluded.emplace_back(file.matches.size(), false);
        m_files.push_back(std::move(file));
    }
    emit filesAdded(first, static_cast<int>(m_files.size()) - first);
}

void FindSession::onFinished(std::uint64_t generation, Result<search::FindSummary> result) {
    if (generation != m_generation) {
        return;
    }
    m_running = false;
    if (result) {
        m_summary = std::move(*result);
    } else {
        m_error = std::move(result.error());
    }
    emit finished();
}

QString FindSession::statusText() const {
    if (m_error) {
        return fromUtf8(m_error->message());
    }
    if (m_settings.query.pattern.empty()) {
        return {};
    }
    const std::size_t files = m_files.size();
    if (m_running) {
        return label("Searching... %1 in %2").arg(matchesText(m_matchCount), filesText(files));
    }
    if (m_matchCount == 0) {
        return m_summary.cancelled ? label("Stopped, nothing found") : label("Nothing found");
    }
    QString status = label("%1 in %2").arg(matchesText(m_matchCount), filesText(files));
    if (m_summary.limitReached) {
        status += QStringLiteral(" ") + label("(too many results: showing the first %1)")
                                            .arg(QLocale().toString(static_cast<qulonglong>(m_matchCount)));
    } else if (m_summary.cancelled) {
        status += QStringLiteral(" ") + label("(stopped)");
    }
    const std::size_t skipped = m_summary.skippedBinary + m_summary.skippedLarge + m_summary.skippedUnreadable;
    if (skipped > 0) {
        status += QStringLiteral(", ") + label("%1 skipped").arg(filesText(skipped));
    }
    return status;
}

bool FindSession::isIncluded(std::size_t file, std::size_t match) const {
    return file < m_excluded.size() && match < m_excluded[file].size() && !m_excluded[file][match];
}

void FindSession::setIncluded(std::size_t file, std::size_t match, bool included) {
    if (file >= m_excluded.size() || match >= m_excluded[file].size() || m_excluded[file][match] == !included) {
        return;
    }
    m_excluded[file][match] = !included;
    emit inclusionChanged(static_cast<int>(file), static_cast<int>(match));
}

void FindSession::setFileIncluded(std::size_t file, bool included) {
    if (file >= m_excluded.size()) {
        return;
    }
    std::ranges::fill(m_excluded[file], !included);
    emit inclusionChanged(static_cast<int>(file), -1);
}

std::size_t FindSession::includedCount() const {
    std::size_t count = 0;
    for (const std::vector<bool>& file : m_excluded) {
        count += static_cast<std::size_t>(std::ranges::count(file, false));
    }
    return count;
}

FindLocation FindSession::location(std::size_t file, std::size_t match) const {
    FROST_PRE(file < m_files.size() && match < m_files[file].matches.size());
    const search::FindMatch& found = m_files[file].matches[match];
    return FindLocation{m_files[file].path, found.line, found.column, found.length, found.offset};
}

search::ReplaceRequest FindSession::replaceRequest(std::optional<std::pair<std::size_t, std::size_t>> only) const {
    search::ReplaceRequest request;
    request.pattern = m_settings.query.pattern;
    request.options = m_settings.query.options;
    request.replacement = m_settings.replacement;
    for (std::size_t f = 0; f < m_files.size(); ++f) {
        const search::FileMatches& file = m_files[f];
        search::ReplaceFilePlan plan;
        plan.path = file.path;
        plan.textHash = file.textHash;
        for (std::size_t m = 0; m < file.matches.size(); ++m) {
            const bool wanted = only ? (only->first == f && only->second == m) : !m_excluded[f][m];
            if (!wanted) {
                continue;
            }
            const search::FindMatch& match = file.matches[m];
            plan.ranges.push_back(text::ByteRange{ByteOffset{match.offset}, ByteOffset{match.offset + match.length}});
        }
        if (!plan.ranges.empty()) {
            request.files.push_back(std::move(plan));
        }
    }
    return request;
}

} // namespace frost::ui
