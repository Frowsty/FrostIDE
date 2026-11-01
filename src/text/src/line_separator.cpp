// SPDX-License-Identifier: Apache-2.0
//
// Line-separator names, the platform default, EOL statistics over strings and snapshots, and the
// dominant separator (frost/text/line_separator.hpp; ADR 0018, D9).

#include "frost/text/line_separator.hpp"

#include "frost/core/assert.hpp"
#include "frost/text/snapshot.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace frost::text {
namespace {

constexpr char asciiLower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return asciiLower(x) == asciiLower(y); });
}

} // namespace

std::optional<LineSeparator> findSeparator(std::string_view name) noexcept {
    for (const LineSeparator s : {LineSeparator::Lf, LineSeparator::CrLf, LineSeparator::Cr}) {
        if (equalsIgnoreCase(name, separatorName(s))) {
            return s;
        }
    }
    return std::nullopt;
}

LineSeparator platformDefaultSeparator() noexcept {
#ifdef _WIN32
    return LineSeparator::CrLf;
#else
    return LineSeparator::Lf;
#endif
}

EolStats eolStats(std::string_view text) noexcept {
    FROST_PRE(text.size() <= 0xFFFF'FFFEULL);
    /*
     * Budget: B15 (ADR 0018, D15). Branch-free passes: LF and CR bytes with std::count, then, only
     * if both occur, the CR-LF pairs with a loop over adjacent bytes. On 64 MiB of mixed-EOL code
     * this takes about 15 ms (a first version, one memchr per CR, took 18 ms), twice the UTF-8
     * validation; decodeTextFile therefore counts on the buffer it builds (eolStats(Snapshot)).
     * TODO(M15): m15/large-file fuses the passes if its B15 measurement of mapped loads needs it.
     */
    const auto lfBytes = static_cast<std::uint32_t>(std::count(text.begin(), text.end(), '\n'));
    const auto crBytes = static_cast<std::uint32_t>(std::count(text.begin(), text.end(), '\r'));
    std::uint32_t crlf = 0;
    if (lfBytes != 0 && crBytes != 0) {
        // The text has at least two bytes here. The sum fits: it is at most text.size().
        const char* const p = text.data();
        const std::size_t pairs = text.size() - 1;
        for (std::size_t i = 0; i < pairs; ++i) {
            crlf += static_cast<std::uint32_t>(p[i] == '\r') & static_cast<std::uint32_t>(p[i + 1] == '\n');
        }
    }
    return EolStats{lfBytes - crlf, crlf, crBytes - crlf};
}

EolStats eolStats(const Snapshot& snapshot) noexcept {
    std::uint64_t crBytes = 0;
    for (const std::string_view chunk : snapshot.chunks(ByteRange{ByteOffset{0}, snapshot.size()})) {
        crBytes += static_cast<std::uint64_t>(std::count(chunk.begin(), chunk.end(), '\r'));
    }
    // TextSummary counts breaks in the LSP model and handles CRLFs across pieces (ADR 0013, D2).
    const TextSummary& summary = snapshot.summary();
    const std::uint32_t cr = summary.lineBreaks - summary.lineFeeds;
    const auto crlf = static_cast<std::uint32_t>(crBytes - cr);
    return EolStats{summary.lineFeeds - crlf, crlf, cr};
}

LineSeparator dominantSeparator(const EolStats& stats, LineSeparator fallback) noexcept {
    const std::uint32_t most = std::max({stats.lf, stats.crlf, stats.cr});
    if (most == 0) {
        return fallback;
    }
    const auto count = [&stats](LineSeparator s) {
        switch (s) {
        case LineSeparator::CrLf: return stats.crlf;
        case LineSeparator::Cr: return stats.cr;
        case LineSeparator::Lf: break;
        }
        return stats.lf;
    };
    if (count(fallback) == most) {
        return fallback;
    }
    for (const LineSeparator s : {LineSeparator::Lf, LineSeparator::CrLf, LineSeparator::Cr}) {
        if (count(s) == most) {
            return s;
        }
    }
    return fallback;
}

} // namespace frost::text
