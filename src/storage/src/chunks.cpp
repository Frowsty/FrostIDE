// SPDX-License-Identifier: Apache-2.0
//
// Private: chunked content writer, reader and comparer (see chunks.hpp).

#include "chunks.hpp"

#include "frost/core/assert.hpp"

#include "test_hooks.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <utility>

namespace frost::storage::detail {

namespace {

Result<void> cancelled() { return fail(ErrorCode::Cancelled, "cancelled between chunks"); }

} // namespace

std::uint64_t totalSize(std::span<const std::string_view> pieces) noexcept {
    std::uint64_t size = 0;
    for (const std::string_view piece : pieces) {
        size += piece.size();
    }
    return size;
}

Result<ChunkSummary> writeChunks(Statement& insert, int seqIndex, int dataIndex,
                                 std::span<const std::string_view> pieces, std::size_t chunkBytes,
                                 const std::stop_token& stop) {
    FROST_PRE(chunkBytes > 0);
    ChunkSummary summary;
    Xxh3Stream128 hash;
    std::unique_ptr<char[]> staging; // Allocated on first use: content in whole chunks needs none.
    std::size_t staged = 0;

    auto emit = [&](std::string_view bytes) -> Result<void> {
        if (stop.stop_requested()) {
            return cancelled();
        }
        if (auto bound = insert.bindInt64(seqIndex, summary.chunkCount); !bound) {
            return bound;
        }
        if (auto bound = insert.bindBlobNoCopy(dataIndex, bytes); !bound) {
            return bound;
        }
        if (auto ran = insert.run(); !ran) {
            return ran;
        }
        ++summary.chunkCount;
        g_afterChunkWritten.run();
        return {};
    };

    for (std::string_view piece : pieces) {
        hash.update(piece);
        summary.size += piece.size();
        while (!piece.empty()) {
            if (staged == 0 && piece.size() >= chunkBytes) {
                // A whole chunk inside one piece: bind it in place.
                if (auto written = emit(piece.substr(0, chunkBytes)); !written) {
                    return std::unexpected(std::move(written.error()));
                }
                piece.remove_prefix(chunkBytes);
                continue;
            }
            if (!staging) {
                staging = std::make_unique<char[]>(chunkBytes);
            }
            const std::size_t take = std::min(piece.size(), chunkBytes - staged);
            std::memcpy(staging.get() + staged, piece.data(), take);
            staged += take;
            piece.remove_prefix(take);
            if (staged == chunkBytes) {
                if (auto written = emit({staging.get(), staged}); !written) {
                    return std::unexpected(std::move(written.error()));
                }
                staged = 0;
            }
        }
    }
    if (staged > 0) {
        if (auto written = emit({staging.get(), staged}); !written) {
            return std::unexpected(std::move(written.error()));
        }
    }
    summary.fingerprint = hash.digest();
    return summary;
}

Result<void> readChunks(Statement& select, const ChunkSummary& expected,
                        frost::function_ref<Result<void>(std::string_view)> sink, const std::stop_token& stop) {
    Xxh3Stream128 hash;
    std::uint64_t size = 0;
    std::int64_t count = 0;
    while (true) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        auto row = select.step();
        if (!row) {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row) {
            break;
        }
        const std::int64_t seq = select.columnInt64(0);
        const std::string_view data = select.columnBlob(1);
        if (seq != count) {
            return fail(ErrorCode::Corrupt, std::format("chunk {} found where chunk {} was expected", seq, count));
        }
        if (count >= expected.chunkCount || data.size() > expected.size - size) {
            return fail(ErrorCode::Corrupt, std::format("content longer than its recorded {} bytes in {} chunks",
                                                        expected.size, expected.chunkCount));
        }
        hash.update(data);
        size += data.size();
        ++count;
        if (auto taken = sink(data); !taken) {
            return taken;
        }
    }
    if (count != expected.chunkCount || size != expected.size) {
        return fail(ErrorCode::Corrupt, std::format("content has {} bytes in {} chunks; {} bytes in {} recorded", size,
                                                    count, expected.size, expected.chunkCount));
    }
    if (hash.digest() != expected.fingerprint) {
        return fail(ErrorCode::Corrupt, "content checksum mismatch (XXH3-128)");
    }
    return {};
}

Result<bool> chunksEqual(Statement& select, std::int64_t expectedChunks, std::span<const std::string_view> pieces,
                         const std::stop_token& stop) {
    std::size_t piece = 0;
    std::size_t offset = 0; // Within pieces[piece].
    std::int64_t count = 0;
    while (true) {
        if (stop.stop_requested()) {
            return std::unexpected(cancelled().error());
        }
        auto row = select.step();
        if (!row) {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row) {
            break;
        }
        if (select.columnInt64(0) != count || count >= expectedChunks) {
            return false;
        }
        ++count;
        std::string_view data = select.columnBlob(1);
        while (!data.empty()) {
            while (piece < pieces.size() && offset == pieces[piece].size()) {
                ++piece;
                offset = 0;
            }
            if (piece == pieces.size()) {
                return false; // Stored content is longer.
            }
            const std::size_t take = std::min(data.size(), pieces[piece].size() - offset);
            if (std::memcmp(data.data(), pieces[piece].data() + offset, take) != 0) {
                return false;
            }
            data.remove_prefix(take);
            offset += take;
        }
    }
    while (piece < pieces.size() && offset == pieces[piece].size()) {
        ++piece;
        offset = 0;
    }
    return piece == pieces.size() && count == expectedChunks;
}

} // namespace frost::storage::detail
