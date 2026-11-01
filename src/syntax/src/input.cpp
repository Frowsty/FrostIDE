// SPDX-License-Identifier: Apache-2.0
//
// input.cpp: frost::syntax's tree-sitter input over text snapshots and the TextChange ->
// TSInputEdit mapping (ADR 0023, D3).
/*
 * Why lone CRs become LF (the D3 proof, in short): let V(T)[p] be '\n' when T[p] is a CR not
 * followed by LF, else T[p]. V has exactly one LF per LSP line break (LF -> LF, lone CR -> LF,
 * CRLF -> CR LF), so a byte's tree-sitter row is its LSP line and its column (bytes since the
 * last LF) is its LSP byte column, except strictly inside a CRLF. V[p] depends only on T[p] and
 * T[p+1], so outside a change V can differ only at the byte before it, when that is a CR and the
 * change flips whether an LF follows; D8 widens exactly that case into the change. D8 bounds are
 * never CRLF-interior, so each TextChange is an exact edit of V with tree-sitter points equal to
 * its LSP points. CRLF passes through unchanged: replacing its CR would break `\`-CRLF line
 * continuations in C and C++.
 */

#include "input.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <cstring>

namespace frost::syntax::detail {

InputSource::InputSource(text::Snapshot text) noexcept
    : snapshot(std::move(text)), loneCr(snapshot.summary().lineBreaks != snapshot.summary().lineFeeds) {}

TSInput makeInput(InputSource& source) noexcept {
    TSInput input{};
    input.payload = &source;
    input.read = &readInput;
    input.encoding = TSInputEncodingUTF8;
    input.decode = nullptr;
    return input;
}

const char* readInput(void* payload, std::uint32_t byte, TSPoint /*position*/, std::uint32_t* bytesRead) noexcept {
    auto& source = *static_cast<InputSource*>(payload);
    const std::uint64_t size = source.snapshot.size().value();
    if (byte >= size) {
        *bytesRead = 0;
        return "";
    }
    const std::string_view chunk = source.snapshot.chunkAt(ByteOffset{byte});
    *bytesRead = static_cast<std::uint32_t>(chunk.size());
    if (!source.loneCr || std::memchr(chunk.data(), '\r', chunk.size()) == nullptr) {
        return chunk.data();
    }
    source.scratch.assign(chunk);
    for (std::size_t i = 0; i < chunk.size(); ++i) {
        if (chunk[i] != '\r') {
            continue;
        }
        // A CR at the chunk's end looks at the first byte of the next piece.
        const std::uint64_t next = byte + i + 1;
        const bool lfFollows = i + 1 < chunk.size() ? chunk[i + 1] == '\n'
                                                    : next < size && source.snapshot.byteAt(ByteOffset{next}) == '\n';
        if (!lfFollows) {
            source.scratch[i] = '\n';
        }
    }
    return source.scratch.data();
}

TSInputEdit toInputEdit(const text::TextChange& change) noexcept {
    FROST_PRE(change.oldEnd.value() <= 0xFFFF'FFFFu && change.newEnd.value() <= 0xFFFF'FFFFu);
    TSInputEdit edit{};
    edit.start_byte = static_cast<std::uint32_t>(change.start.value());
    edit.old_end_byte = static_cast<std::uint32_t>(change.oldEnd.value());
    edit.new_end_byte = static_cast<std::uint32_t>(change.newEnd.value());
    edit.start_point = TSPoint{change.startPoint.line.value(), change.startPoint.column.value()};
    edit.old_end_point = TSPoint{change.oldEndPoint.line.value(), change.oldEndPoint.column.value()};
    edit.new_end_point = TSPoint{change.newEndPoint.line.value(), change.newEndPoint.column.value()};
    return edit;
}

std::vector<TSInputEdit> toInputEdits(std::span<const text::TextChange> changes) noexcept {
    std::vector<TSInputEdit> edits;
    edits.reserve(changes.size());
    for (const text::TextChange& change : changes) {
        if (change.start == change.oldEnd && change.start == change.newEnd) {
            continue;
        }
        edits.push_back(toInputEdit(change));
    }
    return edits;
}

} // namespace frost::syntax::detail
