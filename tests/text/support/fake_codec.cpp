// SPDX-License-Identifier: Apache-2.0
//
// The table-driven ITextCodec double and the random page generators (fake_codec.hpp).

#include "text/support/fake_codec.hpp"

#include "text/support/encoding_oracle.hpp"
#include "text/support/text_oracle.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <unordered_map>
#include <utility>

namespace frost::text::test {

struct FakeCodePage::Data {
    std::string name;
    std::vector<LeadByteRange> leads;
    std::vector<std::uint8_t> c1;
    CodePageInfo info;
    std::array<std::optional<char32_t>, 256> single{};
    std::unordered_map<std::uint16_t, char32_t> pairs;
    std::map<char32_t, std::string> encode;
    std::vector<Unit> units;
};

FakeCodePage::FakeCodePage(std::string name, std::vector<LeadByteRange> leads, std::vector<std::uint8_t> windowsOnlyC1,
                           std::vector<Unit> units)
    : m_data(std::make_unique<Data>()) {
    Data& d = *m_data;
    d.name = std::move(name);
    d.leads = std::move(leads);
    d.c1 = std::move(windowsOnlyC1);
    d.info.codePage = 0;
    d.info.name = d.name;
    d.info.iconvName = "";
    d.info.leadBytes = d.leads;
    d.info.windowsOnlyC1 = d.c1;
    for (Unit& unit : units) {
        bool fresh = false;
        if (unit.bytes.size() == 1) {
            const auto b = static_cast<std::uint8_t>(unit.bytes[0]);
            if (!d.single[b]) {
                d.single[b] = unit.scalar;
                fresh = true;
            }
        } else if (unit.bytes.size() == 2) {
            const auto key = static_cast<std::uint16_t>((static_cast<std::uint8_t>(unit.bytes[0]) << 8) |
                                                        static_cast<std::uint8_t>(unit.bytes[1]));
            fresh = d.pairs.emplace(key, unit.scalar).second;
        }
        if (fresh) {
            d.encode.emplace(unit.scalar, unit.bytes); // The first unit for a scalar wins.
            d.units.push_back(std::move(unit));
        }
    }
}

FakeCodePage::FakeCodePage(FakeCodePage&&) noexcept = default;
FakeCodePage& FakeCodePage::operator=(FakeCodePage&&) noexcept = default;
FakeCodePage::~FakeCodePage() = default;

const CodePageInfo& FakeCodePage::info() const noexcept { return m_data->info; }

bool FakeCodePage::isLead(std::uint8_t byte) const noexcept {
    return std::any_of(m_data->leads.begin(), m_data->leads.end(),
                       [byte](const LeadByteRange& r) { return byte >= r.first && byte <= r.last; });
}

std::optional<char32_t> FakeCodePage::single(std::uint8_t byte) const noexcept {
    return isLead(byte) ? std::nullopt : m_data->single[byte];
}

std::optional<char32_t> FakeCodePage::pair(std::uint8_t lead, std::uint8_t trail) const noexcept {
    const auto it = m_data->pairs.find(static_cast<std::uint16_t>((lead << 8) | trail));
    return it != m_data->pairs.end() ? std::optional<char32_t>(it->second) : std::nullopt;
}

std::optional<std::string> FakeCodePage::encode(char32_t scalar) const {
    const auto it = m_data->encode.find(scalar);
    return it != m_data->encode.end() ? std::optional<std::string>(it->second) : std::nullopt;
}

void FakeCodePage::addOneWay(char32_t scalar, std::string bytes) { m_data->encode[scalar] = std::move(bytes); }

const std::vector<FakeCodePage::Unit>& FakeCodePage::units() const noexcept { return m_data->units; }

namespace {

using sched::detail::SplitMix64;

// Scalars for random pages: several scripts and UTF-8 lengths, never C1, surrogates or U+FFFD.
char32_t randomScalar(SplitMix64& rng) {
    struct Range {
        char32_t first;
        char32_t last;
    };
    constexpr Range kRanges[] = {{0xA0, 0x24F},      {0x370, 0x3FF},    {0x400, 0x4FF},   {0x2000, 0x206F},
                                 {0x3000, 0x30FF},   {0x4E00, 0x9FFF},  {0xE000, 0xF8FF}, {0xFF61, 0xFF9F},
                                 {0x1F300, 0x1F64F}, {0x20000, 0x2A6DF}};
    const Range& r = kRanges[rng.below(std::size(kRanges))];
    return static_cast<char32_t>(r.first + rng.below(r.last - r.first + 1));
}

char32_t freshScalar(SplitMix64& rng, std::set<char32_t>& used) {
    while (true) {
        const char32_t c = randomScalar(rng);
        if (used.insert(c).second) {
            return c;
        }
    }
}

struct SingleBytes {
    std::vector<FakeCodePage::Unit> units;
    std::vector<std::uint8_t> c1;
};

// 00..7F as ASCII, then 80..FF outside the lead ranges with undefined bytes and C1 entries.
SingleBytes randomSingles(SplitMix64& rng, const std::vector<LeadByteRange>& leads, std::set<char32_t>& used) {
    SingleBytes out;
    const auto isLead = [&](std::uint32_t b) {
        return std::any_of(leads.begin(), leads.end(),
                           [b](const LeadByteRange& r) { return b >= r.first && b <= r.last; });
    };
    for (std::uint32_t b = 0; b < 0x80; ++b) {
        out.units.push_back({std::string(1, static_cast<char>(b)), static_cast<char32_t>(b)});
    }
    for (std::uint32_t b = 0x80; b <= 0xFF; ++b) {
        if (isLead(b)) {
            continue;
        }
        const std::string bytes(1, static_cast<char>(static_cast<std::uint8_t>(b)));
        if (b <= 0x9F && rng.below(4) == 0) {
            out.c1.push_back(static_cast<std::uint8_t>(b));
            if (rng.below(2) == 0) {
                out.units.push_back({bytes, static_cast<char32_t>(b)}); // Windows' table has U+00bb.
            }
            continue; // Otherwise undefined, as glibc has it.
        }
        if (rng.below(7) == 0) {
            continue; // Undefined.
        }
        out.units.push_back({bytes, freshScalar(rng, used)});
    }
    return out;
}

} // namespace

FakeCodePage randomSbcs(SplitMix64& rng) {
    std::set<char32_t> used;
    SingleBytes singles = randomSingles(rng, {}, used);
    return FakeCodePage("fake-sbcs", {}, std::move(singles.c1), std::move(singles.units));
}

FakeCodePage randomDbcs(SplitMix64& rng) {
    std::set<char32_t> used;
    std::vector<LeadByteRange> leads;
    const std::uint64_t ranges = 1 + rng.below(3);
    std::uint32_t next = 0xA0;
    for (std::uint64_t r = 0; r < ranges; ++r) {
        const auto first = static_cast<std::uint8_t>(next + rng.below(16));
        const auto last = static_cast<std::uint8_t>(std::min<std::uint64_t>(first + rng.below(4), 0xFE));
        leads.push_back({first, last});
        next = last + 2u;
        if (next > 0xF8) {
            break;
        }
    }
    // Sometimes a lead range in 81..9F too, so lead bytes and C1 candidates interleave.
    if (rng.below(2) == 0) {
        const auto first = static_cast<std::uint8_t>(0x81 + rng.below(8));
        leads.insert(leads.begin(), LeadByteRange{first, static_cast<std::uint8_t>(first + rng.below(3))});
    }
    SingleBytes singles = randomSingles(rng, leads, used);
    std::vector<FakeCodePage::Unit> units = std::move(singles.units);
    std::vector<char32_t> pairScalars;
    for (const LeadByteRange& range : leads) {
        for (std::uint32_t lead = range.first; lead <= range.last; ++lead) {
            for (std::uint32_t trail = 0x40; trail <= 0xFE; ++trail) {
                if (trail == 0x7F || rng.below(6) == 0) {
                    continue; // Undefined pairs, including every 7F trail.
                }
                char32_t scalar = 0;
                if (!pairScalars.empty() && rng.below(40) == 0) {
                    scalar = pairScalars[rng.below(pairScalars.size())]; // A duplicate: NotRoundTrip.
                } else {
                    scalar = freshScalar(rng, used);
                    pairScalars.push_back(scalar);
                }
                std::string bytes;
                bytes.push_back(static_cast<char>(static_cast<std::uint8_t>(lead)));
                bytes.push_back(static_cast<char>(static_cast<std::uint8_t>(trail)));
                units.push_back({std::move(bytes), scalar});
            }
        }
    }
    return FakeCodePage("fake-dbcs", std::move(leads), std::move(singles.c1), std::move(units));
}

struct FakeCodecProvider::Shared {
    struct Served {
        std::shared_ptr<const FakeCodePage> page;
        std::unique_ptr<CodePageInfo> info; // The page's info with codePage = the served id.
    };
    std::map<std::uint16_t, Served> pages;
    std::map<std::uint16_t, ErrorCode> openFailures;
    std::optional<std::uint64_t> decodeBudget;
    ErrorCode decodeCode = ErrorCode::Internal;
    std::optional<std::uint64_t> encodeBudget;
    ErrorCode encodeCode = ErrorCode::Internal;
    bool overconsume = false;
    bool illFormed = false;
    std::uint64_t decodeCalls = 0;
    std::uint64_t encodeCalls = 0;
    std::uint64_t opens = 0;
};

namespace {

class FakeCodec final : public ITextCodec {
public:
    FakeCodec(FakeCodecProvider::Shared& shared, const FakeCodePage& page, const CodePageInfo& info) noexcept
        : m_shared(&shared), m_page(&page), m_info(&info) {}

