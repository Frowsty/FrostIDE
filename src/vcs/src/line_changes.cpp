// SPDX-License-Identifier: Apache-2.0
//
// Editor change markers (frost/vcs/line_changes.hpp): hunks to LineChanges and the rollback edit.

#include "frost/vcs/line_changes.hpp"

#include <algorithm>

namespace frost::vcs {

std::string_view toString(LineChangeKind kind) noexcept {
    switch (kind) {
    case LineChangeKind::Added: return "added";
    case LineChangeKind::Modified: return "modified";
    case LineChangeKind::Deleted: return "deleted";
    }
    return "modified";
}

Result<std::vector<LineChange>> computeLineChanges(const diff::LineSequence& base, const diff::LineSequence& current,
                                                   std::stop_token stop) {
    auto diffed = diff::diffLines(base, current, {}, std::move(stop));
    if (!diffed) {
        return std::unexpected(std::move(diffed.error()));
    }
    std::vector<LineChange> changes;
    changes.reserve(diffed->hunks.size());
    for (const diff::Hunk& hunk : diffed->hunks) {
        LineChange change;
        change.base = hunk.a;
        change.current = hunk.b;
        change.kind = hunk.a.empty()   ? LineChangeKind::Added
                      : hunk.b.empty() ? LineChangeKind::Deleted
                                       : LineChangeKind::Modified;
        changes.push_back(change);
    }
    return changes;
}

std::string_view baseText(const diff::LineSequence& base, const LineChange& change) noexcept {
    if (change.base.empty()) {
        return {};
    }
    const text::ByteRange range = base.byteRange(change.base);
    return base.bytes().substr(range.begin.value(), range.end - range.begin);
}

LineRollback rollbackEdit(const diff::LineSequence& base, const diff::LineSequence& current, const LineChange& change) {
    return LineRollback{current.byteRange(change.current), std::string(baseText(base, change))};
}

const LineChange* changeAtLine(std::span<const LineChange> changes, LineIndex line) noexcept {
    // The first change that ends after `line` (a Deleted change "ends" at its begin).
    const auto it = std::ranges::lower_bound(changes, line, {}, [](const LineChange& change) {
        return change.current.empty() ? change.current.begin : change.current.end - 1;
    });
    if (it == changes.end()) {
        return nullptr;
    }
    const bool covers =
        it->current.empty() ? it->current.begin == line : it->current.begin <= line && line < it->current.end;
    return covers ? &*it : nullptr;
}

} // namespace frost::vcs
