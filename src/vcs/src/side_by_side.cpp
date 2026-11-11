// SPDX-License-Identifier: Apache-2.0
//
// The side-by-side diff model (frost/vcs/side_by_side.hpp): blocks, refinement, line mapping and
// navigation.

#include "frost/vcs/side_by_side.hpp"

#include <algorithm>
#include <cstdint>

namespace frost::vcs {

LineChangeKind DiffBlock::kind() const noexcept {
    if (hunk.a.empty()) {
        return LineChangeKind::Added;
    }
    if (hunk.b.empty()) {
        return LineChangeKind::Deleted;
    }
    return LineChangeKind::Modified;
}

Result<SideBySideDiff> SideBySideDiff::compute(std::string left, std::string right, const std::stop_token& stop) {
    auto a = diff::LineSequence::fromBytes(std::move(left), diff::LineSplit::Lf);
    if (!a) {
        return std::unexpected(std::move(a.error()));
    }
    auto b = diff::LineSequence::fromBytes(std::move(right), diff::LineSplit::Lf);
    if (!b) {
        return std::unexpected(std::move(b.error()));
    }
    auto diffed = diff::diffLines(*a, *b, {}, stop);
    if (!diffed) {
        return std::unexpected(std::move(diffed.error()));
    }
    SideBySideDiff result;
    result.m_left = std::move(*a);
    result.m_right = std::move(*b);
    result.m_blocks.reserve(diffed->hunks.size());
    for (const diff::Hunk& hunk : diffed->hunks) {
        DiffBlock block{hunk, {}, false};
        if (!hunk.a.empty() && !hunk.b.empty()) {
            auto refined = diff::refineHunk(result.m_left, result.m_right, hunk, diff::Granularity::Word, {}, stop);
            if (!refined) {
                return std::unexpected(std::move(refined.error()));
            }
            block.inner = std::move(refined->changes);
            block.coarse = refined->coarse;
        }
        result.m_blocks.push_back(std::move(block));
    }
    return result;
}

std::vector<diff::Hunk> SideBySideDiff::hunks() const {
    std::vector<diff::Hunk> out;
    out.reserve(m_blocks.size());
    for (const DiffBlock& block : m_blocks) {
        out.push_back(block.hunk);
    }
    return out;
}

LineIndex SideBySideDiff::mapLine(DiffSide from, LineIndex line) const noexcept {
    const DiffSide to = from == DiffSide::Left ? DiffSide::Right : DiffSide::Left;
    std::int64_t fromEnd = 0;
    std::int64_t toEnd = 0;
    const std::int64_t at = line.value();
    for (const DiffBlock& block : m_blocks) {
        const diff::LineRange source = block.lines(from);
        const diff::LineRange target = block.lines(to);
        if (at < static_cast<std::int64_t>(source.begin.value())) {
            break;
        }
        if (at < static_cast<std::int64_t>(source.end.value())) {
            const std::uint32_t offset = line - source.begin;
            const std::uint32_t clamped = target.empty() ? 0 : std::min(offset, target.size() - 1);
            return target.begin + clamped;
        }
        fromEnd = source.end.value();
        toEnd = target.end.value();
    }
    const std::int64_t mapped = at - fromEnd + toEnd;
    const std::int64_t limit = text(to).size();
    return LineIndex{static_cast<std::uint32_t>(std::clamp<std::int64_t>(mapped, 0, limit))};
}

std::optional<std::size_t> SideBySideDiff::blockAfter(DiffSide side, LineIndex line) const noexcept {
    const auto it = std::ranges::upper_bound(m_blocks, line, {}, [side](const DiffBlock& b) { return b.begin(side); });
    if (it == m_blocks.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - m_blocks.begin());
}

std::optional<std::size_t> SideBySideDiff::blockBefore(DiffSide side, LineIndex line) const noexcept {
    const auto it = std::ranges::lower_bound(m_blocks, line, {}, [side](const DiffBlock& b) { return b.begin(side); });
    if (it == m_blocks.begin()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - m_blocks.begin()) - 1;
}

std::optional<std::size_t> SideBySideDiff::blockAt(DiffSide side, LineIndex line) const noexcept {
    for (std::size_t i = 0; i < m_blocks.size(); ++i) {
        const diff::LineRange lines = m_blocks[i].lines(side);
        if (lines.empty() ? lines.begin == line : lines.begin <= line && line < lines.end) {
            return i;
        }
        if (lines.begin > line) {
            break;
        }
    }
    return std::nullopt;
}

} // namespace frost::vcs
