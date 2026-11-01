// SPDX-License-Identifier: Apache-2.0
//
/*
 * WatchEnv: the environment of the real file-watcher tests (integration, stress and perf; ADR 0020). It owns
 * a Reactor, a two-worker ThreadPool and a RealFileWatcher and tears them down in the documented exit order;
 * it waits for batches with deadlines, applies them to a WatchMirror, establishes quiescence with fence files
 * and compares the mirror with an independent std::filesystem walk of the disk.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/thread_pool.hpp"

#include "support/watch_mirror.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace frost::test {

inline constexpr char kNativeSeparator = io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';

// `path` joined with a relative name in the native style.
[[nodiscard]] inline std::string nativeJoin(const std::string& directory, std::string_view name) {
    std::string path = directory;
    if (path.back() != kNativeSeparator) {
        path += kNativeSeparator;
    }
    path += name;
    return path;
}

// Writes `data` in place (no temp file), creating or truncating the file.
inline void writePlain(const std::string& path, std::string_view data) {
    auto native = io::toStdPath(path);
    ASSERT_TRUE(native) << path;
    std::FILE* file = nullptr;
#ifdef _WIN32
    ASSERT_EQ(_wfopen_s(&file, native->c_str(), L"wb"), 0) << path;
#else
    file = std::fopen(native->c_str(), "wb");
    ASSERT_NE(file, nullptr) << path;
#endif
    ASSERT_EQ(std::fwrite(data.data(), 1, data.size(), file), data.size());
    ASSERT_EQ(std::fclose(file), 0);
}

inline void renamePath(const std::string& from, const std::string& to) {
    std::error_code error;
    std::filesystem::rename(io::toStdPath(from).value(), io::toStdPath(to).value(), error);
    ASSERT_FALSE(error) << from << " -> " << to << ": " << error.message();
}

inline void removeAll(const std::string& path) {
    std::error_code error;
    std::filesystem::remove_all(io::toStdPath(path).value(), error);
    ASSERT_FALSE(error) << path << ": " << error.message();
}

inline void makeDirectory(const std::string& path) {
    auto created = io::realFileSystem().createDirectories(path);
    ASSERT_TRUE(created) << created.error().describe();
}

/*
 * The disk as an independent oracle: a std::filesystem walk below `root` with the mirror's filters
 * (recursion, exclusions, frost temp names). Symlinks are entries, never followed.
 */
[[nodiscard]] inline MirrorTree walkDisk(const std::string& root, const MirrorOptions& options) {
    MirrorTree tree;
    std::error_code error;
    auto start = io::toStdPath(root);
    if (!start) {
        ADD_FAILURE() << root;
        return tree;
    }
    std::filesystem::recursive_directory_iterator it(*start, std::filesystem::directory_options::skip_permission_denied,
                                                     error);
    for (; !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        auto path = io::fromStdPath(it->path());
        if (!path) {
            continue;
        }
        const std::string name(io::fileName(*path));
        if (options.skipFrostTemp && name.find(".frost-tmp-") != std::string::npos) {
            continue;
        }
        const auto status = it->symlink_status(error);
        if (error) {
            error.clear(); // Vanished during the walk.
            continue;
        }
        MirrorNode node;
        if (std::filesystem::is_symlink(status)) {
            node.type = io::FileType::Symlink;
            it.disable_recursion_pending();
        } else if (std::filesystem::is_directory(status)) {
            node.type = io::FileType::Directory;
            const bool excluded = std::ranges::find(options.excluded, *path) != options.excluded.end();
            if (!options.recursive || excluded) {
                it.disable_recursion_pending();
            }
        } else {
            node.type = io::FileType::Regular;
            if (options.readContent) {
                auto content = io::realFileSystem().readFile(*path);
                if (!content) {
                    continue;
                }
                node.content = std::move(*content);
            }
        }
        tree.emplace(std::move(*path), std::move(node));
    }
    return tree;
}

[[nodiscard]] inline bool covers(const io::FileChange& change, const std::string& path, char separator) {
    if (change.path == path || (change.kind == io::FileChangeKind::Renamed && change.oldPath == path)) {
        return true;
    }
    const bool below = path.size() > change.path.size() && path.starts_with(change.path) &&
                       (change.path.back() == separator || path[change.path.size()] == separator);
    return below && (change.kind == io::FileChangeKind::Rescan || change.kind == io::FileChangeKind::Removed);
}

[[nodiscard]] inline bool mentions(const io::FileChangeBatch& batch, const std::string& path) {
    return std::ranges::any_of(batch.changes,
                               [&](const io::FileChange& change) { return covers(change, path, kNativeSeparator); });
}

