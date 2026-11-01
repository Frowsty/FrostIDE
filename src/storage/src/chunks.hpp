// SPDX-License-Identifier: Apache-2.0
//
// Private: chunked content storage shared by the hot-exit journal and local history. Content
// is written as rows (owner, seq, data) of at most chunkBytes each, with an XXH3-128 checksum
// kept beside it; reads stream the rows in seq order and verify count, size and checksum.
#pragma once

#include "frost/compat/function_ref.hpp"
#include "frost/core/error.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/storage/statement.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string_view>

namespace frost::storage::detail {

// What was written, or what a reader expects.
struct ChunkSummary {
    std::uint64_t size = 0;
    std::int64_t chunkCount = 0;
    Fingerprint128 fingerprint{};
};

/*
 * Writes the concatenation of `pieces` through `insert`, a prepared INSERT whose parameter
 * `seqIndex` takes the sequence number and `dataIndex` the bytes (any owner parameter is bound by
 * the caller; bindings survive the resets). Pieces are copied into one staging buffer of
 * chunkBytes, except that a whole chunk lying inside one piece is bound in place (the pieces
 * outlive the call). Rows are written in seq order from 0. A stop request between chunks returns
 * Cancelled; the caller rolls its transaction back. O(total size) time, O(chunkBytes) memory.
 * Errors: those of the insert, Cancelled.
 */
[[nodiscard]] Result<ChunkSummary> writeChunks(Statement& insert, int seqIndex, int dataIndex,
                                               std::span<const std::string_view> pieces, std::size_t chunkBytes,
                                               const std::stop_token& stop);

/*
 * Streams the rows of `select` (columns: seq, data; ordered by seq; bound by the caller) to
 * `sink`, checking that seq runs 0, 1, 2, ... and that count, size and XXH3-128 match `expected`.
 * Corrupt on any mismatch (the sink must discard what it received); a sink error stops the read
 * and is returned. A stop request between chunks returns Cancelled. O(size) time, O(chunk) memory.
 */
[[nodiscard]] Result<void> readChunks(Statement& select, const ChunkSummary& expected,
                                      frost::function_ref<Result<void>(std::string_view)> sink,
                                      const std::stop_token& stop);

/*
 * Whether the stored rows of `select` (as for readChunks) hold exactly the concatenation of
 * `pieces`, comparing bytes as they stream; a gap in seq, a short or long blob is simply "not
 * equal". The stored checksum is not consulted (the caller compared it already). Errors: those
 * of the select, Cancelled.
 */
[[nodiscard]] Result<bool> chunksEqual(Statement& select, std::int64_t expectedChunks,
                                       std::span<const std::string_view> pieces, const std::stop_token& stop);

// The total size of `pieces`. O(pieces).
[[nodiscard]] std::uint64_t totalSize(std::span<const std::string_view> pieces) noexcept;

} // namespace frost::storage::detail
