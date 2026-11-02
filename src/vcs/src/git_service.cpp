// SPDX-License-Identifier: Apache-2.0
//
// GitService (frost/vcs/git_service.hpp): the two queues, the debounced refresh, the operations
// and the reads.

#include "frost/vcs/git_service.hpp"

#include "frost/core/mutex.hpp"

#include "environment.hpp"
#include "serial_queue.hpp"

#include <array>

namespace frost::vcs {
namespace {

#ifdef _WIN32
constexpr char kSeparator = '\\';
#else
constexpr char kSeparator = '/';
#endif

using Clock = std::chrono::steady_clock;

// Whether `key` lies strictly below `rootKey` (both vcsPathKey spellings); sets `rest` to the part after it.
bool below(std::string_view key, std::string_view rootKey, std::string_view& rest) {
    std::size_t prefix = rootKey.size();
    if (prefix > 0 && rootKey.back() == kSeparator) {
        --prefix;
    }
    if (rootKey.empty() || key.size() <= prefix + 1 || key.compare(0, prefix, rootKey.substr(0, prefix)) != 0 ||
        key[prefix] != kSeparator) {
        return false;
    }
    rest = key.substr(prefix + 1);
    return true;
}

} // namespace

std::string_view toString(GitOperationKind kind) noexcept {
    switch (kind) {
    case GitOperationKind::Commit: return "Commit";
    case GitOperationKind::Rollback: return "Rollback";
    case GitOperationKind::Checkout: return "Checkout";
    case GitOperationKind::CreateBranch: return "New Branch";
    case GitOperationKind::Fetch: return "Fetch";
    case GitOperationKind::Pull: return "Pull";
    case GitOperationKind::Push: return "Push";
    }
    return "Commit";
}

struct GitService::State {
    State(GitRunner gitRunner, io::IFileSystem& fileSystem, RepositoryInfo repositoryInfo,
          GitServiceOptions serviceOptions, GitServiceCallbacks serviceCallbacks)
        : runner(std::move(gitRunner)), fs(&fileSystem), repository(runner, fileSystem, std::move(repositoryInfo)),
          options(serviceOptions), callbacks(std::move(serviceCallbacks)),
          workTreeKey(vcsPathKey(repository.info().workTree)), gitDirKey(vcsPathKey(repository.info().gitDir)) {}

    GitRunner runner;
    io::IFileSystem* fs;
    GitRepository repository; // Holds a reference to `runner`; State never moves.
    GitServiceOptions options;
    GitServiceCallbacks callbacks;
    std::string workTreeKey;
    std::string gitDirKey;

    mutable Mutex mutex;
    std::shared_ptr<const GitStatusSnapshot> snapshot FROST_GUARDED_BY(mutex);
    std::atomic<std::uint64_t> generation{0};

    // Declared last, destroyed first: the operation queue goes before the read queue it posts to.
    detail::SerialQueue reads;
    detail::SerialQueue operations;

    [[nodiscard]] RunControl control(std::stop_token stop) const {
        RunControl run;
        run.stop = std::move(stop);
        if (callbacks.onOutput) {
            run.onLine = callbacks.onOutput;
        }
        return run;
    }

    void refreshNow() {
        const auto now = Clock::now();
        reads.schedule(now, now, [this](std::stop_token stop) { refresh(stop); });
    }

    void refresh(std::stop_token stop) {
        auto status = repository.status(stop);
        if (!status) {
            if (status.error().code() != ErrorCode::Cancelled && callbacks.onStatusFailed) {
                callbacks.onStatusFailed(status.error());
            }
            return;
        }
        std::shared_ptr<const GitStatusSnapshot> next;
        {
            LockGuard lock(mutex);
            if (snapshot && snapshot->status == *status) {
                return; // Nothing changed: no new generation, no callback.
            }
            const std::uint64_t number = generation.load() + 1;
            auto files = std::make_shared<const VcsStatusMap>(repository.info().workTree, *status, number);
            next = std::make_shared<const GitStatusSnapshot>(
                GitStatusSnapshot{std::move(*status), std::move(files), number});
            snapshot = next;
            generation.store(number);
        }
        if (callbacks.onStatusChanged) {
            callbacks.onStatusChanged(next);
        }
    }

    void started(GitOperationKind kind) const {
        if (callbacks.onOperationStarted) {
            callbacks.onOperationStarted(kind);
        }
    }

    void finished(const GitOperationResult& result) {
        if (callbacks.onOperationFinished) {
            callbacks.onOperationFinished(result);
        }
        refreshNow();
    }

