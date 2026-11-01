// SPDX-License-Identifier: Apache-2.0
//
// SliceEdit: one replacement of TextBuffer::replaySlices, the precise undo and redo path of
// frost::text::Document (ADR 0017, decision 5). Not public API: text_buffer.hpp needs the complete
// type for the private member's declaration.
#pragma once

#include "frost/text/detail/piece.hpp"
#include "frost/text/positions.hpp"

namespace frost::text::detail {

// `range` of the current text becomes the pieces of `slice`, which point into the same buffer's
// storage (cut from an earlier tree of that buffer, never from another buffer's).
struct SliceEdit {
    ByteRange range;
    PieceTree slice;
};

} // namespace frost::text::detail
