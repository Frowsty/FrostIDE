// SPDX-License-Identifier: Apache-2.0
// Strict Content-Length parsing shared by Lua DAP authentication and its direct transport.
#include "lua_dap_wire.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>

namespace frost::debug::detail {
Result<std::size_t> luaDapContentLength(std::string_view header) {
    if (header.size() > kLuaDapHeaderLimit || !header.ends_with("\r\n\r\n"))
        return fail(ErrorCode::Corrupt, "invalid Lua DAP header");
    for (const char byte : header) {
        const auto c = static_cast<unsigned char>(byte);
        if ((c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c > 0x7e)
            return fail(ErrorCode::Corrupt, "invalid Lua DAP header character");
    }
    std::optional<std::size_t> length;
    while (header != "\r\n") {
        const auto end = header.find("\r\n");
        if (end == std::string_view::npos)
            return fail(ErrorCode::Corrupt, "invalid Lua DAP header line");
        auto line = header.substr(0, end);
        header.remove_prefix(end + 2);
        if (line.find_first_of("\r\n") != std::string_view::npos)
            return fail(ErrorCode::Corrupt, "invalid Lua DAP header line break");
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
            return fail(ErrorCode::Corrupt, "invalid Lua DAP header field");
        const auto name = line.substr(0, colon);
        constexpr std::string_view expected = "content-length";
        const bool isLength = std::ranges::equal(name, expected, [](char a, char b) {
            return (a >= 'A' && a <= 'Z' ? static_cast<char>(a + ('a' - 'A')) : a) == b;
        });
        if (!isLength)
            continue;
        if (length)
            return fail(ErrorCode::Corrupt, "duplicate Lua DAP content length");
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.remove_suffix(1);
        if (value.empty())
            return fail(ErrorCode::Corrupt, "empty Lua DAP content length");
        std::uint64_t parsed = 0;
        const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (value.empty() || conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size())
            return fail(ErrorCode::Corrupt, "invalid Lua DAP content length");
        if (parsed == 0 || parsed > kLuaDapPayloadLimit)
            return fail(ErrorCode::OutOfRange, "Lua DAP payload exceeds its limit");
        length = static_cast<std::size_t>(parsed);
    }
    if (!length)
        return fail(ErrorCode::Corrupt, "missing Lua DAP content length");
    return *length;
}
Result<std::string> readLuaDapFrame(io::LoopbackStream& stream) {
    std::string header;
    std::array<char, 1> byte{};
    while (!header.ends_with("\r\n\r\n")) {
        if (header.size() == kLuaDapHeaderLimit)
            return fail(ErrorCode::OutOfRange, "Lua DAP header exceeds its limit");
        auto read = stream.read(byte);
        if (!read)
            return std::unexpected(read.error());
        if (*read == 0)
            return fail(ErrorCode::IoError, "Lua DAP connection closed");
        header.append(byte.data(), *read);
    }
    auto length = luaDapContentLength(header);
    if (!length)
        return std::unexpected(length.error());
    std::string body(*length, '\0');
    std::size_t offset = 0;
    while (offset < body.size()) {
        auto read = stream.read(std::span<char>(body.data() + offset, body.size() - offset));
        if (!read)
            return std::unexpected(read.error());
        if (*read == 0)
            return fail(ErrorCode::IoError, "Lua DAP connection closed inside a frame");
        offset += *read;
    }
    return body;
}
Result<void> LuaDapFrameDecoder::feed(std::string_view bytes) {
    if (bytes.size() > kLuaDapPayloadLimit + kLuaDapHeaderLimit ||
        m_buffer.size() > kLuaDapPayloadLimit + kLuaDapHeaderLimit - bytes.size())
        return fail(ErrorCode::OutOfRange, "Lua DAP outgoing frame exceeds its limit");
    m_buffer.append(bytes);
    return {};
}
std::optional<Result<std::string>> LuaDapFrameDecoder::next() {
    const auto end = m_buffer.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (m_buffer.size() >= kLuaDapHeaderLimit)
            return fail(ErrorCode::OutOfRange, "Lua DAP header exceeds its limit");
        return std::nullopt;
    }
    auto length = luaDapContentLength(std::string_view(m_buffer).substr(0, end + 4));
    if (!length)
        return std::unexpected(length.error());
    if (m_buffer.size() - (end + 4) < *length)
        return std::nullopt;
    std::string body = m_buffer.substr(end + 4, *length);
    m_buffer.erase(0, end + 4 + *length);
    return body;
}
} // namespace frost::debug::detail