    // Runs `body` as operation `kind`: started, the body's error or output, finished.
    template<class Body>
    void operation(GitOperationKind kind, std::string target, Body body) {
        operations.post([this, kind, target = std::move(target), body = std::move(body)](std::stop_token stop) mutable {
            started(kind);
            GitOperationResult result;
            result.kind = kind;
            result.target = target;
            body(stop, result);
            finished(result);
        });
    }

    void pushNow(const PushOptions& pushOptions, std::stop_token stop) {
        started(GitOperationKind::Push);
        GitOperationResult result;
        result.kind = GitOperationKind::Push;
        auto pushed = repository.push(pushOptions, control(std::move(stop)));
        if (pushed) {
            result.output = pushed->stdoutBytes + pushed->stderrBytes;
        } else {
            result.error = std::move(pushed.error());
        }
        finished(result);
    }
};

GitService::GitService(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}

GitService::~GitService() = default;

std::unique_ptr<GitService> GitService::create(GitRunner runner, io::IFileSystem& fs, RepositoryInfo info,
                                               GitServiceOptions options, GitServiceCallbacks callbacks) {
    auto state = std::make_unique<State>(std::move(runner), fs, std::move(info), options, std::move(callbacks));
    std::unique_ptr<GitService> service(new GitService(std::move(state)));
    service->refreshStatus();
    return service;
}

const RepositoryInfo& GitService::info() const noexcept { return m_state->repository.info(); }

const GitRepository& GitService::repository() const noexcept { return m_state->repository; }

std::shared_ptr<const GitStatusSnapshot> GitService::snapshot() const {
    LockGuard lock(m_state->mutex);
    return m_state->snapshot;
}

std::optional<VcsFileStatus> GitService::statusOf(std::string_view absolutePath) const {
    const std::shared_ptr<const GitStatusSnapshot> current = snapshot();
    if (!current) {
        return std::nullopt;
    }
    return current->files->statusOf(absolutePath);
}

std::uint64_t GitService::generation() const noexcept { return m_state->generation.load(); }

void GitService::refreshStatus() { m_state->refreshNow(); }

bool GitService::affectsStatus(std::string_view path) const {
    const std::string key = vcsPathKey(path);
    if (key.empty()) {
        return false;
    }
    std::string_view rest;
    if (below(key, m_state->gitDirKey, rest)) {
        // Inside the git directory only the files that change what status prints matter.
        std::string name = detail::asciiLower(rest);
        for (char& c : name) {
            if (c == '\\') {
                c = '/';
            }
        }
        static constexpr std::array<std::string_view, 7> kFiles{
            "index", "head", "packed-refs", "merge_head", "cherry_pick_head", "revert_head", "orig_head"};
        for (const std::string_view file : kFiles) {
            if (name == file) {
                return true;
            }
        }
        return (name.starts_with("refs/") && !name.ends_with(".lock")) || name.starts_with("rebase-merge") ||
               name.starts_with("rebase-apply");
    }
    if (key == m_state->gitDirKey) {
        return true; // The git directory itself was replaced or removed.
    }
    return key == m_state->workTreeKey || below(key, m_state->workTreeKey, rest);
}

void GitService::applyChanges(const io::FileChangeBatch& batch) {
    bool relevant = false;
    for (const io::FileChange& change : batch.changes) {
        if (affectsStatus(change.path) || (!change.oldPath.empty() && affectsStatus(change.oldPath))) {
            relevant = true;
            break;
        }
    }
    if (!relevant) {
        return;
    }
    const auto now = Clock::now();
    const auto debounce = std::chrono::duration_cast<Clock::duration>(m_state->options.debounce);
    const auto maxDelay = std::chrono::duration_cast<Clock::duration>(m_state->options.maxDelay);
    State* state = m_state.get();
    m_state->reads.schedule(now + debounce, now + maxDelay, [state](std::stop_token stop) { state->refresh(stop); });
}

sched::Task<Result<void>> GitService::follow(io::IDirectoryWatch& watch, std::stop_token token) {
    for (;;) {
        auto batch = co_await watch.next(token);
        if (!batch) {
            co_return std::unexpected(std::move(batch.error()));
        }
        applyChanges(*batch);
    }
}

void GitService::commit(CommitRequest request, bool pushAfter) {
    State* state = m_state.get();
    state->operation(
        GitOperationKind::Commit, {},
        [state, request = std::move(request), pushAfter](std::stop_token stop, GitOperationResult& result) {
            auto committed = state->repository.commit(request, state->control(stop));
            if (committed) {
                result.commit = committed->commit;
                result.output = std::move(committed->output);
            } else {
                result.error = std::move(committed.error());
            }
            if (pushAfter && result.succeeded()) {
                // Commit and Push: the push follows as its own operation, after the commit's result.
                state->operations.post([state](std::stop_token pushStop) { state->pushNow(PushOptions{}, pushStop); });
            }
        });
}

void GitService::rollback(std::vector<std::string> absolutePaths) {
    State* state = m_state.get();
    state->operation(GitOperationKind::Rollback, {},
                     [state, paths = std::move(absolutePaths)](std::stop_token stop, GitOperationResult& result) {
                         if (auto done = state->repository.rollback(paths, state->control(stop)); !done) {
                             result.error = std::move(done.error());
                         }
                     });
}

void GitService::checkout(std::string branch) {
    State* state = m_state.get();
    std::string target = branch;
    state->operation(GitOperationKind::Checkout, std::move(target),
                     [state, branch = std::move(branch)](std::stop_token stop, GitOperationResult& result) {
                         if (auto done = state->repository.checkout(branch, state->control(stop)); !done) {
                             result.error = std::move(done.error());
                         }
                     });
}

void GitService::createBranch(std::string name) {
    State* state = m_state.get();
    std::string target = name;
    state->operation(GitOperationKind::CreateBranch, std::move(target),
                     [state, name = std::move(name)](std::stop_token stop, GitOperationResult& result) {
                         if (auto done = state->repository.createBranch(name, state->control(stop)); !done) {
                             result.error = std::move(done.error());
                         }
                     });
}

void GitService::fetch() {
    State* state = m_state.get();
    state->operation(GitOperationKind::Fetch, {}, [state](std::stop_token stop, GitOperationResult& result) {
        auto fetched = state->repository.fetch(state->control(stop));
        if (fetched) {
            result.output = fetched->stdoutBytes + fetched->stderrBytes;
        } else {
            result.error = std::move(fetched.error());
        }
    });
}

void GitService::pull(PullStrategy strategy) {
    State* state = m_state.get();
    state->operation(GitOperationKind::Pull, {}, [state, strategy](std::stop_token stop, GitOperationResult& result) {
        auto pulled = state->repository.pull(strategy, state->control(stop));
        if (pulled) {
            result.output = pulled->stdoutBytes + pulled->stderrBytes;
        } else {
            result.error = std::move(pulled.error());
        }
    });
}

void GitService::push(PushOptions options) {
    State* state = m_state.get();
    state->operations.post([state, options](std::stop_token stop) { state->pushNow(options, stop); });
}

void GitService::cancelOperation() { m_state->operations.cancelRunning(); }

bool GitService::operationBusy() const { return m_state->operations.busy(); }

void GitService::readHeadContent(std::string absolutePath,
                                 std::function<void(Result<std::optional<std::string>>)> done) {
    State* state = m_state.get();
    state->reads.post([state, path = std::move(absolutePath), done = std::move(done)](std::stop_token) {
        done(state->repository.headContent(path));
    });
}

void GitService::readIndexContent(std::string absolutePath,
                                  std::function<void(Result<std::optional<std::string>>)> done) {
    State* state = m_state.get();
    state->reads.post([state, path = std::move(absolutePath), done = std::move(done)](std::stop_token) {
        done(state->repository.indexContent(path));
    });
}

void GitService::readBranches(std::function<void(Result<std::vector<GitBranch>>)> done) {
    State* state = m_state.get();
    state->reads.post([state, done = std::move(done)](std::stop_token) { done(state->repository.branches()); });
}

void GitService::readLastCommitMessage(std::function<void(Result<std::string>)> done) {
    State* state = m_state.get();
    state->reads.post(
        [state, done = std::move(done)](std::stop_token) { done(state->repository.lastCommitMessage()); });
}

void GitService::readRecentCommitMessages(std::size_t count,
                                          std::function<void(Result<std::vector<std::string>>)> done) {
    State* state = m_state.get();
    state->reads.post([state, count, done = std::move(done)](std::stop_token) {
        done(state->repository.recentCommitMessages(count));
    });
}

bool GitService::waitForIdle(std::chrono::milliseconds timeout) const {
    const auto deadline = Clock::now() + timeout;
    while (true) {
        const auto left = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        };
        if (left().count() <= 0 || !m_state->operations.waitIdle(left()) || !m_state->reads.waitIdle(left())) {
            return !m_state->operations.busy() && !m_state->reads.busy();
        }
        if (!m_state->operations.busy() && !m_state->reads.busy()) {
            return true;
        }
    }
}

} // namespace frost::vcs
