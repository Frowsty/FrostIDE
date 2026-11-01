// SPDX-License-Identifier: Apache-2.0
//
// LineSequence (frost/diff/line_sequence.hpp): limit checks before any allocation, the line table
// for both splits, FNV-1a line hashes and the first line break.

#include "frost/diff/line_sequence.hpp"

#include "frost/core/assert.hpp"
#include "frost/text/snapshot.hpp"

#include "fnv.hpp"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace frost::diff {

// Heap-allocated once and never moved, so views into `bytes` stay valid while any copy lives.
struct LineSequence::Storage {
    std::string bytes;
    std::vector<std::uint32_t> starts; // size() + 1 entries; starts[size()] == bytes.size().
    std::vector<std::uint64_t> hashes; // One per line.
    LineSplit split = LineSplit::Lsp;
    text::LineBreak firstBreak = text::LineBreak::None;
};

namespace {

constexpr std::uint64_t kHardMaxBytes = 0xFFFF'FFFEULL;

// The number of lines of `bytes` under `split` (the final empty line is not counted). O(n).
std::uint64_t countLines(std::string_view bytes, LineSplit split) noexcept {
    std::uint64_t lines = 0;
    const std::size_t n = bytes.size();
    if (split == LineSplit::Lf) {
        lines = static_cast<std::uint64_t>(std::count(bytes.begin(), bytes.end(), '\n'));
        if (n > 0 && bytes[n - 1] != '\n') {
            ++lines;
        }
        return lines;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const char c = bytes[i];
        if (c == '\n' || (c == '\r' && (i + 1 == n || bytes[i + 1] != '\n'))) {
            ++lines;
        }
    }
    if (n > 0 && bytes[n - 1] != '\n' && bytes[n - 1] != '\r') {
        ++lines;
    }
    return lines;
}

Result<void> checkSize(std::uint64_t bytes, const SequenceLimits& limits) noexcept {
    const std::uint64_t cap = std::min(limits.maxBytes, kHardMaxBytes);
    if (bytes > cap) {
        return fail(ErrorCode::OutOfRange, std::format("text of {} bytes exceeds the limit of {} bytes", bytes, cap));
    }
    return {};
}

Result<void> checkLines(std::uint64_t lines, const SequenceLimits& limits) noexcept {
    if (lines > limits.maxLines) {
        return fail(ErrorCode::OutOfRange,
                    std::format("text of {} lines exceeds the limit of {} lines", lines, limits.maxLines));
    }
    return {};
}

} // namespace

LineSequence::LineSequence(std::shared_ptr<const Storage> storage) noexcept : m_storage(std::move(storage)) {}

Result<LineSequence> LineSequence::fromBytes(std::string bytes, LineSplit split,
                                             const SequenceLimits& limits) noexcept {
    if (auto sized = checkSize(bytes.size(), limits); !sized) {
        return std::unexpected(std::move(sized.error()));
    }
    const std::uint64_t lines = countLines(bytes, split);
    if (auto counted = checkLines(lines, limits); !counted) {
        return std::unexpected(std::move(counted.error()));
    }
    auto storage = std::make_shared<Storage>();
    storage->bytes = std::move(bytes);
    storage->split = split;
    const std::string_view view = storage->bytes;
    const std::size_t n = view.size();
    storage->starts.reserve(static_cast<std::size_t>(lines) + 1);
    storage->hashes.reserve(static_cast<std::size_t>(lines));
    std::size_t start = 0;
    const auto endLine = [&](std::size_t end, text::LineBreak kind) {
        storage->starts.push_back(static_cast<std::uint32_t>(start));
        storage->hashes.push_back(detail::fnv1a64(view.substr(start, end - start)));
        if (storage->firstBreak == text::LineBreak::None) {
            storage->firstBreak = kind;
        }
        start = end;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const char c = view[i];
        if (c == '\n') {
            const bool crlf = i > start && view[i - 1] == '\r';
            endLine(i + 1, crlf ? text::LineBreak::CrLf : text::LineBreak::Lf);
        } else if (c == '\r' && split == LineSplit::Lsp) {
            if (i + 1 < n && view[i + 1] == '\n') {
                ++i;
                endLine(i + 1, text::LineBreak::CrLf);
            } else {
                endLine(i + 1, text::LineBreak::Cr);
            }
        }
    }
    if (start < n) {
        // The last line, without a break; it does not set firstBreak.
        storage->starts.push_back(static_cast<std::uint32_t>(start));
        storage->hashes.push_back(detail::fnv1a64(view.substr(start)));
    }
    storage->starts.push_back(static_cast<std::uint32_t>(n));
    FROST_ASSERT(storage->hashes.size() == lines);
    return LineSequence(std::move(storage));
}

