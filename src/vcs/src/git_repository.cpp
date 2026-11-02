// SPDX-License-Identifier: Apache-2.0
//
// GitRepository and repository discovery (frost/vcs/git_repository.hpp).

#include "frost/vcs/git_repository.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/vcs/vcs_status.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <map>
#include <set>

namespace frost::vcs {
namespace {

bool contains(std::string_view text, std::string_view needle) { return text.find(needle) != std::string_view::npos; }

// The lines of `text` without their terminators; empty lines are dropped.
std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
        start = end + 1;
    }
    return lines;
}

std::vector<std::string_view> splitNul(std::string_view text) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\0', start), text.size());
        fields.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return fields;
}

std::string joinArguments(const std::vector<std::string>& arguments) {
    std::string line = "git";
    for (const std::string& argument : arguments) {
        line += ' ';
        const bool quote = argument.empty() || argument.find(' ') != std::string::npos;
        if (quote) {
            line += '"';
        }
        line += argument;
        if (quote) {
            line += '"';
        }
    }
    return line;
}

// "a.txt, b.txt and 3 more".
std::string listFiles(const std::vector<std::string>& files) {
    std::string out;
    const std::size_t shown = std::min<std::size_t>(files.size(), 3);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i > 0) {
            out += i + 1 == shown && files.size() == shown ? " and " : ", ";
        }
        out += files[i];
    }
    if (files.size() > shown) {
        out += std::format(" and {} more", files.size() - shown);
    }
    return out;
}

} // namespace

std::vector<std::string> checkoutBlockers(std::string_view stderrText) {
    std::vector<std::string> files;
    bool inList = false;
    std::size_t start = 0;
    while (start < stderrText.size()) {
        std::size_t end = stderrText.find('\n', start);
        if (end == std::string_view::npos) {
            end = stderrText.size();
        }
        std::string_view line = stderrText.substr(start, end - start);
        start = end + 1;
        if (contains(line, "would be overwritten by")) {
            inList = true;
            continue;
        }
        if (inList && !line.empty() && line.front() == '\t') {
            const std::string_view file = trimGitText(line);
            if (!file.empty()) {
                files.emplace_back(file);
            }
            continue;
        }
        inList = false;
    }
    return files;
}

Result<std::optional<RepositoryInfo>> discoverRepository(const GitRunner& runner, std::string_view path,
                                                         std::stop_token stop) {
    auto normalized = io::normalizePath(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    std::string directory = *normalized;
    if (auto status = io::realFileSystem().status(directory); status && status->type != io::FileType::Directory) {
        directory = std::string(io::parentPath(directory));
    }
    GitInvocation invocation;
    invocation.stop = stop;
    invocation.arguments = {"rev-parse", "--show-toplevel", "--absolute-git-dir"};
    invocation.workingDirectory = directory;
    invocation.background = true;
    auto output = runner.run(invocation);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    if (!output->succeeded()) {
        if (contains(output->stderrBytes, "not a git repository") ||
            contains(output->stderrBytes, "must be run in a work tree")) {
            return std::optional<RepositoryInfo>();
        }
        return std::unexpected(gitFailure("git rev-parse", *output));
    }
    const std::vector<std::string_view> lines = splitLines(output->stdoutBytes);
    if (lines.size() < 2) {
        return fail(ErrorCode::IoError, std::format("git rev-parse printed '{}'", output->stdoutBytes));
    }
    auto workTree = io::normalizePath(lines[0]);
    auto gitDir = io::normalizePath(lines[1]);
    if (!workTree || !gitDir) {
        return fail(ErrorCode::IoError, std::format("git rev-parse printed paths FrostIDE cannot use: '{}'", lines[0]));
    }
    RepositoryInfo info{std::move(*workTree), std::move(*gitDir), {}};
    GitInvocation modules;
    modules.stop = stop;
    modules.arguments = {"config", "--file", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"};
    modules.workingDirectory = info.workTree;
    modules.background = true;
    if (auto listed = runner.run(modules); listed && listed->succeeded()) {
        for (const std::string_view line : splitLines(listed->stdoutBytes)) {
            const std::size_t space = line.find(' ');
            if (space == std::string_view::npos) {
                continue;
            }
            if (std::string submodule = absoluteGitPath(info.workTree, line.substr(space + 1)); !submodule.empty()) {
                info.submodules.push_back(std::move(submodule));
            }
        }
    }
    return std::optional<RepositoryInfo>(std::move(info));
}

GitRepository::GitRepository(const GitRunner& runner, io::IFileSystem& fs, RepositoryInfo info)
    : m_runner(&runner), m_fs(&fs), m_info(std::move(info)) {}

Result<std::string> GitRepository::relativePath(std::string_view absolutePath) const {
    auto relative = relativeGitPath(m_info.workTree, absolutePath);
    if (!relative) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("'{}' is not in the work tree {}", absolutePath, m_info.workTree));
    }
    return std::move(*relative);
}

