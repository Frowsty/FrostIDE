// SPDX-License-Identifier: Apache-2.0
//
// FindSession (ADR 0045): one Find or Replace in Files search as the UI shows it, shared by the
// Find in Files popup and the Find tool window: its settings, the results that stream in from
// frost::search's engine, the replace preview's exclusions and the summary.
/*
 * Threads. A session lives on the GUI thread. start() resolves the scope there (the solution model,
 * the open documents' snapshots through the SaveManager) and spawns the engine on the scheduler;
 * the engine's batches and its end are posted back to the session's thread and applied there, so
 * every member and signal is GUI-thread only. A new start() cancels the running search and waits
 * for it (quick: the engine checks its stop token between files); posted results of an older
 * search are dropped by generation. The destructor cancels and waits too, so the session may be
 * deleted at any time; the scheduler and the SaveManager must outlive it.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/search/find_in_files.hpp"
#include "frost/search/replace_in_files.hpp"
#include "frost/search/search_scope.hpp"

#include <QObject>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace frost::document {
class LoadService;
class SaveManager;
} // namespace frost::document

namespace frost::project {
class SolutionModel;
} // namespace frost::project

namespace frost::ui {

// What a session searches for, as the popup edits it.
struct FindSettings {
    search::FindQuery query; // FindQuery::fileMask is used only while fileMaskEnabled.
    search::SearchScope scope;
    bool fileMaskEnabled = false;
    bool replaceMode = false;
    std::string replacement;
};

// Where a result is: what the open-location callback receives. Lines are 0-based; column and
// length count UTF-8 bytes of the line (the editor maps them to its caret).
struct FindLocation {
    std::string path;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t length = 0;
    std::uint64_t offset = 0; // Of the match in the text that was searched.

    friend bool operator==(const FindLocation&, const FindLocation&) = default;
};

// What sessions use. Every pointer is required and outlives the sessions.
struct FindSessionServices {
    sched::IScheduler* scheduler = nullptr;
    document::SaveManager* documents = nullptr;
    // The open solution, or null (the Solution and Project scopes need one).
    std::function<const project::SolutionModel*()> solution;
    search::FindLimits limits{};
};

class FindSession final : public QObject {
    Q_OBJECT

public:
    explicit FindSession(FindSessionServices services, QObject* parent = nullptr);
    ~FindSession() override;
    FindSession(const FindSession&) = delete ("QObject instances have identity and cannot be copied");
    FindSession& operator=(const FindSession&) = delete ("QObject instances have identity and cannot be copied");

    // Clears the results and searches with `settings` (the header comment). An empty pattern only
    // clears. A scope or query error ends the search at once with error() set.
    void start(FindSettings settings);
    // start(settings()) again (the tool window's Rerun).
    void rerun();
    // Requests a stop; finished() follows with summary().cancelled.
    void cancel();
    // Blocks until the running search has ended and its results are applied (tests, teardown).
    void wait();

    [[nodiscard]] const FindSettings& settings() const noexcept { return m_settings; }
    [[nodiscard]] bool running() const noexcept { return m_running; }
    [[nodiscard]] const std::vector<search::FileMatches>& files() const noexcept { return m_files; }
    [[nodiscard]] std::size_t matchCount() const noexcept { return m_matchCount; }
    [[nodiscard]] const search::FindSummary& summary() const noexcept { return m_summary; }
    [[nodiscard]] const std::optional<Error>& error() const noexcept { return m_error; }
    // The status line the popup and the tool window show ("3 matches in 2 files", "Searching...").
    [[nodiscard]] QString statusText() const;

    // The replace preview's exclusions; every match starts included.
    [[nodiscard]] bool isIncluded(std::size_t file, std::size_t match) const;
    void setIncluded(std::size_t file, std::size_t match, bool included);
    // Includes or excludes every match of `file`.
    void setFileIncluded(std::size_t file, bool included);
    [[nodiscard]] std::size_t includedCount() const;

    // The location of a match. Precondition: the indices are in range.
    [[nodiscard]] FindLocation location(std::size_t file, std::size_t match) const;
    // The plan that replaces every included match, or only `only` (file, match) when given.
    [[nodiscard]] search::ReplaceRequest
    replaceRequest(std::optional<std::pair<std::size_t, std::size_t>> only = std::nullopt) const;

signals:
    // The results were emptied (a new search started).
    void cleared();
    // Files [first, first + count) of files() arrived.
    void filesAdded(int first, int count);
    // The search ended (done, cancelled, limited or failed).
    void finished();
    // A match's (or with match -1, a file's) inclusion changed.
    void inclusionChanged(int file, int match);

private:
    // The spawned body: runs the engine and posts its end back to `session` (alive until the job
    // is joined, which the destructor does).
    static sched::Task<void> run(sched::IScheduler& scheduler, const document::LoadService& loader,
                                 search::FindRequest request, search::FindSink sink, std::stop_token token,
                                 FindSession* session, std::uint64_t generation);
    void onBatch(std::uint64_t generation, search::FindBatch batch);
    void onFinished(std::uint64_t generation, Result<search::FindSummary> result);
    void stopRunning();

    FindSessionServices m_services;
    FindSettings m_settings;
    std::vector<search::FileMatches> m_files;
    std::vector<std::vector<bool>> m_excluded;
    std::size_t m_matchCount = 0;
    search::FindSummary m_summary;
    std::optional<Error> m_error;
    bool m_running = false;
    std::uint64_t m_generation = 0;
    std::stop_source m_stop;
    std::optional<sched::JoinHandle<void>> m_job;
};

} // namespace frost::ui
