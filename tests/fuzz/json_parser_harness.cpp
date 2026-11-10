// SPDX-License-Identifier: Apache-2.0
//
// Fuzz harness for frost::rpc::Json and parseJson (FND-14, PLAN.md Appendix C):
//   - parses arbitrary byte sequences with parseJson();
//   - walks parsed Json trees exercising all type predicates and accessors;
//   - round-trips every successfully parsed Json through dump() -> parseJson() and checks
//     that the re-serialized text is byte-for-byte identical;
//   - exercises exact 64-bit signed and unsigned integer round-tripping (including handles
//     above 2^53 up to UINT64_MAX, FEC-1).

#include "frost/rpc/json.hpp"

#include "fuzz/fuzz_harness.hpp"
#include "support/crash_dialogs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace frost::fuzz {
namespace {

constexpr int kWalkMaxDepth = 64;

void inspectJsonTree(const rpc::Json& node, int depth) {
    if (depth > kWalkMaxDepth) {
        return;
    }
    (void)node.kind();
    (void)node.isNull();
    (void)node.isBool();
    (void)node.isNumber();
    (void)node.isString();
    (void)node.isArray();
    (void)node.isObject();
    (void)node.asBool();
    (void)node.asInteger();
    (void)node.asUnsigned();
    (void)node.asDouble();
    (void)node.asString();
    (void)node.size();

    if (node.isArray()) {
        for (const rpc::Json& item : node.items()) {
            inspectJsonTree(item, depth + 1);
        }
    } else if (node.isObject()) {
        (void)node.find("__frost_missing_key__");
        (void)node.stringAt("__frost_missing_key__", "fallback");
        (void)node.integerAt("__frost_missing_key__");
        (void)node.unsignedAt("__frost_missing_key__");
        (void)node.boolAt("__frost_missing_key__");
        const auto& keys = node.keys();
        const auto& items = node.items();
        const std::size_t count = std::min<std::size_t>(keys.size(), 32);
        for (std::size_t i = 0; i < count; ++i) {
            const std::string& key = keys[i];
            (void)node.find(key);
            (void)node.stringAt(key);
            (void)node.integerAt(key);
            (void)node.unsignedAt(key);
            (void)node.boolAt(key);
            inspectJsonTree(items[i], depth + 1);
        }
    }
}

} // namespace

void initialize() { frost::test::disableCrashDialogs(); }

Result<void> runInput(std::span<const std::uint8_t> data) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());

    if (auto parsed = rpc::parseJson(text)) {
        inspectJsonTree(*parsed, 0);
        const std::string dumped = parsed->dump();
        auto reparsed = rpc::parseJson(dumped);
        if (!reparsed) {
            return fail(ErrorCode::Internal, "parseJson failed on dump() output");
        }
        if (reparsed->dump() != dumped) {
            return fail(ErrorCode::Internal, "parseJson(dump())->dump() was not idempotent");
        }
    }

    if (data.size() >= 16) {
        std::uint64_t rawU64 = 0;
        std::int64_t rawI64 = 0;
        std::memcpy(&rawU64, data.data(), sizeof(rawU64));
        std::memcpy(&rawI64, data.data() + sizeof(rawU64), sizeof(rawI64));

        const std::size_t tailLen = std::min<std::size_t>(data.size() - 16, 64);
        std::string tail(reinterpret_cast<const char*>(data.data() + 16), tailLen);

        rpc::Json synthetic = rpc::Json::object()
                                  .set("u64", rawU64)
                                  .set("i64", rawI64)
                                  .set("flag", (rawU64 & 1U) != 0U)
                                  .set("str", std::move(tail));
        const std::string encoded = synthetic.dump();
        auto decoded = rpc::parseJson(encoded);
        if (!decoded) {
            return fail(ErrorCode::Internal, "parseJson failed on synthetic object dump()");
        }
        if (decoded->unsignedAt("u64") != rawU64) {
            return fail(ErrorCode::Internal, "uint64 round-trip mismatch");
        }
        if (decoded->integerAt("i64") != rawI64) {
            return fail(ErrorCode::Internal, "int64 round-trip mismatch");
        }
    }
    return {};
}

} // namespace frost::fuzz