std::string GitRepository::absolutePath(std::string_view relativePath) const {
    return absoluteGitPath(m_info.workTree, relativePath);
}

Result<GitOutput> GitRepository::git(std::vector<std::string> arguments, const RunControl& control,
                                     IClock::Duration timeout, bool background, bool requiresTrust,
                                     std::string_view indexFile) const {
    if (control.onLine) {
        control.onLine(GitStream::Command, joinArguments(arguments));
    }
    GitInvocation invocation;
    invocation.arguments = std::move(arguments);
    invocation.workingDirectory = m_info.workTree;
    invocation.timeout = timeout;
    invocation.stop = control.stop;
    invocation.background = background;
    invocation.requiresTrust = !background || requiresTrust;
    invocation.onLine = control.onLine;
    if (!indexFile.empty())
        invocation.environment.emplace_back("GIT_INDEX_FILE", std::string(indexFile));
    return m_runner->run(invocation);
}

Result<void> GitRepository::requireTrust(std::string_view what) const {
    if (!m_runner->trusted()) {
        return fail(ErrorCode::Untrusted,
                    std::format("{} can run the repository's hooks; trust the folder {} first", what, m_info.workTree));
    }
    return {};
}

Result<GitStatus> GitRepository::status(std::stop_token stop) const {
    if (!m_runner->trusted())
        return restrictedStatus(stop);
    // Normal porcelain status may run configured clean/process filters; recheck trust at launch.
    auto output = git(statusArguments(), RunControl{stop, {}}, kLocalTimeout, true, true);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    if (!output->succeeded()) {
        return std::unexpected(gitFailure("git status", *output));
    }
    return parsePorcelainV2(output->stdoutBytes);
}

