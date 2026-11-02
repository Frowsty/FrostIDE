// SPDX-License-Identifier: Apache-2.0
//
// scanTree (frost/io/tree_scan.hpp; ADR 0020): a coordinator coroutine keeps up to `parallelism`
// directory-listing tasks in flight on the scheduler, joins them in start order, and queues the
// subdirectories each one found together with the IgnoreStack in force there.

#include "frost/io/tree_scan.hpp"

#include "frost/io/path.hpp"

#include "internal.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace frost::io {
namespace {

struct DirJob {
    std::string path;     // Absolute, native syntax.
    std::string relative; // '/'-separated; "" for the root.
    IgnoreStack stack;    // The rules in force above this directory's own .gitignore.
};

struct DirOutcome {
    bool listed = false;
    std::vector<ScanEntry> entries;
    std::vector<DirJob> children;
    std::vector<ScanIssue> issues;
};

[[nodiscard]] std::string childPath(const std::string& directory, std::string_view name, char separator) {
    std::string path = directory;
    if (path.empty() || path.back() != separator) {
        path += separator;
    }
    path += name;
    return path;
}

// Lists one directory. Errors only for the root (its listing error) and for a stop (Cancelled).
Result<DirOutcome> listOne(IFileSystem& fs, const ScanOptions& options, const DirJob& job, char separator, bool isRoot,
                           const std::stop_token& token) {
    if (token.stop_requested()) {
        return fail(ErrorCode::Cancelled, "the scan was cancelled");
    }
    DirOutcome outcome;
    auto listing = fs.listDirectoryWithDiagnostics(job.path);
    if (!listing) {
        if (isRoot) {
            return std::unexpected(std::move(listing.error()));
        }
        outcome.issues.push_back(ScanIssue{ScanIssueKind::ListFailed, job.path, std::move(listing.error())});
        return outcome;
    }
    outcome.listed = true;
    for (DirListingIssue& issue : listing->issues) {
        outcome.issues.push_back(ScanIssue{ScanIssueKind::NameSkipped, job.path, std::move(issue.error)});
    }
    IgnoreStack stack = job.stack;
    // Reads one ignore file of this directory (a regular file only: git does not follow a symlinked
    // .gitignore either) and pushes it on top of `stack`.
    const auto readIgnoreFile = [&](std::string_view name) {
        const auto found = std::ranges::find(listing->entries, name, &DirEntry::name);
        if (found == listing->entries.end() || found->type != FileType::Regular) {
            return;
        }
        const std::string path = childPath(job.path, name, separator);
        auto text = fs.readFile(path);
        if (!text) {
            outcome.issues.push_back(ScanIssue{ScanIssueKind::IgnoreFileFailed, path, std::move(text.error())});
            return;
        }
        IgnoreRules rules = IgnoreRules::parse(*text, job.relative, path, options.ignoreOptions);
        for (const IgnoreDiagnostic& diagnostic : rules.diagnostics()) {
            outcome.issues.push_back(ScanIssue{
                ScanIssueKind::IgnoreRule, path,
                Error(ErrorCode::InvalidArgument, std::format("line {}: {}", diagnostic.line, diagnostic.message))});
        }
        stack = stack.pushed(std::move(rules));
    };
    if (options.readGitignore) {
        readIgnoreFile(".gitignore");
    }
    for (const std::string& name : options.extraIgnoreFiles) {
        readIgnoreFile(name);
    }
    for (const DirEntry& entry : listing->entries) {
        if (std::ranges::find(options.skipNames, entry.name) != options.skipNames.end() ||
            (options.skipFrostTempFiles && entry.name.find(detail::kAtomicTempMarker) != std::string::npos)) {
            continue;
        }
        std::string relative = job.relative.empty() ? entry.name : job.relative + '/' + entry.name;
        const bool isDirectory = entry.type == FileType::Directory;
        const bool ignored = !stack.empty() && stack.match(relative, isDirectory).ignored;
        if (ignored && !options.reportIgnored) {
            continue;
        }
        ScanEntry scanned{childPath(job.path, entry.name, separator), std::move(relative), entry.type, ignored, {}};
        scanned.status.type = entry.type;
        const bool descend =
            isDirectory && !ignored && options.recursive && (!options.descend || options.descend(scanned.path));
        if (options.withStatus && (entry.type == FileType::Regular || descend)) {
            auto status = fs.status(scanned.path);
            if (!status) {
                continue; // Gone (or unreadable) since the listing.
            }
            scanned.status = *status;
        }
        if (descend) {
            outcome.children.push_back(DirJob{scanned.path, scanned.relative, stack});
        }
        outcome.entries.push_back(std::move(scanned));
    }
    return outcome;
}

sched::Task<Result<DirOutcome>> listTask(IFileSystem& fs, const ScanOptions& options, DirJob job, char separator,
                                         bool isRoot, std::stop_token token) {
    co_return listOne(fs, options, job, separator, isRoot, token);
}

} // namespace