[[nodiscard]] inline std::string describe(const io::FileChangeBatch& batch) {
    std::string text =
        std::format("batch {}{}:", batch.sequence, batch.mode == io::WatchMode::Polling ? " (polling)" : "");
    for (const io::FileChange& change : batch.changes) {
        text += "\n  " + describe(change);
    }
    return text;
}

class WatchEnv {
public:
    explicit WatchEnv(unsigned reactorThreads = 1) {
        auto reactor = io::Reactor::create(io::ReactorOptions{reactorThreads, "frost-test-watch"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<sched::ThreadPool>(sched::ThreadPoolOptions{.workerCount = 2});
        auto watcher = io::RealFileWatcher::create(*m_reactor, *m_pool);
        if (!watcher) {
            ADD_FAILURE() << watcher.error().describe();
            return;
        }
        m_watcher = std::move(*watcher);
    }
    // The exit order of file_watcher.hpp; the test destroyed its watches first.
    ~WatchEnv() {
        if (m_reactor) {
            m_reactor->shutdown();
        }
        m_watcher.reset();
        if (m_pool) {
            m_pool->shutdown();
        }
        m_pool.reset();
        m_reactor.reset();
    }
    WatchEnv(const WatchEnv&) = delete ("owns the reactor");
    WatchEnv& operator=(const WatchEnv&) = delete ("owns the reactor");

    [[nodiscard]] io::Reactor& reactor() { return *m_reactor; }
    [[nodiscard]] sched::ThreadPool& pool() { return *m_pool; }
    [[nodiscard]] io::RealFileWatcher& watcher() { return *m_watcher; }

    [[nodiscard]] Result<std::unique_ptr<io::IDirectoryWatch>> watch(const std::string& root,
                                                                     io::WatchOptions options = {}) {
        return sched::syncWait(m_watcher->watch(root, std::move(options)));
    }

    // The next batch, or Timeout once `timeout` passed (the pending next() is cancelled and joined).
    [[nodiscard]] Result<io::FileChangeBatch> next(io::IDirectoryWatch& watch,
                                                   std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::stop_source stop;
        auto handle = m_pool->spawn(sched::Lane::UserVisible, watch.next(stop.get_token()));
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!handle.ready() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!handle.ready()) {
            static_cast<void>(stop.request_stop());
            auto late = handle.join();
            if (!late && late.error().code() == ErrorCode::Cancelled) {
                return fail(ErrorCode::Timeout, "no batch within the deadline");
            }
            return late;
        }
        return handle.join();
    }

    /*
     * Applies batches to `mirror` (when given) until `done` accepts one; false (with a test failure naming
     * the batches seen) after `timeout` or on an error.
     */
    bool until(io::IDirectoryWatch& watch, WatchMirror* mirror,
               const std::function<bool(const io::FileChangeBatch&)>& done,
               std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::string seen;
        for (;;) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                ADD_FAILURE() << "condition not met; batches:\n" << seen;
                return false;
            }
            auto batch = next(watch, left);
            if (!batch) {
                ADD_FAILURE() << batch.error().describe() << "; batches:\n" << seen;
                return false;
            }
            seen += describe(*batch) + "\n";
            batches.push_back(*batch);
            if (mirror != nullptr) {
                mirror->apply(*batch);
            }
            if (done(*batch)) {
                return true;
            }
        }
    }

    /*
     * Quiescence: once the Linux new-directory setups are done, a fresh file in `directory` shows up in a
     * batch only after every earlier change was reported (inotify and RDCW report in order, the queue
     * keeps it); its removal is waited for as well. Then the mirror must equal the disk, and, because a
     * watcher that only said Rescan(root) would converge too, no Rescan may have been handed out unless
     * the scenario provokes one (`rescansExpected`).
     */
    void settle(io::IDirectoryWatch& watch, WatchMirror& mirror, const std::string& directory,
                const MirrorOptions& options, bool rescansExpected = false) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (watch.status().pendingSetup != 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_EQ(watch.status().pendingSetup, 0U);
        const std::string fence = nativeJoin(directory, std::format(".fence-{}", m_fences++));
        writePlain(fence, "fence");
        ASSERT_TRUE(until(watch, &mirror, [&](const io::FileChangeBatch& b) { return mentions(b, fence); }));
        removeAll(fence);
        ASSERT_TRUE(until(watch, &mirror, [&](const io::FileChangeBatch& b) { return mentions(b, fence); }));
        EXPECT_EQ(mirror.tree(), walkDisk(watch.root(), options));
        if (!rescansExpected) {
            EXPECT_EQ(watch.status().rescans, 0U);
        }
    }

    std::vector<io::FileChangeBatch> batches; // Every batch until() saw, for assertions.

private:
    std::unique_ptr<io::Reactor> m_reactor;
    std::unique_ptr<sched::ThreadPool> m_pool;
    std::unique_ptr<io::RealFileWatcher> m_watcher;
    unsigned m_fences = 0;
};

} // namespace frost::test