// Restricted status does not call git status: it may run repository clean/process filters. Only
// built-in index/tree enumeration and hash-object --no-filters run. Raw-byte comparison is
// conservative for filtered, symlinked and submodule files; the UI identifies this mode.
Result<GitStatus> GitRepository::restrictedStatus(std::stop_token stop) const {
    struct Entry {
        std::string headMode, headHash, indexMode, indexHash;
        bool conflict = false;
    };
    std::map<std::string, Entry> entries;
    GitStatus result;
    result.restrictedRawComparison = true;
    const auto run = [&](std::vector<std::string> args) -> Result<GitOutput> {
        auto output = git(std::move(args), RunControl{stop, {}}, kLocalTimeout, true);
        if (!output)
            return std::unexpected(std::move(output.error()));
        if (!output->succeeded())
            return std::unexpected(gitFailure("restricted git read", *output));
        return output;
    };
    auto branch = git({"symbolic-ref", "--quiet", "--short", "HEAD"}, RunControl{stop, {}}, kLocalTimeout, true);
    if (!branch)
        return std::unexpected(std::move(branch.error()));
    if (branch->succeeded())
        result.branch.head = std::string(trimGitText(branch->stdoutBytes));
    auto head = git({"rev-parse", "--verify", "HEAD"}, RunControl{stop, {}}, kLocalTimeout, true);
    if (!head)
        return std::unexpected(std::move(head.error()));
    if (head->succeeded()) {
        result.branch.oid = std::string(trimGitText(head->stdoutBytes));
        auto tree = run({"ls-tree", "-r", "-z", "HEAD"});
        if (!tree)
            return std::unexpected(std::move(tree.error()));
        for (auto record : splitNul(tree->stdoutBytes)) {
            if (record.empty())
                continue;
            const auto tab = record.find('\t');
            const auto a = record.find(' ');
            const auto b = a == std::string_view::npos ? a : record.find(' ', a + 1);
            if (tab == std::string_view::npos || a == std::string_view::npos || b == std::string_view::npos || b >= tab)
                return fail(ErrorCode::Corrupt, "Malformed Git tree entry");
            auto& item = entries[std::string(record.substr(tab + 1))];
            item.headMode = record.substr(0, a);
            item.headHash = record.substr(b + 1, tab - b - 1);
        }
    }
    auto index = run({"ls-files", "--stage", "-z"});
    if (!index)
        return std::unexpected(std::move(index.error()));
    for (auto record : splitNul(index->stdoutBytes)) {
        if (record.empty())
            continue;
        const auto tab = record.find('\t');
        const auto a = record.find(' ');
        const auto b = a == std::string_view::npos ? a : record.find(' ', a + 1);
        if (tab == std::string_view::npos || a == std::string_view::npos || b == std::string_view::npos || b >= tab)
            return fail(ErrorCode::Corrupt, "Malformed Git index entry");
        auto& item = entries[std::string(record.substr(tab + 1))];
        if (record.substr(b + 1, tab - b - 1) != "0")
            item.conflict = true;
        else {
            item.indexMode = record.substr(0, a);
            item.indexHash = record.substr(a + 1, b - a - 1);
        }
    }
    // Resolve local tracking refs without fetching or executing repository filters.
    auto upstream = git({"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"}, RunControl{stop, {}},
                        kLocalTimeout, true);
    if (!upstream)
        return std::unexpected(std::move(upstream.error()));
    if (upstream->succeeded()) {
        result.branch.upstream = std::string(trimGitText(upstream->stdoutBytes));
        auto counts = git({"rev-list", "--left-right", "--count", "HEAD...@{upstream}"}, RunControl{stop, {}},
                          kLocalTimeout, true);
        if (!counts)
            return std::unexpected(std::move(counts.error()));
        if (counts->succeeded()) {
            auto line = trimGitText(counts->stdoutBytes);
            auto separator = line.find('\t');
            if (separator != std::string_view::npos) {
                std::from_chars(line.data(), line.data() + separator, result.branch.ahead);
                std::from_chars(line.data() + separator + 1, line.data() + line.size(), result.branch.behind);
            }
        }
    }
    std::vector<std::string> hashPaths;
    std::map<std::string, char> worktree;
    for (const auto& [path, item] : entries) {
        if (item.indexHash.empty() || item.conflict)
            continue;
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Git status cancelled");
        const auto file = m_fs->status(absolutePath(path));
        if (!file) {
            worktree[path] = file.error().code() == ErrorCode::NotFound ? 'D' : 'M';
            continue;
        }
        // Git hashes link targets and submodule commits differently from regular file bytes.
        if (item.indexMode == "120000" || item.indexMode == "160000" || file->type != io::FileType::Regular) {
            worktree[path] = 'M';
            continue;
        }
        hashPaths.push_back(path);
    }
    for (std::size_t first = 0; first < hashPaths.size();) {
        std::vector<std::string> args{"hash-object", "--no-filters", "--"};
        std::size_t last = first, bytes = 0;
        while (last < hashPaths.size() && last - first < 64 &&
               (last == first || bytes + hashPaths[last].size() < 6000)) {
            bytes += hashPaths[last].size();
            args.push_back(hashPaths[last++]);
        }
        auto hashes = git(std::move(args), RunControl{stop, {}}, kLocalTimeout, true);
        if (!hashes)
            return std::unexpected(std::move(hashes.error()));
        const auto lines = splitLines(hashes->stdoutBytes);
        for (std::size_t n = first; n < last; ++n) {
            const auto line = n - first;
            worktree[hashPaths[n]] =
                hashes->succeeded() && line < lines.size() && lines[line] == entries[hashPaths[n]].indexHash ? '.'
                                                                                                             : 'M';
        }
        first = last;
    }
    for (const auto& [path, item] : entries) {
        GitStatusEntry entry;
        entry.path = path;
        if (item.conflict)
            entry.kind = GitEntryKind::Unmerged;
        else {
            if (item.headHash.empty() && !item.indexHash.empty())
                entry.index = 'A';
            else if (!item.headHash.empty() && item.indexHash.empty())
                entry.index = 'D';
            else if (item.headHash != item.indexHash || item.headMode != item.indexMode)
                entry.index = 'M';
            if (auto found = worktree.find(path); found != worktree.end())
                entry.worktree = found->second;
            entry.submodule = item.indexMode == "160000";
        }
        if (entry.kind == GitEntryKind::Unmerged || entry.index != '.' || entry.worktree != '.')
            result.entries.push_back(std::move(entry));
    }
    auto untracked = run({"ls-files", "--others", "--exclude-standard", "-z"});
    if (!untracked)
        return std::unexpected(std::move(untracked.error()));
    for (auto path : splitNul(untracked->stdoutBytes)) {
        if (path.empty())
            continue;
        GitStatusEntry entry;
        entry.kind = GitEntryKind::Untracked;
        entry.path = path;
        result.entries.push_back(std::move(entry));
    }
    return result;
}