const IgnoreRules& defaultProjectExcludes() {
    // Directory-only patterns without a '/' inside, so they match at any depth (ignore_rules.hpp).
    // Built on first use and never destroyed (no exit-time destructor; startup hygiene, PLAN.md 2.1).
    static const IgnoreRules* const kRules = new IgnoreRules(IgnoreRules::parse("build/\n"
                                                                                "out/\n"
                                                                                "bin/\n"
                                                                                "obj/\n"
                                                                                "ipch/\n"
                                                                                "cmake-build-*/\n"
                                                                                "[Dd]ebug/\n"
                                                                                "[Rr]elease/\n"
                                                                                "x64/\n"
                                                                                "x86/\n"
                                                                                "ARM64/\n",
                                                                                {}, "<FrostIDE default excludes>"));
    return *kRules;
}

std::vector<std::string> defaultProjectSkipNames() { return {".git", ".vs", ".svn", ".hg"}; }

sched::Task<Result<TreeScan>> scanTree(IFileSystem& fs, sched::IScheduler& scheduler, std::string root,
                                       ScanOptions options, std::stop_token token) {
    if (options.parallelism == 0) {
        co_return fail(ErrorCode::InvalidArgument, "scanTree: parallelism must be at least 1");
    }
    // The file system's syntax: the native one, or the other for a MemFs of the other style.
    constexpr PathStyle kOther = kNativePathStyle == PathStyle::Windows ? PathStyle::Posix : PathStyle::Windows;
    PathStyle style = kNativePathStyle;
    if (!isAbsolutePath(root, style)) {
        style = kOther;
        if (!isAbsolutePath(root, style)) {
            co_return fail(ErrorCode::InvalidArgument, std::format("scanTree: '{}' is not an absolute path", root));
        }
    }
    const char separator = style == PathStyle::Windows ? '\\' : '/';

    TreeScan scan;
    std::deque<DirJob> queue;
    queue.push_back(DirJob{root, {}, options.ignore});
    std::deque<sched::JoinHandle<Result<DirOutcome>>> inflight;
    std::optional<Error> failure;
    bool isRoot = true;
    while (!queue.empty() || !inflight.empty()) {
        while (!failure && !queue.empty() && inflight.size() < options.parallelism) {
            inflight.push_back(scheduler.spawn(
                options.lane, listTask(fs, options, std::move(queue.front()), separator, isRoot, token)));
            queue.pop_front();
            isRoot = false;
        }
        if (inflight.empty()) {
            break; // After a failure: the queued directories are never listed.
        }
        // Every task in flight is joined, also after a failure: they reference `options`.
        Result<DirOutcome> outcome = co_await std::move(inflight.front());
        inflight.pop_front();
        if (failure) {
            continue;
        }
        if (!outcome) {
            failure = std::move(outcome.error());
            continue;
        }
        if (outcome->listed) {
            ++scan.directoriesListed;
        }
        std::ranges::move(outcome->entries, std::back_inserter(scan.entries));
        std::ranges::move(outcome->issues, std::back_inserter(scan.issues));
        std::ranges::move(outcome->children, std::back_inserter(queue));
        if (token.stop_requested()) {
            failure = Error(ErrorCode::Cancelled, "the scan was cancelled");
        }
    }
    if (failure) {
        co_return std::unexpected(std::move(*failure));
    }
    std::ranges::sort(scan.entries, {}, &ScanEntry::path);
    co_return scan;
}

} // namespace frost::io
