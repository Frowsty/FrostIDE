// SPDX-License-Identifier: Apache-2.0
//
// Private helpers of frost::text_editor (ADR 0025): code point cursors that walk a Snapshot forwards
// (over ChunkRange) and backwards (over ReverseChunkRange), and a forward grapheme cluster cursor
// built on utf8proc. Used by unicode.cpp, the commands and the search.
#pragma once

#include "frost/core/strong_type.hpp"
#include "frost/text/editor/unicode.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_shaper.hpp"

#include <cstdint>
#include <iterator>
#include <string_view>

namespace frost::text::detail {

/*
 * The code points of [from, size) in order. Holds the ChunkRange its iterator borrows, so it can be
 * neither copied nor moved. Precondition: `from` is a char boundary. O(B log n) to start, O(1) per
 * code point.
 */
class ForwardChars {
public:
    ForwardChars(const Snapshot& text, ByteOffset from) noexcept
        : m_chunks(text.chunks(ByteRange{from, text.size()})), m_it(m_chunks.begin()), m_offset(from) {
        if (m_it != std::default_sentinel) {
            m_rest = *m_it;
        }
    }
    ForwardChars(const ForwardChars&) = delete ("its iterator borrows its ChunkRange");
    ForwardChars& operator=(const ForwardChars&) = delete ("its iterator borrows its ChunkRange");

    // Stores the next code point and its offset; false at the end.
    bool next(ByteOffset& at, DecodedChar& out) noexcept {
        if (m_rest.empty()) {
            if (m_it == std::default_sentinel) {
                return false;
            }
            ++m_it;
            if (m_it == std::default_sentinel) {
                return false;
            }
            m_rest = *m_it;
        }
        out = decodeUtf8(m_rest);
        at = m_offset;
        m_offset += out.bytes;
        m_rest.remove_prefix(out.bytes);
        return true;
    }

private:
    ChunkRange m_chunks;
    ChunkRange::Iterator m_it;
    ByteOffset m_offset;
    std::string_view m_rest; // The unread part of the current chunk.
};

/*
 * The code points of [0, to) from the last to the first. Same ownership rules as ForwardChars.
 * Precondition: `to` is a char boundary. O(B log n) per chunk, O(1) per code point.
 */
class BackwardChars {
public:
    BackwardChars(const Snapshot& text, ByteOffset to) noexcept
        : m_chunks(text.reversedChunks(ByteRange{ByteOffset{0}, to})), m_it(m_chunks.begin()) {
        if (m_it != std::default_sentinel) {
            m_rest = *m_it;
            m_restStart = m_it.offset();
        }
    }
    BackwardChars(const BackwardChars&) = delete ("its iterator borrows its ReverseChunkRange");
    BackwardChars& operator=(const BackwardChars&) = delete ("its iterator borrows its ReverseChunkRange");

    // Stores the previous code point and its offset; false at the start of the text.
    bool next(ByteOffset& at, DecodedChar& out) noexcept {
        if (m_rest.empty()) {
            if (m_it == std::default_sentinel) {
                return false;
            }
            ++m_it;
            if (m_it == std::default_sentinel) {
                return false;
            }
            m_rest = *m_it;
            m_restStart = m_it.offset();
        }
        std::size_t lead = m_rest.size() - 1;
        while (lead > 0 && (static_cast<unsigned char>(m_rest[lead]) & 0xC0u) == 0x80u) {
            --lead;
        }
        out = decodeUtf8(m_rest.substr(lead));
        at = m_restStart + lead;
        m_rest = m_rest.substr(0, lead);
        return true;
    }

private:
    ReverseChunkRange m_chunks;
    ReverseChunkRange::Iterator m_it;
    std::string_view m_rest; // The unread part of the current chunk (a prefix of it).
    ByteOffset m_restStart;  // The offset of m_rest's first byte.
};

// One grapheme cluster: its bytes and its first code point.
struct Cluster {
    ByteRange range;
    char32_t first = 0;
};

/*
 * The extended grapheme clusters of [from, size), in order, by utf8proc's stateful break rule.
 * Precondition: `from` is a cluster boundary (or the scan is approximate from there; unicode.hpp).
 */
class ForwardClusters {
public:
    ForwardClusters(const Snapshot& text, ByteOffset from) noexcept : m_chars(text, from) {
        m_has = m_chars.next(m_at, m_char);
    }

    // Stores the next cluster; false at the end.
    bool next(Cluster& out) noexcept;

private:
    ForwardChars m_chars;
    bool m_has = false; // m_char is the first unconsumed code point.
    ByteOffset m_at;    // Its offset.
    DecodedChar m_char{};
};

} // namespace frost::text::detail