Result<std::optional<std::string>> GitRepository::fileContent(std::string_view absolutePath, std::string_view rev,
                                                              std::stop_token stop) const {
    auto relative = relativePath(absolutePath);
    if (!relative) {
        return std::unexpected(std::move(relative.error()));
    }
    const std::string object = std::format("{}:{}", rev, *relative);
    const bool filtered = m_runner->trusted();
    std::vector<std::string> arguments = filtered ? std::vector<std::string>{"cat-file", "--filters", object}
                                                  : std::vector<std::string>{"cat-file", "blob", object};
    auto output = git(std::move(arguments), RunControl{stop, {}}, kLocalTimeout, true, filtered);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    if (output->succeeded()) {
        return std::optional<std::string>(std::move(output->stdoutBytes));
    }
    // A missing file (or no commit yet) is not an error: tell it apart from a real failure.
    auto exists = git({"rev-parse", "-q", "--verify", object}, RunControl{stop, {}}, kLocalTimeout, true);
    if (!exists)
        return std::unexpected(std::move(exists.error()));
    if (!exists->succeeded()) {
        return std::optional<std::string>();
    }
    return std::unexpected(gitFailure("git cat-file", *output));
}

Result<std::optional<std::string>> GitRepository::headContent(std::string_view absolutePath,
                                                              std::stop_token stop) const {
    return fileContent(absolutePath, "HEAD", stop);
}

Result<std::optional<std::string>> GitRepository::indexContent(std::string_view absolutePath,
                                                               std::stop_token stop) const {
    return fileContent(absolutePath, "", stop);
}

Result<std::string> GitRepository::lastCommitMessage(std::stop_token stop) const {
    auto head = git({"rev-parse", "-q", "--verify", "HEAD"}, RunControl{stop, {}}, kLocalTimeout, true);
    if (!head) {
        return std::unexpected(std::move(head.error()));
    }
    if (!head->succeeded()) {
        return std::string();
    }
    auto output = git({"log", "-1", "--format=%B"}, RunControl{stop, {}}, kLocalTimeout, true);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    if (!output->succeeded()) {
        return std::unexpected(gitFailure("git log", *output));
    }
    return std::string(trimGitText(output->stdoutBytes));
}

Result<std::vector<std::string>> GitRepository::recentCommitMessages(std::size_t count, std::stop_token stop) const {
    std::vector<std::string> messages;
    auto head = git({"rev-parse", "-q", "--verify", "HEAD"}, RunControl{stop, {}}, kLocalTimeout, true);
    if (!head) {
        return std::unexpected(std::move(head.error()));
    }
    if (!head->succeeded() || count == 0) {
        return messages;
    }
    auto output =
        git({"log", std::format("-n{}", count), "--format=%B%x00"}, RunControl{stop, {}}, kLocalTimeout, true);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    if (!output->succeeded()) {
        return std::unexpected(gitFailure("git log", *output));
    }
    for (const std::string_view message : splitNul(output->stdoutBytes)) {
        const std::string_view trimmed = trimGitText(message);
        if (!trimmed.empty()) {
            messages.emplace_back(trimmed);
        }
    }
    return messages;
}

