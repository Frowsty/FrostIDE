// SPDX-License-Identifier: Apache-2.0
//
// frost::text's append-only byte storage; see storage.hpp for the layout and the concurrency rules.

#include "storage.hpp"

#include "frost/core/assert.hpp"

#include <cstdint>
#include <cstring>
#include <utility>

namespace frost::text::detail {

Storage::Storage(TextBufferOptions options) noexcept
    : m_options(options), m_original(std::make_unique<std::string>()) {}

Storage::Storage(TextBufferOptions options, std::string original) noexcept
    : m_options(options), m_original(std::make_unique<std::string>(std::move(original))) {}

const char* Storage::append(std::string_view text) noexcept {
    FROST_PRE(!text.empty());
    const std::size_t size = text.size();
    if (size >= kDedicatedChunkBytes) {
        // A dedicated exact-size chunk; the write cursor stays where it was.
        Chunk chunk{std::make_unique_for_overwrite<char[]>(size), size, size};
        std::memcpy(chunk.data.get(), text.data(), size);
        const char* start = chunk.data.get();
        m_addChunks.push_back(std::move(chunk));
        return start;
    }
    if (m_cursorChunk == kNoChunk || m_addChunks[m_cursorChunk].capacity - m_addChunks[m_cursorChunk].used < size) {
        m_addChunks.push_back(Chunk{std::make_unique_for_overwrite<char[]>(kAddChunkBytes), kAddChunkBytes, 0});
        m_cursorChunk = m_addChunks.size() - 1;
    }
    Chunk& chunk = m_addChunks[m_cursorChunk];
    // The destination lies past every published byte; `text` (if it points into this storage)
    // lies before it, so the ranges never overlap.
    char* start = chunk.data.get() + chunk.used;
    std::memcpy(start, text.data(), size);
    chunk.used += size;
    return start;
}

bool Storage::endsAtCursor(const char* data, std::size_t size) const noexcept {
    if (m_cursorChunk == kNoChunk) {
        return false;
    }
    const Chunk& chunk = m_addChunks[m_cursorChunk];
    if (chunk.used == 0) {
        return false;
    }
    // Integer comparisons within one chunk: `data` must lie inside it and end at its cursor.
    const auto base = reinterpret_cast<std::uintptr_t>(chunk.data.get());
    const auto begin = reinterpret_cast<std::uintptr_t>(data);
    return begin >= base && begin + size == base + chunk.used;
}

std::size_t Storage::cursorRoom() const noexcept {
    if (m_cursorChunk == kNoChunk) {
        return 0;
    }
    const Chunk& chunk = m_addChunks[m_cursorChunk];
    return chunk.capacity - chunk.used;
}

std::vector<Storage::Region> Storage::regions() const {
    std::vector<Region> out;
    out.reserve(m_addChunks.size() + 1);
    out.push_back({m_original->data(), m_original->size()});
    for (const Chunk& chunk : m_addChunks) {
        out.push_back({chunk.data.get(), chunk.used});
    }
    return out;
}

std::size_t Storage::bytesHeld() const noexcept {
    std::size_t total = m_original->size();
    for (const Chunk& chunk : m_addChunks) {
        total += chunk.capacity;
    }
    return total;
}

} // namespace frost::text::detail
