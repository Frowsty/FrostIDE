// SPDX-License-Identifier: Apache-2.0
//
// The porcelain v2 parser and the file-status mapping (frost/vcs/git_status.hpp).

#include "frost/vcs/git_status.hpp"

#include <charconv>
#include <format>

namespace frost::vcs {
namespace {

// Splits `record` at its first `count` spaces; the remainder (a path, which may hold spaces) is
// the last field. False when the record has fewer fields.
bool splitFields(std::string_view record, std::size_t count, std::vector<std::string_view>& fields) {
    fields.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t space = record.find(' ');
        if (space == std::string_view::npos) {
            return false;
        }
        fields.push_back(record.substr(0, space));
        record.remove_prefix(space + 1);
    }
    fields.push_back(record);
    return !record.empty();
}

bool parseCount(std::string_view text, int& value) {
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        text.remove_prefix(1);
    }
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc() && end == text.data() + text.size();
}

std::unexpected<Error> corrupt(std::string_view record) {
    return fail(ErrorCode::Corrupt, std::format("unexpected git status record '{}'", record.substr(0, 80)));
}

} // namespace

std::string_view toString(VcsFileStatus status) noexcept {
    switch (status) {
    case VcsFileStatus::Unmodified: return "unmodified";
    case VcsFileStatus::Added: return "added";
    case VcsFileStatus::Modified: return "modified";
    case VcsFileStatus::Deleted: return "deleted";
    case VcsFileStatus::Renamed: return "renamed";
    case VcsFileStatus::Untracked: return "untracked";
    case VcsFileStatus::Conflicted: return "conflicted";
    case VcsFileStatus::Ignored: return "ignored";
    }
    return "unmodified";
}

VcsFileStatus GitStatusEntry::fileStatus() const noexcept {
    switch (kind) {
    case GitEntryKind::Unmerged: return VcsFileStatus::Conflicted;
    case GitEntryKind::Untracked: return VcsFileStatus::Untracked;
    case GitEntryKind::Ignored: return VcsFileStatus::Ignored;
    case GitEntryKind::RenamedOrCopied: return index == 'C' ? VcsFileStatus::Added : VcsFileStatus::Renamed;
    case GitEntryKind::Ordinary: break;
    }
    if (index == 'A') {
        return worktree == 'D' ? VcsFileStatus::Deleted : VcsFileStatus::Added;
    }
    if (index == 'D' || worktree == 'D') {
        return VcsFileStatus::Deleted;
    }
    if (index == '.' && worktree == '.') {
        return VcsFileStatus::Unmodified;
    }
    return VcsFileStatus::Modified;
}

bool GitStatusEntry::staged() const noexcept {
    return (kind == GitEntryKind::Ordinary || kind == GitEntryKind::RenamedOrCopied ||
            kind == GitEntryKind::Unmerged) &&
           index != '.';
}

bool GitStatusEntry::unstaged() const noexcept {
    if (kind == GitEntryKind::Untracked) {
        return true;
    }
    return kind != GitEntryKind::Ignored && worktree != '.';
}

const GitStatusEntry* GitStatus::find(std::string_view relativePath) const noexcept {
    for (const GitStatusEntry& entry : entries) {
        if (entry.path == relativePath || (!entry.originalPath.empty() && entry.originalPath == relativePath)) {
            return &entry;
        }
    }
    return nullptr;
}

Result<GitStatus> parsePorcelainV2(std::string_view output) {
    GitStatus status;
    std::vector<std::string_view> fields;
    while (!output.empty()) {
        const std::size_t end = output.find('\0');
        const std::string_view record = output.substr(0, end);
        output = end == std::string_view::npos ? std::string_view() : output.substr(end + 1);
        if (record.empty()) {
            continue;
        }
        switch (record.front()) {
        case '#': {
            if (record.starts_with("# branch.oid ")) {
                const std::string_view oid = record.substr(13);
                status.branch.oid = oid == "(initial)" ? std::string() : std::string(oid);
            } else if (record.starts_with("# branch.head ")) {
                const std::string_view head = record.substr(14);
                status.branch.head = head == "(detached)" ? std::string() : std::string(head);
            } else if (record.starts_with("# branch.upstream ")) {
                status.branch.upstream = std::string(record.substr(18));
            } else if (record.starts_with("# branch.ab ")) {
                const std::string_view ab = record.substr(12);
                const std::size_t space = ab.find(' ');
                if (space == std::string_view::npos || !parseCount(ab.substr(0, space), status.branch.ahead) ||
                    !parseCount(ab.substr(space + 1), status.branch.behind)) {
                    return corrupt(record);
                }
            }
            break;
        }
        case '1':
        case '2':
        case 'u': {
            const char type = record.front();
            // The fields after the type and before the path: XY sub mH mI mW hH hI (1), plus Xscore
            // (2); XY sub m1 m2 m3 mW h1 h2 h3 (u).
            const std::size_t count = type == '1' ? 7 : type == '2' ? 8 : 9;
            if (record.size() < 2 || record[1] != ' ' || !splitFields(record.substr(2), count, fields) ||
                fields[0].size() != 2) {
                return corrupt(record);
            }
            GitStatusEntry entry;
            entry.kind = type == '1'   ? GitEntryKind::Ordinary
                         : type == '2' ? GitEntryKind::RenamedOrCopied
                                       : GitEntryKind::Unmerged;
            entry.index = fields[0][0];
            entry.worktree = fields[0][1];
            entry.submodule = !fields[1].empty() && fields[1].front() == 'S';
            entry.path = std::string(fields.back());
            if (type == '2') {
                // -z puts the source path in the next record.
                const std::size_t origEnd = output.find('\0');
                const std::string_view orig = output.substr(0, origEnd);
                if (orig.empty()) {
                    return corrupt(record);
                }
                entry.originalPath = std::string(orig);
                output = origEnd == std::string_view::npos ? std::string_view() : output.substr(origEnd + 1);
            }
            status.entries.push_back(std::move(entry));
            break;
        }
        case '?':
        case '!': {
            if (record.size() < 3 || record[1] != ' ') {
                return corrupt(record);
            }
            GitStatusEntry entry;
            entry.kind = record.front() == '?' ? GitEntryKind::Untracked : GitEntryKind::Ignored;
            entry.index = record.front();
            entry.worktree = record.front();
            entry.path = std::string(record.substr(2));
            status.entries.push_back(std::move(entry));
            break;
        }
        default: return corrupt(record);
        }
    }
    return status;
}

std::vector<std::string> statusArguments() {
    return {"status",       "--porcelain=v2",           "-z", "--branch", "--untracked-files=all",
            "--ignored=no", "--ignore-submodules=dirty"};
}

} // namespace frost::vcs