Result<CommitResult> GitRepository::commit(const CommitRequest& request, const RunControl& control) const {
    if (auto trusted = requireTrust("Commit"); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    if (trimGitText(request.message).empty()) {
        return fail(ErrorCode::InvalidArgument, "the commit message is empty");
    }
    if (request.paths.empty() && !request.amend) {
        return fail(ErrorCode::InvalidArgument, "no files are selected for the commit");
    }
    // Reserve the real Git index lock for the complete operation. A private index inside this
    // directory means a killed hook cannot leave a real index lock or partially stage files.
    const auto index = io::joinPath(m_info.gitDir, "index");
    const auto lock = io::joinPath(m_info.gitDir, "index.lock");
    if (!index || !lock)
        return fail(ErrorCode::InvalidArgument, "Invalid Git index path");
#ifdef _WIN32
    auto wideLock = io::toWin32Path(*lock);
    if (!wideLock)
        return std::unexpected(std::move(wideLock.error()));
    const auto nativeLock = std::filesystem::path(*wideLock);
#else
    const auto nativeLock = std::filesystem::path(*lock);
#endif
    std::error_code lockError;
    if (!std::filesystem::create_directory(nativeLock, lockError))
        return fail(ErrorCode::Busy, std::format("Cannot reserve Git index lock {}: {}", *lock,
                                                 lockError ? lockError.message() : "another Git operation is active"));
    const auto privateIndex = io::joinPath(*lock, "frost-index");
    if (!privateIndex) {
        std::filesystem::remove(nativeLock, lockError);
        return fail(ErrorCode::InvalidArgument, "Invalid private Git index path");
    }
    struct IndexReservation {
        io::IFileSystem* fs;
        std::string index;
        std::filesystem::path lock;
        bool preserve = false;
        ~IndexReservation() {
            if (preserve)
                return;
            static_cast<void>(fs->removeFile(index));
            static_cast<void>(fs->removeFile(index + ".lock"));
            std::error_code ignored;
            std::filesystem::remove(lock, ignored); // Never recursively remove another process's files.
        }
    } reservation{m_fs, *privateIndex, nativeLock};
    const auto nativeIndex = io::toStdPath(*index);
    const auto nativePrivateIndex = io::toStdPath(*privateIndex);
    if (!nativeIndex || !nativePrivateIndex)
        return fail(ErrorCode::InvalidArgument, "Invalid Git index path");
    const auto earlier = [](std::filesystem::file_time_type timestamp) {
        const auto margin =
            std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::seconds(2));
        return timestamp < std::filesystem::file_time_type::min() + margin ? timestamp : timestamp - margin;
    };
    std::optional<std::filesystem::file_time_type> conservativeTime;
    const auto makeRacy = [&](const std::filesystem::path& path) -> Result<void> {
        if (!conservativeTime)
            return {};
        std::error_code error;
        std::filesystem::last_write_time(path, *conservativeTime, error);
        if (error)
            return fail(ErrorCode::IoError, std::format("Cannot preserve Git index timestamp: {}", error.message()));
        return {};
    };
    auto existing = m_fs->readFile(*index);
    if (!existing && existing.error().code() != ErrorCode::NotFound)
        return std::unexpected(std::move(existing.error()).withContext("reading the Git index"));
    if (existing) {
        std::error_code error;
        const auto timestamp = std::filesystem::last_write_time(*nativeIndex, error);
        if (error)
            return fail(ErrorCode::IoError, std::format("Cannot read Git index timestamp: {}", error.message()));
        conservativeTime = earlier(timestamp);
        if (auto copied = m_fs->writeFileAtomic(*privateIndex, *existing); !copied)
            return std::unexpected(std::move(copied.error()).withContext("copying the Git index"));
        // Copying an index advances its mtime and can make Git trust stale same-size stat entries.
        if (auto aged = makeRacy(*nativePrivateIndex); !aged)
            return std::unexpected(std::move(aged.error()));
    }
    auto originalHead = git({"rev-parse", "--verify", "HEAD"}, control, kLocalTimeout, true);
    if (!originalHead)
        return std::unexpected(std::move(originalHead.error()));
    const std::string oldHead = originalHead->succeeded() ? std::string(trimGitText(originalHead->stdoutBytes)) : "";
    auto current = status(control.stop);
    if (!current) {
        return std::unexpected(std::move(current.error()));
    }
    std::vector<std::string> paths;
    std::vector<std::string> untracked;
    std::set<std::string> seen;
    const auto addPath = [&](std::string path) {
        if (seen.insert(path).second) {
            paths.push_back(std::move(path));
        }
    };
    for (const std::string& absolute : request.paths) {
        auto relative = relativePath(absolute);
        if (!relative) {
            return std::unexpected(std::move(relative.error()));
        }
        const GitStatusEntry* entry = current->find(*relative);
        if (entry != nullptr && entry->kind == GitEntryKind::Untracked) {
            untracked.push_back(*relative);
        }
        if (entry != nullptr && entry->kind == GitEntryKind::RenamedOrCopied) {
            addPath(entry->path);
            addPath(entry->originalPath);
        } else {
            addPath(std::move(*relative));
        }
    }
    if (!untracked.empty()) {
        std::vector<std::string> add = {"add", "--"};
        add.insert(add.end(), untracked.begin(), untracked.end());
        auto added = git(std::move(add), control, kLocalTimeout, false, true, *privateIndex);
        if (!added)
            return std::unexpected(std::move(added.error()));
        if (!added->succeeded())
            return std::unexpected(gitFailure("git add", *added));
        // git add may rewrite the private index and advance its timestamp again.
        if (auto aged = makeRacy(*nativePrivateIndex); !aged)
            return std::unexpected(std::move(aged.error()));
    }
    auto messageFile = io::joinPath(*lock, "commit-message");
    if (!messageFile)
        return fail(ErrorCode::InvalidArgument, "Invalid commit message path");
    std::string message(trimGitText(request.message));
    message += '\n';
    if (auto written = m_fs->writeFileAtomic(*messageFile, message); !written)
        return std::unexpected(std::move(written.error()).withContext("writing the commit message"));
    std::vector<std::string> arguments = {"commit", "-F", *messageFile};
    if (request.amend)
        arguments.emplace_back("--amend");
    arguments.emplace_back("--only");
    if (!paths.empty()) {
        arguments.emplace_back("--");
        arguments.insert(arguments.end(), paths.begin(), paths.end());
    }
    auto committed = git(std::move(arguments), control, kHookTimeout, false, true, *privateIndex);
    static_cast<void>(m_fs->removeFile(*messageFile));
    // A post-commit hook may be cancelled after Git moved HEAD. Inspect it with a fresh bounded
    // read while our index reservation is still held; never discard the only resulting index.
    auto head = git({"rev-parse", "--verify", "HEAD"}, RunControl{}, std::chrono::seconds(2), true);
    if (!head || (!head->succeeded() && !oldHead.empty())) {
        reservation.preserve = true;
        return fail(ErrorCode::IoError,
                    std::format("Could not determine whether the commit completed. Recovery index retained at {}",
                                *privateIndex));
    }
    const std::string newHead = head->succeeded() ? std::string(trimGitText(head->stdoutBytes)) : "";
    const bool headChanged = newHead != oldHead;
    if (!committed && !headChanged)
        return std::unexpected(std::move(committed.error()));
    if (committed && !committed->succeeded() && !headChanged)
        return std::unexpected(gitFailure("git commit", *committed));
    if (headChanged && (!committed || !committed->succeeded())) {
        // An independent update-ref can move HEAD while this Git process is in a hook. A failed
        // or interrupted command does not identify which process moved it; never publish the
        // private index or claim that this operation created the observed commit.
        reservation.preserve = true;
        return fail(ErrorCode::IoError,
                    std::format("HEAD changed while Git was interrupted or failed; commit provenance and index "
                                "recovery are uncertain. The real index was not changed. Recovery files are "
                                "retained under {} (private index {}, pending index {}.lock if present)",
                                *lock, *privateIndex, *privateIndex));
    }
    // Git can move HEAD and then block in the reference-transaction committed hook before
    // renaming its private index lock. In that state the old index is not a valid result.
    const std::string pendingIndex = *privateIndex + ".lock";
    auto pending = m_fs->status(pendingIndex);
    if (pending) {
        reservation.preserve = true;
        return fail(ErrorCode::IoError,
                    std::format("Commit {} exists, but its index recovery is incomplete. Both index files are "
                                "retained at {} and {}; inspect them before recovering the index",
                                newHead, *privateIndex, pendingIndex));
    }
    if (pending.error().code() != ErrorCode::NotFound) {
        reservation.preserve = true;
        return fail(pending.error().code(),
                    std::format("Commit {} exists, but index recovery is incomplete: cannot inspect {}. "
                                "Recovery files are retained under {}: {}",
                                newHead, pendingIndex, *lock, pending.error().message()));
    }
    auto updated = m_fs->readFile(*privateIndex);
    if (!updated) {
        reservation.preserve = true;
        return fail(updated.error().code(),
                    std::format("Commit {} exists, but reading its recovery index failed; retained at {}: {}", newHead,
                                *privateIndex, updated.error().message()));
    }
    if (!conservativeTime) {
        std::error_code error;
        const auto timestamp = std::filesystem::last_write_time(*nativePrivateIndex, error);
        if (error) {
            reservation.preserve = true;
            return fail(
                ErrorCode::IoError,
                std::format("Commit {} exists, but its index timestamp cannot be read; recovery retained at {}: {}",
                            newHead, *privateIndex, error.message()));
        }
        conservativeTime = earlier(timestamp);
    }
    if (auto written = m_fs->writeFileAtomicWithMtime(*index, *updated, *conservativeTime); !written) {
        reservation.preserve = true;
        return fail(written.error().code(),
                    std::format("Commit {} exists, but updating the index failed; recovery index retained at {}: {}",
                                newHead, *privateIndex, written.error().message()));
    }
    CommitResult result;
    result.output = committed->stdoutBytes + committed->stderrBytes;
    result.commit = newHead;
    return result;
}

