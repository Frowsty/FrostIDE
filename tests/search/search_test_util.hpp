// SPDX-License-Identifier: Apache-2.0
//
// Helpers for the frost::search tests: a scratch tree on the real file system with files in several
// encodings, a load service with machine-independent detection options, and a synchronous runner
// for findInFiles over a small ThreadPool. Header-only; test code only.
#pragma once

#include "frost/document/load_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/search/find_in_files.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::search::test {

// The load options of every test: windows-1252 as the legacy page and LF for new lines, so the
// detection does not depend on the machine's locale.
inline document::LoadServiceOptions loadOptions() {
    document::LoadServiceOptions options;
    options.text.detect.legacyCodePage = 1252;
    options.text.defaultSeparator = text::LineSeparator::Lf;
    return options;
}

// `text` (ASCII) as UTF-16LE bytes.
inline std::string utf16le(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out += c;
        out += '\0';
    }
    return out;
}

// A scratch directory with helpers to write files below it ('/'-separated relative names).
class Tree {
public:
    explicit Tree(std::string_view tag) : m_scratch(tag) {}

    // Writes `bytes` to <root>/<relative>, creating directories. Returns the absolute path.
    std::string write(std::string_view relative, std::string_view bytes) {
        std::string native(relative);
        if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
            for (char& c : native) {
                if (c == '/') {
                    c = '\\';
                }
            }
        }
        const std::string file = m_scratch.join(native);
        EXPECT_TRUE(io::realFileSystem().createDirectories(std::string(io::parentPath(file))));
        EXPECT_TRUE(io::realFileSystem().writeFileAtomic(file, bytes)) << file;
        return file;
    }

    [[nodiscard]] std::string read(const std::string& file) const {
        auto bytes = io::realFileSystem().readFile(file);
        return bytes ? std::move(*bytes) : std::string("<unreadable>");
    }

    [[nodiscard]] const std::string& root() const noexcept { return m_scratch.path(); }

private:
    frost::test::ScratchDir m_scratch;
};

// What one findInFiles run produced.
struct FindRun {
    Result<FindSummary> summary{FindSummary{}};
    std::vector<FileMatches> files; // Every batch's files, in delivery order.
    std::size_t batches = 0;

    // The match count of `path`'s entry, or 0.
    [[nodiscard]] std::size_t matchesIn(std::string_view fileName) const {
        for (const FileMatches& file : files) {
            if (io::fileName(file.path) == fileName) {
                return file.matches.size();
            }
        }
        return 0;
    }
    [[nodiscard]] const FileMatches* file(std::string_view fileName) const {
        for (const FileMatches& entry : files) {
            if (io::fileName(entry.path) == fileName) {
                return &entry;
            }
        }
        return nullptr;
    }
};

// Runs `request` to completion on a two-worker pool. `onBatch` (optional) runs after each batch is
// recorded, on the engine's thread.
inline FindRun runFind(const document::LoadService& loader, FindRequest request, std::stop_source* stop = nullptr,
                       std::function<void(const FindRun&)> onBatch = {}) {
    sched::ThreadPoolOptions options;
    options.workerCount = 2;
    sched::ThreadPool pool(options);
    FindRun run;
    FindSink sink = [&run, &onBatch](FindBatch batch) {
        ++run.batches;
        for (FileMatches& file : batch.files) {
            run.files.push_back(std::move(file));
        }
        if (onBatch) {
            onBatch(run);
        }
    };
    run.summary = sched::syncWait(
        findInFiles(pool, loader, std::move(request), std::move(sink), stop ? stop->get_token() : std::stop_token{}));
    pool.shutdown();
    return run;
}

// A query over a directory target.
inline FindRequest directoryRequest(std::string directory, std::string pattern, text::SearchOptions options = {},
                                    std::string mask = {}) {
    FindRequest request;
    request.query.pattern = std::move(pattern);
    request.query.options = options;
    request.query.fileMask = std::move(mask);
    SearchScope scope;
    scope.kind = ScopeKind::Directory;
    scope.directory = std::move(directory);
    auto target = resolveScope(scope, nullptr, {});
    EXPECT_TRUE(target) << target.error().describe();
    request.target = target ? std::move(*target) : ScopeTarget{};
    return request;
}

} // namespace frost::search::test
