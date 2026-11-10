// SPDX-License-Identifier: Apache-2.0
//
// FrostEngine's 64-bit ids on the IDE side (FEC-1): asset handles, entity ids and every other
// engine UUID. The engine prints them as JSON integers (up to 2^64 - 1), or as decimal strings when
// a client asked for safe integers (FrostEditor --safe-integers, server.configure safeIntegers),
// and accepts the integer, the decimal string or a 0x-hex string wherever a UUID parameter is
// declared. A double holds only 53 bits, so a handle must never pass through one: these helpers
// read and write the exact value (frost::rpc::Json keeps integers above 2^53 exactly).
#pragma once

#include "frost/rpc/json.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace frost::engine {

// An engine id. A value type; 0 is the engine's "no id" (UUID 0), still a valid value here.
struct EngineHandle {
    std::uint64_t value = 0;
    friend bool operator==(EngineHandle, EngineHandle) noexcept = default;
};

/*
 * The handle a JSON value holds: a non-negative integer (exact up to 2^64 - 1), or a decimal or
 * 0x-hex string of one (the engine's safe-integer form). nullopt for anything else: negative
 * numbers, fractions, doubles (precision may already be lost; the engine never prints one), text
 * that is not a whole number, or a value beyond 2^64 - 1. Pure; O(length).
 */
[[nodiscard]] std::optional<EngineHandle> engineHandleFrom(const rpc::Json& value) noexcept;

// engineHandleFrom of member `key`; nullopt when the member is missing.
[[nodiscard]] std::optional<EngineHandle> engineHandleAt(const rpc::Json& object, std::string_view key) noexcept;

// The JSON integer the engine reads back as the same handle (exact for every value). Pure.
[[nodiscard]] rpc::Json toJson(EngineHandle handle);

} // namespace frost::engine