Result<void> GitRepository::rollback(std::span<const std::string> absolutePaths, const RunControl& control) const {
    if (auto trusted = requireTrust("Rollback"); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    auto current = status(control.stop);
    if (!current) {
        return std::unexpected(std::move(current.error()));
    }
    std::set<std::string> restore;
    std::set<std::string> unstage;
    std::set<std::string> remove;
    for (const std::string& absolute : absolutePaths) {
        auto relative = relativePath(absolute);
        if (!relative) {
            return std::unexpected(std::move(relative.error()));
        }
        const GitStatusEntry* entry = current->find(*relative);
        if (entry == nullptr || entry->kind == GitEntryKind::Untracked || entry->kind == GitEntryKind::Ignored) {
            continue; // Unchanged, or unversioned: nothing to roll back.
        }
        if (entry->kind == GitEntryKind::RenamedOrCopied) {
            if (entry->index == 'R') {
                restore.insert(entry->originalPath);
                remove.insert(entry->path);
            } else {
                unstage.insert(entry->path); // A copy is a new file.
            }
        } else if (entry->kind == GitEntryKind::Ordinary && entry->index == 'A') {
            unstage.insert(entry->path);
        } else {
            restore.insert(entry->path);
        }
    }
    const auto runOn = [&](std::vector<std::string> arguments, const std::set<std::string>& files,
                           std::string_view what) -> Result<void> {
        if (files.empty()) {
            return {};
        }
        arguments.emplace_back("--");
        arguments.insert(arguments.end(), files.begin(), files.end());
        auto output = git(std::move(arguments), control, kLocalTimeout);
        if (!output) {
            return std::unexpected(std::move(output.error()));
        }
        if (!output->succeeded()) {
            return std::unexpected(gitFailure(what, *output));
        }
        return {};
    };
    if (auto done = runOn({"rm", "-q", "-f"}, remove, "git rm"); !done) {
        return done;
    }
    if (auto done = runOn({"rm", "-q", "--cached"}, unstage, "git rm --cached"); !done) {
        return done;
    }
    return runOn({"restore", "--source=HEAD", "--staged", "--worktree"}, restore, "git restore");
}

Result<std::vector<GitBranch>> GitRepository::branches(std::stop_token stop) const {
    auto output = git({"for-each-ref",
                       "--format=%(HEAD)%00%(refname)%00%(refname:short)%00%(upstream:short)%00%(objectname:short)",
                       "refs/heads", "refs/remotes"},
                      RunControl{stop, {}}, kLocalTimeout, true);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    if (!output->succeeded()) {
        return std::unexpected(gitFailure("git for-each-ref", *output));
    }
    std::vector<GitBranch> local;
    std::vector<GitBranch> remote;
    for (const std::string_view line : splitLines(output->stdoutBytes)) {
        const std::vector<std::string_view> fields = splitNul(line);
        if (fields.size() < 5) {
            continue;
        }
        GitBranch branch;
        branch.current = fields[0] == "*";
        branch.fullName = std::string(fields[1]);
        branch.name = std::string(fields[2]);
        branch.upstream = std::string(fields[3]);
        branch.commit = std::string(fields[4]);
        branch.remote = branch.fullName.starts_with("refs/remotes/");
        if (branch.remote && branch.fullName.ends_with("/HEAD")) {
            continue; // origin/HEAD names the remote's default branch, not a branch.
        }
        (branch.remote ? remote : local).push_back(std::move(branch));
    }
    std::ranges::sort(local, {}, &GitBranch::name);
    std::ranges::sort(remote, {}, &GitBranch::name);
    local.insert(local.end(), std::make_move_iterator(remote.begin()), std::make_move_iterator(remote.end()));
    return local;
}

Result<void> GitRepository::checkout(std::string_view branch, const RunControl& control) const {
    if (auto trusted = requireTrust("Checkout"); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    auto all = branches();
    if (!all) {
        return std::unexpected(std::move(all.error()));
    }
    std::vector<std::string> arguments = {"switch", std::string(branch)};
    const auto found = std::ranges::find(*all, branch, &GitBranch::name);
    if (found != all->end() && found->remote) {
        // origin/feature -> the local branch feature, created to track it unless it exists.
        const std::size_t slash = found->name.find('/');
        const std::string localName = slash == std::string::npos ? found->name : found->name.substr(slash + 1);
        const bool exists =
            std::ranges::any_of(*all, [&](const GitBranch& b) { return !b.remote && b.name == localName; });
        arguments = exists ? std::vector<std::string>{"switch", localName}
                           : std::vector<std::string>{"switch", "-c", localName, "--track", found->name};
    }
    auto output = git(std::move(arguments), control, kHookTimeout);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    if (output->succeeded()) {
        return {};
    }
    if (const std::vector<std::string> blockers = checkoutBlockers(output->stderrBytes); !blockers.empty()) {
        return fail(ErrorCode::Busy,
                    std::format("Checkout of '{}' was refused: it would overwrite the local changes to {}. Commit, "
                                "stash or roll back those changes first.",
                                branch, listFiles(blockers)));
    }
    return std::unexpected(gitFailure(std::format("Checkout of '{}'", branch), *output));
}

Result<void> GitRepository::createBranch(std::string_view name, const RunControl& control) const {
    if (auto trusted = requireTrust("Creating a branch"); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    auto valid = git({"check-ref-format", "--branch", std::string(name)}, {}, kLocalTimeout, true);
    if (!valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (!valid->succeeded() || name.empty() || name.front() == '-') {
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a valid branch name", name));
    }
    auto all = branches();
    if (!all) {
        return std::unexpected(std::move(all.error()));
    }
    if (std::ranges::any_of(*all, [&](const GitBranch& b) { return !b.remote && b.name == name; })) {
        return fail(ErrorCode::AlreadyExists, std::format("a branch named '{}' already exists", name));
    }
    auto output = git({"switch", "-c", std::string(name)}, control, kHookTimeout);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    if (!output->succeeded()) {
        return std::unexpected(gitFailure(std::format("Creating the branch '{}'", name), *output));
    }
    return {};
}

Result<GitOutput> GitRepository::fetch(const RunControl& control) const {
    if (auto trusted = requireTrust("Fetch"); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    auto output = git({"fetch", "--all", "--prune"}, control, kRemoteTimeout);
    if (output && !output->succeeded()) {
        return std::unexpected(gitFailure("Fetch", *output));
    }
    return output;
}

Result<GitOutput> GitRepository::pull(PullStrategy strategy, const RunControl& control) const {
    if (auto trusted = requireTrust("Pull"); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    auto output = git({"pull", "--no-edit", strategy == PullStrategy::Rebase ? "--rebase" : "--no-rebase"}, control,
                      kRemoteTimeout);
    if (output && !output->succeeded()) {
        return std::unexpected(gitFailure("Pull", *output));
    }
    return output;
}

Result<GitOutput> GitRepository::push(const PushOptions& options, const RunControl& control) const {
    if (auto trusted = requireTrust("Push"); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    if (options.forceWithLease && !options.confirmed) {
        return fail(ErrorCode::InvalidArgument, "a force push needs the user's confirmation");
    }
    auto current = status(control.stop);
    if (!current) {
        return std::unexpected(std::move(current.error()));
    }
    const std::string& head = current->branch.head;
    if (head.empty()) {
        return fail(ErrorCode::InvalidArgument, "HEAD is detached: check a branch out before pushing");
    }
    std::vector<std::string> arguments = {"push"};
    if (options.forceWithLease) {
        arguments.emplace_back("--force-with-lease");
    }
    const auto config = [&](std::string key) -> std::string {
        auto value = git({"config", "--get", std::move(key)}, {}, kLocalTimeout, true);
        return value && value->succeeded() ? std::string(trimGitText(value->stdoutBytes)) : std::string();
    };
    const std::string remote = config(std::format("branch.{}.remote", head));
    const std::string merge = config(std::format("branch.{}.merge", head));
    if (!remote.empty() && !merge.empty()) {
        arguments.push_back(remote);
        arguments.push_back(std::format("HEAD:{}", merge));
    } else {
        auto remotes = git({"remote"}, {}, kLocalTimeout, true);
        if (!remotes) {
            return std::unexpected(std::move(remotes.error()));
        }
        const std::vector<std::string_view> names = splitLines(remotes->stdoutBytes);
        std::string target;
        if (std::ranges::find(names, std::string_view("origin")) != names.end()) {
            target = "origin";
        } else if (names.size() == 1) {
            target = std::string(names.front());
        } else {
            return fail(ErrorCode::InvalidArgument,
                        names.empty() ? std::string("the repository has no remote to push to")
                                      : std::string("the branch has no upstream and there is no 'origin' remote"));
        }
        arguments.emplace_back("--set-upstream");
        arguments.push_back(std::move(target));
        arguments.push_back(head);
    }
    auto output = git(std::move(arguments), control, kRemoteTimeout);
    if (output && !output->succeeded()) {
        return std::unexpected(gitFailure("Push", *output));
    }
    return output;
}

} // namespace frost::vcs
