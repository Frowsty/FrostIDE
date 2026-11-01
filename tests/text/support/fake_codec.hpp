// SPDX-License-Identifier: Apache-2.0
//
// A table-driven ITextCodec double for frost::text's encoding tests and the text_decode fuzz
// target (ADR 0018): FakeCodePage (a code page as explicit units), FakeCodecProvider (serves pages
// under code page ids, with fault knobs and call counters) and random page generators.
/*
 * The fake follows ITextCodec's contract exactly like the platform codecs: decodePrefix converts
 * the longest prefix of complete units the table defines (a lead byte consumes the next byte as
 * its trail; a lead byte at the very end stops), encodePrefix the longest prefix whose scalars
 * have a mapping. The encode map is derived from the units (the first unit wins for a scalar that
 * several units decode to, which makes those loads NotRoundTrip) plus encode-only "one-way"
 * entries that test encodeLegacy's reversible filter. A lead byte never decodes alone, so every
 * fake page is prefix-free, as the M1 code pages are.
 *
 * Not thread-safe: a provider and its codecs belong to one thread. A codec must not outlive its
 * provider (it refers to the provider's pages, ids and knobs).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/encoding.hpp"
#include "frost/text/text_codec.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text::test {

// One legacy code page as explicit units. Movable; the CodePageInfo spans point into owned heap
// storage, so they stay valid when the page moves.
class FakeCodePage {
public:
    struct Unit {
        std::string bytes; // 1 byte (a non-lead byte) or 2 bytes (lead, trail).
        char32_t scalar = 0;
    };

    // Precondition: every 1-byte unit is a non-lead byte, every 2-byte unit starts with a lead
    // byte; `windowsOnlyC1` is sorted. Duplicate byte sequences keep the first unit.
    FakeCodePage(std::string name, std::vector<LeadByteRange> leads, std::vector<std::uint8_t> windowsOnlyC1,
                 std::vector<Unit> units);
    FakeCodePage(FakeCodePage&&) noexcept;
    FakeCodePage& operator=(FakeCodePage&&) noexcept;
    ~FakeCodePage();

    // The page's info (codePage 0; providers report the id they serve it under).
    [[nodiscard]] const CodePageInfo& info() const noexcept;
    [[nodiscard]] bool isLead(std::uint8_t byte) const noexcept;
    // The scalar of a 1-byte unit, or of a (lead, trail) pair.
    [[nodiscard]] std::optional<char32_t> single(std::uint8_t byte) const noexcept;
    [[nodiscard]] std::optional<char32_t> pair(std::uint8_t lead, std::uint8_t trail) const noexcept;
    // The bytes encodePrefix produces for `scalar`: the first unit that decodes to it, else a
    // one-way entry, else nullopt.
    [[nodiscard]] std::optional<std::string> encode(char32_t scalar) const;
    // Adds an encode-only mapping (its bytes need not decode back to `scalar`).
    void addOneWay(char32_t scalar, std::string bytes);
    [[nodiscard]] const std::vector<Unit>& units() const noexcept;

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};

// A random single-byte page: ASCII 00..7F as itself, then 80..FF with some undefined bytes,
// some windowsOnlyC1 bytes in 80..9F (undefined, as glibc has them, or defined as U+00bb, as
// Windows has them) and scalars from several scripts (2-, 3- and 4-byte UTF-8, never C1 or
// surrogates).
[[nodiscard]] FakeCodePage randomSbcs(sched::detail::SplitMix64& rng);
// A random double-byte page: randomSbcs's single bytes outside 1-3 lead ranges in 81..FE, pairs
// for trails 40..FE with some undefined pairs, valid and invalid ASCII trails, and a few duplicate
// scalars (NotRoundTrip).
[[nodiscard]] FakeCodePage randomDbcs(sched::detail::SplitMix64& rng);

class FakeCodecProvider final : public ITextCodecProvider {
public:
    FakeCodecProvider();
    ~FakeCodecProvider();
    FakeCodecProvider(const FakeCodecProvider&) = delete ("codecs refer to the provider");
    FakeCodecProvider& operator=(const FakeCodecProvider&) = delete ("codecs refer to the provider");

    // Serves `page` under `codePage`: open(codePage) succeeds (whether or not the id is in the M1
    // table) and the codec's info() is the page's info with codePage set to the id.
    void serve(std::shared_ptr<const FakeCodePage> page, std::uint16_t codePage);

    // Fault knobs. failOpen makes open(codePage) fail with `code`. failDecodeAfter(calls) lets
    // `calls` decodePrefix calls (counted from now, over all codecs) succeed and fails every later
    // one with `code`; failEncodeAfter does the same for encodePrefix. overconsume makes
    // decodePrefix report one byte more than it was given; illFormedOutput makes it append C0 80.
    void failOpen(std::uint16_t codePage, ErrorCode code);
    void failDecodeAfter(std::uint64_t calls, ErrorCode code);
    void failEncodeAfter(std::uint64_t calls, ErrorCode code);
    void setOverconsume(bool on) noexcept;
    void setIllFormedOutput(bool on) noexcept;

    // Calls made by every codec of this provider since construction or resetCounts().
    [[nodiscard]] std::uint64_t decodeCalls() const noexcept;
    [[nodiscard]] std::uint64_t encodeCalls() const noexcept;
    [[nodiscard]] std::uint64_t opens() const noexcept;
    void resetCounts() noexcept;

    [[nodiscard]] Result<std::unique_ptr<ITextCodec>> open(std::uint16_t codePage) noexcept override;

    // The state codecs share with their provider. Not part of the interface.
    struct Shared;

private:
    std::unique_ptr<Shared> m_shared;
};

} // namespace frost::text::test
