// SPDX-License-Identifier: Apache-2.0
//
// frost::engine handles (handle.hpp): the exact reading and writing of FrostEngine's 64-bit ids.

#include "frost/engine/handle.hpp"

#include <charconv>
#include <string>
#include <system_error>

namespace frost::engine {

std::optional<EngineHandle> engineHandleFrom(const rpc::Json& value) noexcept {
    if (value.kind() == rpc::Json::Kind::Integer) {
        const auto number = value.asUnsigned();
        return number ? std::optional<EngineHandle>(EngineHandle{*number}) : std::nullopt;
    }
    const std::string* text = value.asString();
    if (text == nullptr || text->empty()) {
        return std::nullopt;
    }
    // The engine's safe-integer form: decimal, or 0x-hex as its UUID parameters also accept.
    std::string_view digits = *text;
    int base = 10;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits.remove_prefix(2);
        base = 16;
    }
    std::uint64_t number = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number, base);
    if (error != std::errc() || end != digits.data() + digits.size()) {
        return std::nullopt; // Not a whole number, a sign, trailing text, or beyond 2^64 - 1.
    }
    return EngineHandle{number};
}

std::optional<EngineHandle> engineHandleAt(const rpc::Json& object, std::string_view key) noexcept {
    const rpc::Json* member = object.find(key);
    return member != nullptr ? engineHandleFrom(*member) : std::nullopt;
}

rpc::Json toJson(EngineHandle handle) { return rpc::Json(handle.value); }

} // namespace frost::engine
