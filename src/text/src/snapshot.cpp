// SPDX-License-Identifier: Apache-2.0
//
// Snapshot, ChunkRange and LineCursor (frost/text/snapshot.hpp). The special members live here so
// that the Storage type stays private; the queries delegate to piece_tree.cpp.

#include "frost/text/snapshot.hpp"

#include "frost/core/assert.hpp"

#include "piece_tree.hpp"
#include "storage.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::text {

Snapshot::Snapshot() noexcept = default;
Snapshot::Snapshot(const Snapshot& other) noexcept = default;
Snapshot::Snapshot(Snapshot&& other) noexcept = default;
Snapshot& Snapshot::operator=(const Snapshot& other) noexcept = default;
Snapshot& Snapshot::operator=(Snapshot&& other) noexcept = default;
Snapshot::~Snapshot() = default;

Snapshot::Snapshot(detail::PieceTree tree, detail::Ref<detail::Storage> storage, std::uint64_t version) noexcept
    : m_tree(std::move(tree)), m_storage(std::move(storage)), m_version(version) {}

bool Snapshot::sharesStorageWith(const Snapshot& other) const noexcept {
    return m_storage && m_storage == other.m_storage;
}

LineIndex Snapshot::lineAt(ByteOffset offset) const noexcept { return detail::pointAt(m_tree, offset.value()).line; }

Point Snapshot::pointAt(ByteOffset offset) const noexcept { return detail::pointAt(m_tree, offset.value()); }

Utf16Offset Snapshot::utf16At(ByteOffset offset) const noexcept {
    return Utf16Offset{detail::utf16At(m_tree, offset.value())};
}

Utf16Point Snapshot::utf16PointAt(ByteOffset offset) const noexcept {
    return detail::utf16PointAt(m_tree, offset.value());
}

bool Snapshot::isCharBoundary(ByteOffset offset) const noexcept {
    return detail::isCharBoundary(m_tree, offset.value());
}

ByteOffset Snapshot::clip(ByteOffset offset) const noexcept { return ByteOffset{detail::clip(m_tree, offset.value())}; }

char Snapshot::byteAt(ByteOffset offset) const noexcept {
    FROST_PRE(offset < size());
    return detail::byteAt(m_tree, offset.value());
}

Result<ByteOffset> Snapshot::offsetAt(Point point) const noexcept {
    if (point.line.value() >= lineCount()) {
        return fail(ErrorCode::OutOfRange,
                    std::format("line {} is out of range (the text has {} lines)", point.line, lineCount()));
    }
    detail::LineScan scan = detail::lineScanAt(m_tree, point.line.value());
    const LineInfo line = detail::nextLine(m_tree, scan);
    // Clamp to the content end (LSP), then floor a column inside a character to its start.
    const std::uint64_t column = std::min<std::uint64_t>(point.column.value(), line.content.size());
    return ByteOffset{detail::clip(m_tree, line.content.begin.value() + column)};
}

Result<ByteOffset> Snapshot::offsetAt(Utf16Point point) const noexcept {
    if (point.line.value() >= lineCount()) {
        return fail(ErrorCode::OutOfRange,
                    std::format("line {} is out of range (the text has {} lines)", point.line, lineCount()));
    }
    detail::LineScan scan = detail::lineScanAt(m_tree, point.line.value());
    const LineInfo line = detail::nextLine(m_tree, scan);
    const std::uint64_t first = detail::utf16At(m_tree, line.content.begin.value());
    const std::uint64_t last = detail::utf16At(m_tree, line.content.end.value());
    const std::uint64_t units = std::min(first + point.column.value(), last);
    return ByteOffset{detail::offsetAtUtf16(m_tree, units)};
}

Result<ByteOffset> Snapshot::offsetAtUtf16(Utf16Offset offset) const noexcept {
    if (offset > utf16Size()) {
        return fail(ErrorCode::OutOfRange,
                    std::format("UTF-16 offset {} is beyond the text's {} units", offset, utf16Size()));
    }
    return ByteOffset{detail::offsetAtUtf16(m_tree, offset.value())};
}

Result<LineInfo> Snapshot::lineInfo(LineIndex line) const noexcept {
    if (line.value() >= lineCount()) {
        return fail(ErrorCode::OutOfRange,
                    std::format("line {} is out of range (the text has {} lines)", line, lineCount()));
    }
    detail::LineScan scan = detail::lineScanAt(m_tree, line.value());
    return detail::nextLine(m_tree, scan);
}

std::string Snapshot::text() const noexcept { return text(ByteRange{ByteOffset{0}, size()}); }