Result<LineSequence> LineSequence::fromSnapshot(const text::Snapshot& snapshot, LineSplit split,
                                                const SequenceLimits& limits) noexcept {
    const std::uint64_t size = snapshot.size().value();
    if (auto sized = checkSize(size, limits); !sized) {
        return std::unexpected(std::move(sized.error()));
    }
    const text::TextSummary& summary = snapshot.summary();
    std::uint64_t lines = 0;
    if (split == LineSplit::Lsp) {
        // Every break ends a line; a non-empty last row is one more.
        lines = std::uint64_t{summary.lineBreaks} + (summary.lastLineBytes > 0 ? 1 : 0);
    } else {
        // Every LF ends a line; one more unless the text is empty or ends with LF. The last byte is
        // LF exactly when the last row is empty (the text ends with a break) and that break is not a
        // lone CR.
        const bool endsWithLf = size > 0 && summary.lastLineBytes == 0 && !summary.endsWithCr;
        lines = std::uint64_t{summary.lineFeeds} + (size > 0 && !endsWithLf ? 1 : 0);
    }
    if (auto counted = checkLines(lines, limits); !counted) {
        return std::unexpected(std::move(counted.error()));
    }
    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(size));
    for (const std::string_view chunk : snapshot.chunks(text::ByteRange{ByteOffset{0}, snapshot.size()})) {
        bytes.append(chunk);
    }
    return fromBytes(std::move(bytes), split, limits);
}

std::uint32_t LineSequence::size() const noexcept {
    return m_storage ? static_cast<std::uint32_t>(m_storage->hashes.size()) : 0;
}

LineSplit LineSequence::split() const noexcept { return m_storage ? m_storage->split : LineSplit::Lsp; }

std::string_view LineSequence::bytes() const noexcept {
    return m_storage ? std::string_view(m_storage->bytes) : std::string_view();
}

std::string_view LineSequence::line(LineIndex line) const noexcept {
    FROST_PRE(line.value() < size());
    const std::uint32_t begin = m_storage->starts[line.value()];
    const std::uint32_t end = m_storage->starts[line.value() + 1];
    return std::string_view(m_storage->bytes).substr(begin, end - begin);
}

text::ByteRange LineSequence::lineRange(LineIndex line) const noexcept {
    FROST_PRE(line.value() < size());
    return text::ByteRange{ByteOffset{m_storage->starts[line.value()]},
                           ByteOffset{m_storage->starts[line.value() + 1]}};
}

text::ByteRange LineSequence::byteRange(LineRange range) const noexcept {
    FROST_PRE(range.begin <= range.end && range.end.value() <= size());
    if (!m_storage) {
        return text::ByteRange{};
    }
    return text::ByteRange{ByteOffset{m_storage->starts[range.begin.value()]},
                           ByteOffset{m_storage->starts[range.end.value()]}};
}

std::uint64_t LineSequence::lineHash(LineIndex line) const noexcept {
    FROST_PRE(line.value() < size());
    return m_storage->hashes[line.value()];
}

text::LineBreak LineSequence::firstLineBreak() const noexcept {
    return m_storage ? m_storage->firstBreak : text::LineBreak::None;
}

bool LineSequence::hasBreak(LineIndex line) const noexcept {
    const std::string_view bytes = this->line(line);
    const char last = bytes.back();
    return last == '\n' || (last == '\r' && split() == LineSplit::Lsp);
}

} // namespace frost::diff
