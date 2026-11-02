// SPDX-License-Identifier: Apache-2.0
//
// renderUnified (frost/diff/unified.hpp): hunk grouping by context, @@ headers and line bodies,
// written from POSIX.1-2024 `diff -u` and checked byte for byte against git's -U<k> output.

#include "frost/diff/unified.hpp"

#include "frost/core/assert.hpp"

#include "escape.hpp"

#include <algorithm>
#include <format>

namespace frost::diff {
namespace {

// R(s, e) of the header: "s+1" for one line, "s,0" for none, else "s+1,e-s".
void appendRange(std::string& out, std::uint32_t s, std::uint32_t e) {
    if (e - s == 1) {
        std::format_to(std::back_inserter(out), "{}", s + 1);
    } else if (e == s) {
        std::format_to(std::back_inserter(out), "{},0", s);
    } else {
        std::format_to(std::back_inserter(out), "{},{}", s + 1, e - s);
    }
}

void appendLine(std::string& out, char prefix, const LineSequence& sequence, std::uint32_t line) {
    out += prefix;
    out += sequence.line(LineIndex{line});
    if (!sequence.hasBreak(LineIndex{line})) {
        out += "\n\\ No newline at end of file\n";
    }
}

} // namespace

std::string renderUnified(const LineSequence& a, const LineSequence& b, const LineDiff& diff,
                          const UnifiedOptions& options) noexcept {
    std::string out;
    const std::vector<Hunk>& hunks = diff.hunks;
    if (hunks.empty()) {
        return out;
    }
    if (!options.oldHeader.empty() && !options.newHeader.empty()) {
        out += "--- ";
        detail::appendEscapedLine(out, options.oldHeader);
        out += "\n+++ ";
        detail::appendEscapedLine(out, options.newHeader);
        out += '\n';
    }
    const std::uint64_t context = options.context;
    std::size_t first = 0;
    while (first < hunks.size()) {
        // The group: hunks closer than 2 * context unchanged lines join.
        std::size_t last = first;
        while (last + 1 < hunks.size() &&
               std::uint64_t{hunks[last + 1].a.begin.value() - hunks[last].a.end.value()} <= 2 * context) {
            ++last;
        }
        const Hunk& head = hunks[first];
        const Hunk& tail = hunks[last];
        const auto a0 = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, std::int64_t{head.a.begin.value()} - static_cast<std::int64_t>(context)));
        const auto a1 =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(a.size(), std::uint64_t{tail.a.end.value()} + context));
        const std::uint32_t b0 = head.b.begin.value() - (head.a.begin.value() - a0);
        const std::uint32_t b1 = tail.b.end.value() + (a1 - tail.a.end.value());
        FROST_ASSERT(b1 <= b.size());
        out += "@@ -";
        appendRange(out, a0, a1);
        out += " +";
        appendRange(out, b0, b1);
        out += " @@\n";
        std::uint32_t i = a0;
        for (std::size_t h = first; h <= last; ++h) {
            const Hunk& hunk = hunks[h];
            for (; i < hunk.a.begin.value(); ++i) {
                appendLine(out, ' ', a, i);
            }
            for (std::uint32_t k = hunk.a.begin.value(); k < hunk.a.end.value(); ++k) {
                appendLine(out, '-', a, k);
            }
            for (std::uint32_t k = hunk.b.begin.value(); k < hunk.b.end.value(); ++k) {
                appendLine(out, '+', b, k);
            }
            i = hunk.a.end.value();
        }
        for (; i < a1; ++i) {
            appendLine(out, ' ', a, i);
        }
        first = last + 1;
    }
    return out;
}

} // namespace frost::diff