std::string Snapshot::text(ByteRange range) const noexcept {
    FROST_PRE(range.begin <= range.end && range.end <= size());
    std::string out;
    out.reserve(static_cast<std::size_t>(range.size()));
    if (range.empty()) {
        return out;
    }
    const std::uint64_t end = range.end.value();
    detail::PieceTree::Cursor cursor = detail::seekByte(m_tree, range.begin.value());
    std::uint64_t position = range.begin.value();
    while (position < end && cursor.item() != nullptr) {
        const std::string_view piece = cursor.item()->view();
        const std::uint64_t pieceStart = cursor.prefix().bytes;
        const auto from = static_cast<std::size_t>(position - pieceStart);
        const auto to = static_cast<std::size_t>(std::min<std::uint64_t>(end - pieceStart, piece.size()));
        out.append(piece.substr(from, to - from));
        position = pieceStart + to;
        cursor.next();
    }
    return out;
}

TextSummary Snapshot::summaryOf(ByteRange range) const noexcept {
    FROST_PRE(range.begin <= range.end && range.end <= size());
    FROST_PRE(isCharBoundary(range.begin) && isCharBoundary(range.end));
    return detail::summarizeRange(m_tree, range.begin.value(), range.end.value());
}

ChunkRange Snapshot::chunks(ByteRange range) const noexcept {
    FROST_PRE(range.begin <= range.end && range.end <= size());
    FROST_PRE(isCharBoundary(range.begin) && isCharBoundary(range.end));
    return ChunkRange(*this, range);
}

std::string_view Snapshot::chunkAt(ByteOffset offset) const noexcept {
    FROST_PRE(offset <= size());
    if (offset == size()) {
        return {};
    }
    const detail::PieceTree::Cursor cursor = detail::seekByte(m_tree, offset.value());
    return cursor.item()->view().substr(static_cast<std::size_t>(offset.value() - cursor.prefix().bytes));
}

Result<LineCursor> Snapshot::linesFrom(LineIndex first) const noexcept {
    if (first.value() >= lineCount()) {
        return fail(ErrorCode::OutOfRange,
                    std::format("line {} is out of range (the text has {} lines)", first, lineCount()));
    }
    // The scan's cursor borrows this snapshot's nodes, which the LineCursor's copy keeps alive.
    detail::LineScan scan = detail::lineScanAt(m_tree, first.value());
    return LineCursor(*this, scan.cursor, scan.position, scan.line);
}

ChunkRange::ChunkRange(Snapshot snapshot, ByteRange range) noexcept : m_snapshot(std::move(snapshot)), m_range(range) {}

ChunkRange::Iterator ChunkRange::begin() const noexcept {
    Iterator it;
    if (m_range.empty()) {
        return it;
    }
    it.m_end = m_range.end.value();
    it.m_cursor = detail::seekByte(m_snapshot.pieceTree(), m_range.begin.value());
    const std::string_view piece = it.m_cursor.item()->view();
    const std::uint64_t pieceStart = it.m_cursor.prefix().bytes;
    const auto from = static_cast<std::size_t>(m_range.begin.value() - pieceStart);
    const auto to = static_cast<std::size_t>(std::min<std::uint64_t>(it.m_end - pieceStart, piece.size()));
    it.m_chunk = piece.substr(from, to - from);
    return it;
}

ChunkRange::Iterator& ChunkRange::Iterator::operator++() noexcept {
    const std::uint64_t chunkEnd = m_cursor.prefix().bytes + m_cursor.item()->size();
    if (chunkEnd >= m_end || !m_cursor.next()) {
        m_chunk = {};
        return *this;
    }
    const std::string_view piece = m_cursor.item()->view();
    const std::uint64_t pieceStart = m_cursor.prefix().bytes;
    m_chunk = piece.substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(m_end - pieceStart, piece.size())));
    return *this;
}

LineCursor::LineCursor(Snapshot snapshot, detail::PieceTree::Cursor cursor, std::uint64_t position,
                       std::uint32_t line) noexcept
    : m_snapshot(std::move(snapshot)), m_cursor(cursor), m_position(position), m_line(line) {}

bool LineCursor::next(LineInfo& line) noexcept {
    if (m_done) {
        return false;
    }
    detail::LineScan scan{m_cursor, m_position, m_line};
    line = detail::nextLine(m_snapshot.pieceTree(), scan);
    if (scan.line > m_snapshot.summary().lineBreaks) {
        m_done = true;
    }
    m_cursor = scan.cursor;
    m_position = scan.position;
    m_line = scan.line;
    return true;
}

} // namespace frost::text
