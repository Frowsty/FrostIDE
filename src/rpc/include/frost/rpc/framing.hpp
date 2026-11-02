// SPDX-License-Identifier: Apache-2.0
//
// Content-Length framing, the base protocol LSP (LSP 3.17, "Base Protocol") and DAP (DAP 1.71, "Base
// Protocol") share: "Content-Length: N\r\n\r\n" followed by N bytes of UTF-8 JSON. Written from the
// specifications. Moved here from frost::lang (ADR 0050) so the LSP and DAP clients use one codec.
#pragma once

#include "frost/core/error.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace frost::rpc {

// "Content-Length: <size>\r\n\r\n" followed by `payload`. O(size).
[[nodiscard]] std::string encodeFrame(std::string_view payload);

/*
 * Splits a byte stream into frame payloads. Headers are "Name: value" lines ending in "\r\n"
 * (names compared ASCII-case-insensitively), closed by an empty line; Content-Length is required,
 * Content-Type is ignored. Not thread-safe (one reader owns it). Memory is bounded by the largest
 * frame plus 64 KiB of header slack.
 */
class FrameDecoder {
public:
    // Frames larger than `maxPayloadBytes` are skipped and reported.
    explicit FrameDecoder(std::size_t maxPayloadBytes = std::size_t{256} << 20) noexcept
        : m_maxPayload(maxPayloadBytes) {}

    // Appends bytes read from the stream. Amortised O(size).
    void feed(std::string_view bytes);

    /*
     * The next complete payload; nullopt when more bytes are needed. A malformed header (no
     * Content-Length, a non-numeric or oversized length, a header block over 64 KiB) yields one
     * Corrupt error, after which the decoder skips to the next "Content-Length:" it sees.
     */
    [[nodiscard]] std::optional<Result<std::string>> next();

    // Bytes held but not yet returned (tests and diagnostics).
    [[nodiscard]] std::size_t buffered() const noexcept { return m_buffer.size() - m_start; }

private:
    void resync();
    void compact();

    std::string m_buffer;
    std::size_t m_start = 0;
    std::size_t m_skip = 0; // Bytes of an oversized frame still to discard.
    std::size_t m_maxPayload;
};

} // namespace frost::rpc
