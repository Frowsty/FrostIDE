// SPDX-License-Identifier: Apache-2.0
//
// Content-Length framing (frost/rpc/framing.hpp): encodeFrame and FrameDecoder, shared by the LSP and
// DAP clients (ADR 0050).

#include "frost/rpc/framing.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace frost::rpc {
namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::string_view kHeaderEnd = "\r\n\r\n";
constexpr std::string_view kLengthName = "content-length";

[[nodiscard]] char lowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

[[nodiscard]] std::string_view trimSpaces(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// The position of the next "content-length:" (any case) at or after `from`, or npos.
[[nodiscard]] std::size_t findLengthHeader(std::string_view text, std::size_t from) noexcept {
    for (std::size_t i = from; i + kLengthName.size() < text.size(); ++i) {
        if (iequals(text.substr(i, kLengthName.size()), kLengthName) && text[i + kLengthName.size()] == ':') {
            return i;
        }
    }
    return std::string_view::npos;
}

} // namespace

std::string encodeFrame(std::string_view payload) {
    std::string frame = std::format("Content-Length: {}\r\n\r\n", payload.size());
    frame += payload;
    return frame;
}

void FrameDecoder::feed(std::string_view bytes) {
    if (m_skip > 0) {
        const std::size_t dropped = std::min(m_skip, bytes.size());
        m_skip -= dropped;
        bytes.remove_prefix(dropped);
    }
    m_buffer.append(bytes);
}

void FrameDecoder::compact() {
    if (m_start == m_buffer.size()) {
        m_buffer.clear();
        m_start = 0;
    } else if (m_start > 64 * 1024 && m_start * 2 > m_buffer.size()) {
        m_buffer.erase(0, m_start);
        m_start = 0;
    }
}

void FrameDecoder::resync() {
    const std::string_view rest = std::string_view(m_buffer).substr(m_start);
    const std::size_t at = findLengthHeader(rest, 1);
    if (at != std::string_view::npos) {
        m_start += at;
    } else {
        // Keep only a tail that could begin a "Content-Length:" split across reads.
        constexpr std::string_view kHeader = "content-length:";
        std::size_t keep = std::min(rest.size(), kHeader.size() - 1);
        while (keep > 0 && !iequals(rest.substr(rest.size() - keep), kHeader.substr(0, keep))) {
            --keep;
        }
        m_start = m_buffer.size() - keep;
    }
    compact();
}

std::optional<Result<std::string>> FrameDecoder::next() {
    for (;;) {
        const std::string_view rest = std::string_view(m_buffer).substr(m_start);
        if (rest.empty()) {
            compact();
            return std::nullopt;
        }
        const std::size_t headerEnd = rest.find(kHeaderEnd);
        if (headerEnd == std::string_view::npos) {
            if (rest.size() > kMaxHeaderBytes) {
                resync();
                return Result<std::string>(fail(ErrorCode::Corrupt, "frame header longer than 64 KiB"));
            }
            return std::nullopt;
        }
        // Parse the header lines.
        std::optional<std::size_t> length;
        bool malformed = false;
        std::string reason;
        std::string_view headers = rest.substr(0, headerEnd);
        while (!headers.empty()) {
            const std::size_t lineEnd = headers.find("\r\n");
            const std::string_view line = headers.substr(0, lineEnd);
            headers = lineEnd == std::string_view::npos ? std::string_view() : headers.substr(lineEnd + 2);
            if (line.empty()) {
                continue; // Stray line breaks between frames.
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos) {
                malformed = true;
                reason = std::format("header line without ':' ({} bytes)", line.size());
                break;
            }
            const std::string_view name = trimSpaces(line.substr(0, colon));
            const std::string_view value = trimSpaces(line.substr(colon + 1));
            if (iequals(name, kLengthName)) {
                std::size_t parsed = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (error != std::errc() || end != value.data() + value.size() || value.empty()) {
                    malformed = true;
                    reason = "Content-Length is not a decimal number";
                    break;
                }
                length = parsed;
            }
        }
        if (!malformed && !length) {
            malformed = true;
            reason = "frame without Content-Length";
        }
        if (malformed) {
            resync();
            return Result<std::string>(fail(ErrorCode::Corrupt, "malformed frame header: " + reason));
        }
        const std::size_t bodyStart = headerEnd + kHeaderEnd.size();
        if (*length > m_maxPayload) {
            // Discard the header and as much of the body as is buffered; feed() drops the rest.
            const std::size_t available = rest.size() - bodyStart;
            const std::size_t dropped = std::min(available, *length);
            m_skip = *length - dropped;
            m_start += bodyStart + dropped;
            compact();
            return Result<std::string>(fail(
                ErrorCode::Corrupt, std::format("frame of {} bytes exceeds the {} byte limit", *length, m_maxPayload)));
        }
        if (rest.size() - bodyStart < *length) {
            return std::nullopt;
        }
        std::string payload(rest.substr(bodyStart, *length));
        m_start += bodyStart + *length;
        compact();
        return Result<std::string>(std::move(payload));
    }
}

} // namespace frost::rpc