    [[nodiscard]] const CodePageInfo& info() const noexcept override { return *m_info; }

    [[nodiscard]] Result<std::size_t> decodePrefix(std::string_view bytes, std::string& utf8) noexcept override {
        ++m_shared->decodeCalls;
        if (m_shared->decodeBudget) {
            if (*m_shared->decodeBudget == 0) {
                return fail(m_shared->decodeCode, "injected decodePrefix failure");
            }
            --*m_shared->decodeBudget;
        }
        std::size_t i = 0;
        while (i < bytes.size()) {
            const auto b = static_cast<std::uint8_t>(bytes[i]);
            if (m_page->isLead(b)) {
                if (i + 1 >= bytes.size()) {
                    break;
                }
                const auto scalar = m_page->pair(b, static_cast<std::uint8_t>(bytes[i + 1]));
                if (!scalar) {
                    break;
                }
                oracleAppendUtf8(*scalar, utf8);
                i += 2;
            } else {
                const auto scalar = m_page->single(b);
                if (!scalar) {
                    break;
                }
                oracleAppendUtf8(*scalar, utf8);
                i += 1;
            }
        }
        if (m_shared->illFormed) {
            utf8.append("\xC0\x80");
        }
        return m_shared->overconsume ? bytes.size() + 1 : i;
    }

