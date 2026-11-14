// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for frost::rpc: the Content-Length codec (encodeFrame, FrameDecoder on split, joined,
// malformed and oversized input) and the Json value (parse, dump, lookups), plus decoders used on
// several threads at once, which the WSL quick leg runs under TSan.

#include "frost/rpc/framing.hpp"
#include "frost/rpc/json.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using frost::rpc::encodeFrame;
using frost::rpc::FrameDecoder;
using frost::rpc::Json;
using frost::rpc::parseJson;

// Every payload the decoder has ready, in order; a decode error fails the test.
std::vector<std::string> drain(FrameDecoder& decoder) {
    std::vector<std::string> out;
    while (auto next = decoder.next()) {
        EXPECT_TRUE(next->has_value()) << next->error().message();
        if (next->has_value()) {
            out.push_back(**next);
        }
    }
    return out;
}

TEST(RpcFraming, EncodesTheByteLength) {
    EXPECT_EQ(encodeFrame("{}"), "Content-Length: 2\r\n\r\n{}");
    // The length counts UTF-8 bytes, not characters ("\xc3\xa9" is one character).
    EXPECT_EQ(encodeFrame("\"\xc3\xa9\""), "Content-Length: 4\r\n\r\n\"\xc3\xa9\"");
}

TEST(RpcFraming, DecodesSplitAndJoinedFrames) {
    const std::string stream = encodeFrame(R"({"a":1})") + encodeFrame(R"({"b":2})");
    // One byte at a time.
    FrameDecoder split;
    std::vector<std::string> got;
    for (const char c : stream) {
        split.feed(std::string_view(&c, 1));
        for (auto& payload : drain(split)) {
            got.push_back(std::move(payload));
        }
    }
    EXPECT_EQ(got, (std::vector<std::string>{R"({"a":1})", R"({"b":2})"}));
    EXPECT_EQ(split.buffered(), 0U);
    // Both frames in one read, with a Content-Type header the decoder ignores.
    FrameDecoder joined;
    joined.feed("Content-Type: application/vscode-jsonrpc; charset=utf-8\r\ncontent-length: 2\r\n\r\n{}" + stream);
    EXPECT_EQ(drain(joined), (std::vector<std::string>{"{}", R"({"a":1})", R"({"b":2})"}));
}

TEST(RpcFraming, AMalformedHeaderIsOneErrorAndTheNextFrameStillDecodes) {
    FrameDecoder decoder;
    decoder.feed("X-Nothing: 1\r\n\r\n" + encodeFrame("[1]"));
    auto first = decoder.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(first->has_value());
    auto second = decoder.next();
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->has_value());
    EXPECT_EQ(**second, "[1]");
    EXPECT_FALSE(decoder.next().has_value());
}

TEST(RpcFraming, AnOversizedFrameIsSkippedAndReported) {
    FrameDecoder decoder(8);
    decoder.feed(encodeFrame(R"({"too":"large"})") + encodeFrame("{}"));
    auto first = decoder.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(first->has_value());
    EXPECT_EQ(drain(decoder), (std::vector<std::string>{"{}"}));
}

TEST(RpcJson, ParsesDumpsAndLooksUpMembers) {
    auto parsed = parseJson(R"({"seq":3,"type":"event","body":{"ok":true,"items":[1,2.5,null,"x"]}})");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    const Json& value = *parsed;
    EXPECT_TRUE(value.isObject());
    EXPECT_EQ(value.integerAt("seq"), 3);
    EXPECT_EQ(value.stringAt("type"), "event");
    const Json* body = value.find("body");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->boolAt("ok"), true);
    const Json* items = body->find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->size(), 4U);
    EXPECT_EQ(items->items()[1].asDouble(), 2.5);
    EXPECT_TRUE(items->items()[2].isNull());
    // dump() is compact and keeps the document order, so a parse of it is equal.
    EXPECT_EQ(value.dump(), R"({"seq":3,"type":"event","body":{"ok":true,"items":[1,2.5,null,"x"]}})");
    auto again = parseJson(value.dump());
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, value);
}

TEST(RpcJson, BuildsValuesAndRejectsBadInput) {
    Json request = Json::object();
    request.set("command", "threads").set("seq", 1).set("arguments", Json::array({Json(true), Json(nullptr)}));
    request.set("seq", 2); // Replaces in place.
    EXPECT_EQ(request.dump(), R"({"command":"threads","seq":2,"arguments":[true,null]})");
    EXPECT_FALSE(parseJson(R"({"a":)").has_value());
    EXPECT_FALSE(parseJson("").has_value());
}

