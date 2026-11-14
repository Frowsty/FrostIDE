// SPDX-License-Identifier: Apache-2.0
// Bounded, command-free DAP address/base64 codecs shared by native requests and source tests.
#pragma once
#include "frost/core/error.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
namespace frost::dbgeng {
inline Result<std::uint64_t> address(std::string_view text, std::int64_t offset = 0) {
    std::uint64_t value = 0;
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return fail(ErrorCode::InvalidArgument, "Memory reference is not a numeric address");
    const auto magnitude = offset < 0 ? std::uint64_t(-(offset + 1)) + 1 : std::uint64_t(offset);
    if ((offset < 0 && magnitude > value) ||
        (offset >= 0 && magnitude > std::numeric_limits<std::uint64_t>::max() - value))
        return fail(ErrorCode::OutOfRange, "Memory address offset overflows");
    return offset < 0 ? value - magnitude : value + magnitude;
}
inline std::string base64(std::string_view bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        result += alphabet[a >> 2];
        result += alphabet[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return result;
}
inline Result<std::string> unbase64(std::string_view text) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (text.size() > 1024 * 1024 * 4 / 3 + 4 || text.size() % 4 != 0)
        return fail(ErrorCode::InvalidArgument, "Invalid bounded base64 payload");
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        unsigned values[4]{};
        unsigned padding = 0;
        for (unsigned j = 0; j < 4; ++j) {
            if (text[i + j] == '=') {
                if (i + 4 != text.size() || j < 2)
                    return fail(ErrorCode::InvalidArgument, "Invalid base64 padding");
                ++padding;
            } else {
                if (padding)
                    return fail(ErrorCode::InvalidArgument, "Invalid base64 padding");
                const auto at = alphabet.find(text[i + j]);
                if (at == alphabet.npos)
                    return fail(ErrorCode::InvalidArgument, "Invalid base64 byte");
                values[j] = static_cast<unsigned>(at);
            }
        }
        if ((padding == 2 && (values[1] & 15)) || (padding == 1 && (values[2] & 3)))
            return fail(ErrorCode::InvalidArgument, "Noncanonical base64 padding");
        bytes += static_cast<char>((values[0] << 2) | (values[1] >> 4));
        if (padding < 2)
            bytes += static_cast<char>((values[1] << 4) | (values[2] >> 2));
        if (padding < 1)
            bytes += static_cast<char>((values[2] << 6) | values[3]);
    }
    return bytes;
}
// Supported DAP hit grammar: N, ==N, >=N, >N, <=N, <N, and %N (positive decimal N).
inline Result<bool> hitMatches(std::string_view text, std::uint64_t hit) {
    if (text.empty())
        return true;
    std::string_view op = "==";
    for (auto candidate : {std::string_view(">="), std::string_view("<="), std::string_view("=="),
                           std::string_view(">"), std::string_view("<"), std::string_view("%")})
        if (text.starts_with(candidate)) {
            op = candidate;
            text.remove_prefix(candidate.size());
            break;
        }
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number == 0)
        return fail(ErrorCode::InvalidArgument, "Hit condition requires a positive decimal integer");
    if (op == ">=")
        return hit >= number;
    if (op == "<=")
        return hit <= number;
    if (op == ">")
        return hit > number;
    if (op == "<")
        return hit < number;
    if (op == "%")
        return hit % number == 0;
    return hit == number;
}
} // namespace frost::dbgeng
