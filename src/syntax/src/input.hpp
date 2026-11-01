// SPDX-License-Identifier: Apache-2.0
//
// input.hpp: the TSInput frost::syntax gives tree-sitter (ADR 0023, D3): it reads a text::Snapshot
// chunk by chunk and presents every lone CR as LF, so tree-sitter's LF rows equal the editor's
// LSP lines, and converts D8 TextChanges into TSInputEdits.
#pragma once

#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"

#include "ts.hpp"

#include <span>
#include <string>
#include <vector>

namespace frost::syntax::detail {

/*
 * The payload of a TSInput. `loneCr` is true when the snapshot has a CR that is not part of a CRLF
 * (summary().lineBreaks != lineFeeds, O(1)); only then does read() scan chunks for CRs and copy
 * a chunk into `scratch` to substitute them. LF and CRLF documents take the zero-copy path.
 * Must stay at a fixed address while tree-sitter parses (it keeps the payload pointer).
 * Not thread-safe: one parse reads it at a time.
 */
struct InputSource {
    text::Snapshot snapshot;
    bool loneCr = false;
    std::string scratch;

    explicit InputSource(text::Snapshot text) noexcept;
};

// A UTF-8 TSInput reading `source`, which must outlive every parse call that uses it.
[[nodiscard]] TSInput makeInput(InputSource& source) noexcept;

/*
 * The read callback: the rest of the piece at `byte` (Snapshot::chunkAt), with each lone CR shown
 * as LF; *bytesRead = 0 at or past the end. The returned bytes stay valid until the next call
 * (tree-sitter's contract). The TSPoint argument is not needed. Budget: B5.
 */
const char* readInput(void* payload, std::uint32_t byte, TSPoint position, std::uint32_t* bytesRead) noexcept;

// The TSInputEdit of one D8 change: bytes and points field by field (lines are rows, byte columns
// are columns; ADR 0023 D3). Precondition: offsets fit in 32 bits (the session's size limit).
// Budget: B5.
[[nodiscard]] TSInputEdit toInputEdit(const text::TextChange& change) noexcept;

// The edits of `changes` in order, skipping empty no-op changes.
[[nodiscard]] std::vector<TSInputEdit> toInputEdits(std::span<const text::TextChange> changes) noexcept;

} // namespace frost::syntax::detail