TEST(RpcFraming, DecodersOnSeveralThreadsAreIndependent) {
    // One decoder per thread (the reader-thread pattern of both clients); TSan checks the codec
    // shares no hidden state.
    constexpr int kThreads = 4;
    constexpr int kFrames = 200;
    std::vector<int> decoded(kThreads, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &decoded] {
            FrameDecoder decoder;
            for (int i = 0; i < kFrames; ++i) {
                Json message = Json::object();
                message.set("thread", t).set("seq", i);
                decoder.feed(encodeFrame(message.dump()));
                while (auto next = decoder.next()) {
                    if (next->has_value() && parseJson(**next).has_value()) {
                        ++decoded[static_cast<std::size_t>(t)];
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(decoded, std::vector<int>(kThreads, kFrames));
}

// FEC-1: integers above 2^53 (FrostEngine's 64-bit handles and entity ids) keep every digit through
// parse and dump. Before the fix, values above INT64_MAX became doubles and lost their low bits.
TEST(RpcJson, IntegersAbove2To53RoundTripExactly) {
    const std::string text =
        R"({"a":9007199254740993,"b":16273909378015378282,"c":18446744073709551615,"d":-9223372036854775808})";
    auto parsed = parseJson(text);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    EXPECT_EQ(parsed->dump(), text);
    EXPECT_EQ(parsed->integerAt("a"), 9007199254740993LL);
    EXPECT_EQ(parsed->unsignedAt("a"), 9007199254740993ULL);
    // Above INT64_MAX: an exact unsigned integer, not an int64.
    EXPECT_EQ(parsed->find("b")->kind(), Json::Kind::Integer);
    EXPECT_EQ(parsed->unsignedAt("b"), 16273909378015378282ULL);
    EXPECT_EQ(parsed->integerAt("b"), std::nullopt);
    EXPECT_EQ(parsed->unsignedAt("c"), 18446744073709551615ULL);
    EXPECT_EQ(parsed->integerAt("d"), INT64_MIN);
    EXPECT_EQ(parsed->unsignedAt("d"), std::nullopt);
    // Neighbouring handles stay distinct (as doubles they compare equal).
    EXPECT_NE(Json(16273909378015378282ULL), Json(16273909378015378283ULL));
    EXPECT_EQ(Json(16273909378015378282ULL), parsed->find("b") != nullptr ? *parsed->find("b") : Json());
    EXPECT_EQ(Json(std::uint64_t{18446744073709551615ULL}).dump(), "18446744073709551615");
    // A handle built from a parsed value and written into a request is the same text.
    const Json request = Json::object().set("entity", *parsed->unsignedAt("b"));
    EXPECT_EQ(request.dump(), R"({"entity":16273909378015378282})");
    // The integer kinds do not mix up: an unsigned value is not the int64 with the same bits.
    EXPECT_NE(Json(18446744073709551615ULL), Json(-1));
    // Beyond uint64 a number is a double; exact doubles still read as integers.
    auto beyond = parseJson("[18446744073709551616, 4.0, -1.5]");
    ASSERT_TRUE(beyond.has_value());
    EXPECT_EQ(beyond->items()[0].kind(), Json::Kind::Double);
    EXPECT_EQ(beyond->items()[0].asUnsigned(), std::nullopt);
    EXPECT_EQ(beyond->items()[1].asUnsigned(), 4U);
    EXPECT_EQ(beyond->items()[2].asUnsigned(), std::nullopt);
    EXPECT_EQ(Json(5U).asInteger(), 5);
    EXPECT_EQ(Json(-5).asUnsigned(), std::nullopt);
}

TEST(JsonComments, StringsAndByteRangesSurviveCommentAndTrailingCommaNormalization) {
    const std::string source = R"json({/* comment */"url":"https://example/a//b", "escaped":"quote\"//still-string", "list":[1,2,],})json";
    const auto normalized = frost::rpc::normalizeJsonComments(source);
    ASSERT_TRUE(normalized);
    EXPECT_EQ(normalized->size(), source.size());
    const auto parsed = frost::rpc::parseJson(*normalized);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->stringAt("url"), "https://example/a//b");
    EXPECT_EQ(parsed->find("list")->size(), 2U);
    EXPECT_FALSE(frost::rpc::normalizeJsonComments("{/* unterminated"));
}

} // namespace
