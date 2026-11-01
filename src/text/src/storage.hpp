// SPDX-License-Identifier: Apache-2.0
//
// frost::text's append-only byte storage (ADR 0013, D5; private header). One original text plus
// add chunks; pieces point into it with raw pointers, so its memory never moves or frees while any
// owner (the TextBuffer and every Snapshot, through Ref<Storage>) lives.
/*
 * The original-plus-add-buffer layout is the piece table VS Code describes in "Text Buffer
 * Reimplementation" (Peng Lyu, 2018-03-23,
 * https://code.visualstudio.com/blogs/2018/03/23/text-buffer-reimplementation). The code it
 * describes, src/vs/editor/common/model/pieceTreeTextBuffer/ in microsoft/vscode (MIT; latest
 * commit a4ba39c), was not needed: this file follows the post. Unlike VS Code, the add text lives
 * in fixed chunks that are never reallocated, so readers on other threads keep raw pointers into
 * it while the writer appends.
 *
 * Concurrency: the writer only ever writes bytes past every byte a piece references (published
 * bytes), so a reader never reads memory the writer writes (distinct bytes are distinct memory
 * locations, and ThreadSanitizer tracks them byte-precisely). The chunk directory and the write
 * cursor are read only by the writer (P5) and the destructor. The options are immutable after
 * construction and readable from any thread.
 *
 * Deleted text is not reclaimed during a buffer's life, as with VS Code's add buffer.
 * TODO(M13): compaction. It would also invalidate storage-position anchors, if m1/undo-anchors
 * chooses them.
 */
#pragma once

#include "frost/text/detail/ref.hpp"
#include "frost/text/text_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text::detail {

class Storage : public RefCounted {
public:
    // Small inserts share 64 KiB chunks; an insert of at least 16 KiB gets a chunk of its own.
    static constexpr std::size_t kAddChunkBytes = 64 * 1024;
    static constexpr std::size_t kDedicatedChunkBytes = 16 * 1024;

    // Storage with an empty original.
    explicit Storage(TextBufferOptions options) noexcept;
    // Storage whose original is `original`, moved to its final heap place before any piece can
    // point into it (moving a short std::string would move its SSO buffer).
    Storage(TextBufferOptions options, std::string original) noexcept;

    // Immutable; any thread.
    [[nodiscard]] const TextBufferOptions& options() const noexcept { return m_options; }
    [[nodiscard]] std::string_view original() const noexcept { return *m_original; }

    // Writer only. Copies `text` (non-empty) past every published byte and returns where the copy
    // starts; the copy is then published by building pieces over it. `text` may point into this
    // storage. O(|text|).
    [[nodiscard]] const char* append(std::string_view text) noexcept;

    // Writer only. True when the `size` bytes at `data` end exactly at the write cursor of the
    // current small chunk (so appending extends them in place). Compares positions within one
    // chunk only; a piece ending where another allocation happens to start never matches.
    [[nodiscard]] bool endsAtCursor(const char* data, std::size_t size) const noexcept;
    // Writer only. The bytes left in the current small chunk (0 when there is none).
    [[nodiscard]] std::size_t cursorRoom() const noexcept;

    // A region pieces may point into: the original or the used part of one add chunk.
    struct Region {
        const char* data;
        std::size_t size;
    };
    // Writer only (P5). Every region, in no particular order. O(chunks).
    [[nodiscard]] std::vector<Region> regions() const;
    // Writer only. Bytes held: the original plus every add chunk's capacity.
    [[nodiscard]] std::size_t bytesHeld() const noexcept;

private:
    struct Chunk {
        std::unique_ptr<char[]> data;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };
    static constexpr std::size_t kNoChunk = static_cast<std::size_t>(-1);

    TextBufferOptions m_options;
    std::unique_ptr<std::string> m_original;
    std::vector<Chunk> m_addChunks;
    // The write cursor: the small chunk being filled (its `used` is the offset), or kNoChunk.
    std::size_t m_cursorChunk = kNoChunk;
};

} // namespace frost::text::detail