    [[nodiscard]] Result<std::size_t> encodePrefix(std::string_view utf8, std::string& bytes) noexcept override {
        ++m_shared->encodeCalls;
        if (m_shared->encodeBudget) {
            if (*m_shared->encodeBudget == 0) {
                return fail(m_shared->encodeCode, "injected encodePrefix failure");
            }
            --*m_shared->encodeBudget;
        }
        std::size_t i = 0;
        while (i < utf8.size()) {
            char32_t scalar = 0;
            const std::size_t length = oracleDecode(utf8, i, scalar);
            if (length == 0) {
                return fail(ErrorCode::Internal, "the fake codec was given ill-formed UTF-8");
            }
            const auto encoded = m_page->encode(scalar);
            if (!encoded) {
                break;
            }
            bytes.append(*encoded);
            i += length;
        }
        return i;
    }

private:
    FakeCodecProvider::Shared* m_shared;
    const FakeCodePage* m_page;
    const CodePageInfo* m_info;
};

} // namespace

FakeCodecProvider::FakeCodecProvider() : m_shared(std::make_unique<Shared>()) {}
FakeCodecProvider::~FakeCodecProvider() = default;

void FakeCodecProvider::serve(std::shared_ptr<const FakeCodePage> page, std::uint16_t codePage) {
    auto info = std::make_unique<CodePageInfo>(page->info());
    info->codePage = codePage;
    m_shared->pages[codePage] = Shared::Served{std::move(page), std::move(info)};
}

void FakeCodecProvider::failOpen(std::uint16_t codePage, ErrorCode code) { m_shared->openFailures[codePage] = code; }

void FakeCodecProvider::failDecodeAfter(std::uint64_t calls, ErrorCode code) {
    m_shared->decodeBudget = calls;
    m_shared->decodeCode = code;
}

void FakeCodecProvider::failEncodeAfter(std::uint64_t calls, ErrorCode code) {
    m_shared->encodeBudget = calls;
    m_shared->encodeCode = code;
}

void FakeCodecProvider::setOverconsume(bool on) noexcept { m_shared->overconsume = on; }
void FakeCodecProvider::setIllFormedOutput(bool on) noexcept { m_shared->illFormed = on; }
std::uint64_t FakeCodecProvider::decodeCalls() const noexcept { return m_shared->decodeCalls; }
std::uint64_t FakeCodecProvider::encodeCalls() const noexcept { return m_shared->encodeCalls; }
std::uint64_t FakeCodecProvider::opens() const noexcept { return m_shared->opens; }

void FakeCodecProvider::resetCounts() noexcept {
    m_shared->decodeCalls = 0;
    m_shared->encodeCalls = 0;
    m_shared->opens = 0;
}

Result<std::unique_ptr<ITextCodec>> FakeCodecProvider::open(std::uint16_t codePage) noexcept {
    ++m_shared->opens;
    if (const auto failure = m_shared->openFailures.find(codePage); failure != m_shared->openFailures.end()) {
        return fail(failure->second, std::format("injected open failure for code page {}", codePage));
    }
    const auto it = m_shared->pages.find(codePage);
    if (it == m_shared->pages.end()) {
        return fail(ErrorCode::Unsupported, std::format("the fake provider does not serve code page {}", codePage));
    }
    return std::unique_ptr<ITextCodec>(std::make_unique<FakeCodec>(*m_shared, *it->second.page, *it->second.info));
}

} // namespace frost::text::test
