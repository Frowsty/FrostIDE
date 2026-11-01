// SPDX-License-Identifier: Apache-2.0
//
// Invariant checks of frost::text (ADR 0012, section 7): Snapshot::checkInvariants (S1-S4 through
// SumTree, plus the piece rules P1-P4) and TextBuffer::checkInvariants (plus P5 and P6). Each
// failure is ErrorCode::Internal with the invariant's ID first in the message. For tests and
// fuzzing; O(bytes + n).

#include "frost/io/utf.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"

#include "piece_tree.hpp"
#include "storage.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <vector>

namespace frost::text {

Result<void> Snapshot::checkInvariants() const noexcept {
    if (auto structure = m_tree.checkInvariants(); !structure) {
        return structure;
    }
    // A default Snapshot has no storage; its (empty) tree is checked against the default options.
    const TextBufferOptions options = m_storage ? m_storage->options() : TextBufferOptions{};
    if (!m_storage && !m_tree.empty()) {
        return fail(ErrorCode::Internal, "P1: a snapshot without storage holds pieces");
    }
    std::uint64_t index = 0;
    std::uint64_t offset = 0;
    for (detail::PieceTree::Cursor cursor = m_tree.first(); cursor.item() != nullptr; cursor.next(), ++index) {
        const detail::Piece& piece = *cursor.item();
        const std::uint64_t bytes = piece.summary.bytes;
        if (bytes < 1 || bytes > options.maxPieceBytes) {
            return fail(ErrorCode::Internal, std::format("P1: piece {} at byte {} holds {} bytes, expected 1..{}",
                                                         index, offset, bytes, options.maxPieceBytes));
        }
        if (!(summarize(piece.view()) == piece.summary)) {
            return fail(
                ErrorCode::Internal,
                std::format("P2: piece {} at byte {} has a summary that differs from its bytes", index, offset));
        }
        if (const std::size_t valid = io::validUtf8Prefix(piece.view()); valid != piece.size()) {
            return fail(ErrorCode::Internal, std::format("P3: piece {} at byte {} is ill-formed UTF-8 at its byte {}",
                                                         index, offset, valid));
        }
        offset += bytes;
    }
    if (m_tree.summary().bytes > options.maxBufferBytes) {
        return fail(ErrorCode::Internal, std::format("P4: the text has {} bytes, more than the limit of {}",
                                                     m_tree.summary().bytes, options.maxBufferBytes));
    }
    return {};
}

Result<void> TextBuffer::checkInvariants() const noexcept {
    if (!m_storage) {
        return fail(ErrorCode::Internal, "P6: the buffer has no storage (moved from)");
    }
    if (auto snapshot = m_current.checkInvariants(); !snapshot) {
        return snapshot;
    }
    if (m_current.version() != m_version) {
        return fail(ErrorCode::Internal,
                    std::format("P6: current() is at version {}, the buffer at {}", m_current.version(), m_version));
    }
    if (m_current.m_storage != m_storage) {
        return fail(ErrorCode::Internal, "P6: current() does not share the buffer's storage");
    }
    // P5: every piece inside the used part of one region. Regions sorted by address (as integers,
    // so comparing unrelated allocations is well defined) for a binary search per piece.
    struct Span {
        std::uintptr_t begin;
        std::uintptr_t end;
    };
    std::vector<Span> spans;
    for (const detail::Storage::Region& region : m_storage->regions()) {
        const auto begin = reinterpret_cast<std::uintptr_t>(region.data);
        spans.push_back({begin, begin + region.size});
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.begin < b.begin; });
    std::uint64_t index = 0;
    for (detail::PieceTree::Cursor cursor = m_current.pieceTree().first(); cursor.item() != nullptr;
         cursor.next(), ++index) {
        const detail::Piece& piece = *cursor.item();
        const auto begin = reinterpret_cast<std::uintptr_t>(piece.data);
        const std::uintptr_t end = begin + piece.size();
        // The last region starting at or before the piece.
        auto it = std::upper_bound(spans.begin(), spans.end(), begin,
                                   [](std::uintptr_t value, const Span& span) { return value < span.begin; });
        bool inside = false;
        // Empty regions (an empty original, a fresh chunk) can share a start with another region.
        while (it != spans.begin() && !inside) {
            --it;
            inside = begin >= it->begin && end <= it->end;
            if (it->begin < begin) {
                break;
            }
        }
        if (!inside) {
            return fail(
                ErrorCode::Internal,
                std::format("P5: piece {} lies outside the used part of the original and of every add chunk", index));
        }
    }
    return {};
}

} // namespace frost::text
