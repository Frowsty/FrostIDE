// SPDX-License-Identifier: Apache-2.0
//
// The shared property checks of the frost::diff tests (checks.hpp).

#include "diff/support/checks.hpp"

#include "frost/diff/unified.hpp"
#include "frost/text/text_buffer.hpp"

#include "diff/support/oracles.hpp"
#include "engine.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace frost::diff::test {
namespace {

std::string_view bytesOf(const LineSequence& sequence, LineRange range) {
    const text::ByteRange r = sequence.byteRange(range);
    return sequence.bytes().substr(r.begin.value(), r.size());
}

// The label as renderWithMarkers must write it: CR, LF and backslash escaped.
std::string escaped(std::string_view label) {
    std::string out;
    for (const char c : label) {
        if (c == '\r') {
            out += "\\r";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\\') {
            out += "\\\\";
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

bool hasLoneCr(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && (i + 1 == text.size() || text[i + 1] != '\n')) {
            return true;
        }
    }
    return false;
}

bool validUtf8(std::string_view text) { return text::TextBuffer::fromUtf8(std::string(text)).has_value(); }

std::string printable(std::string_view bytes, std::size_t limit) {
    std::string out = "\"";
    for (std::size_t i = 0; i < bytes.size() && i < limit; ++i) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\\' || c == '"') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20 || c >= 0x7F) {
            out += std::format("\\x{:02x}", c);
        } else {
            out += static_cast<char>(c);
        }
    }
    if (bytes.size() > limit) {
        out += "...";
    }
    out += '"';
    return out;
}

Result<void> checkApplies(const LineSequence& a, const LineSequence& b, const LineDiff& diff,
                          const DiffOptions& options) {
    if (auto checked = checkDiff(a, b, diff, options); !checked) {
        return fail(ErrorCode::Internal, std::format("checkDiff: {}", checked.error().describe()));
    }
    const std::vector<std::string> linesA = naiveSplit(a.bytes(), a.split());
    const std::vector<std::string> linesB = naiveSplit(b.bytes(), b.split());
    auto patched = applyHunks(linesA, linesB, diff.hunks);
    if (!patched || *patched != b.bytes()) {
        return fail(ErrorCode::Internal,
                    std::format("byte patcher: {}", patched ? printable(*patched) : patched.error().describe()));
    }
    if (validUtf8(a.bytes()) && validUtf8(b.bytes())) {
        auto buffer = text::TextBuffer::fromUtf8(std::string(a.bytes()));
        const std::vector<text::Edit> edits = toEdits(a, b, diff);
        auto applied = buffer->applyEdits(edits);
        if (!applied) {
            return fail(ErrorCode::Internal, std::format("applyEdits: {}", applied.error().describe()));
        }
        if (buffer->snapshot().text() != b.bytes()) {
            return fail(ErrorCode::Internal, std::format("applyEdits gave {}", printable(buffer->snapshot().text())));
        }
    }
    if (a.split() == LineSplit::Lsp && (hasLoneCr(a.bytes()) || hasLoneCr(b.bytes()))) {
        return {};
    }
    for (const std::uint32_t context : {0u, 1u, 3u}) {
        UnifiedOptions unified;
        unified.context = context;
        unified.oldHeader = "a/file";
        unified.newHeader = "b/file";
        const std::string patch = renderUnified(a, b, diff, unified);
        auto result = applyUnified(a.bytes(), patch);
        if (!result || *result != b.bytes()) {
            return fail(ErrorCode::Internal,
                        std::format("unified -U{}: {}\npatch: {}", context,
                                    result ? printable(*result) : result.error().describe(), printable(patch)));
        }
    }
    return {};
}

Result<void> checkRefinement(const LineSequence& a, const LineSequence& b, const Hunk& hunk,
                             const Refinement& refinement) {
    const text::ByteRange rangeA = a.byteRange(hunk.a);
    const text::ByteRange rangeB = b.byteRange(hunk.b);
    std::string out;
    std::uint64_t cursorA = rangeA.begin.value();
    std::uint64_t cursorB = rangeB.begin.value();
    for (const RangePair& pair : refinement.changes) {
        if (pair.a.begin.value() < cursorA || pair.b.begin.value() < cursorB || pair.a.end < pair.a.begin ||
            pair.b.end < pair.b.begin || pair.a.end > rangeA.end || pair.b.end > rangeB.end) {
            return fail(ErrorCode::Internal, "refined pairs out of order or outside the hunk");
        }
        const std::string_view keptA = a.bytes().substr(cursorA, pair.a.begin.value() - cursorA);
        const std::string_view keptB = b.bytes().substr(cursorB, pair.b.begin.value() - cursorB);
        if (keptA != keptB) {
            return fail(ErrorCode::Internal, "refined pairs leave different unchanged bytes");
        }
        out += keptA;
        out += b.bytes().substr(pair.b.begin.value(), pair.b.size());
        cursorA = pair.a.end.value();
        cursorB = pair.b.end.value();
    }
    out += a.bytes().substr(cursorA, rangeA.end.value() - cursorA);
    const std::string_view expected = b.bytes().substr(rangeB.begin.value(), rangeB.size());
    if (out != expected) {
        return fail(ErrorCode::Internal,
                    std::format("refined pairs give {}, expected {}", printable(out), printable(expected)));
    }
    return {};
}

Result<void> checkRefineScansAgree(const LineSequence& a, const LineSequence& b, const Hunk& hunk,
                                   Granularity granularity, const RefineLimits& limits) {
    auto window = detail::refineHunkWith(a, b, hunk, granularity, limits, {}, detail::RefineScan::Window);
    auto full = detail::refineHunkWith(a, b, hunk, granularity, limits, {}, detail::RefineScan::FullScan);
    if (!window || !full) {
        return std::unexpected(window ? full.error() : window.error());
    }
    const auto describe = [](const Refinement& refinement) {
        std::string out;
        for (const RangePair& pair : refinement.changes) {
            out += std::format("[{},{})->[{},{}) ", pair.a.begin.value(), pair.a.end.value(), pair.b.begin.value(),
                               pair.b.end.value());
        }
        return out + (refinement.coarse ? "coarse" : "");
    };
    if (window->changes != full->changes || window->coarse != full->coarse) {
        return fail(ErrorCode::Internal, std::format("windowed refinement {} differs from the full scan's {}",
                                                     describe(*window), describe(*full)));
    }
    return {};
}

Result<void> checkMarkersRoundTrip(const MergeModel& model, const MarkerOptions& labels) {
    const LineSplit split = model.ours().split();
    /*
     * The marker size, computed here from the documented rule: the smallest n in
     * [clamp(minSize, 7, 32), kMaxMarkerSize] that is no line's leading run of one marker
     * character, over the three sequences' lines (naiveSplit) and the custom texts of Custom chunks
     * (split the same way); none means renderWithMarkers must refuse.
     */
    std::vector<bool> taken(kMaxMarkerSize + 1, false);
    const auto note = [&](std::string_view line) {
        if (line.empty() || (line[0] != '<' && line[0] != '|' && line[0] != '=' && line[0] != '>')) {
            return;
        }
        std::size_t run = 0;
        while (run < line.size() && line[run] == line[0]) {
            ++run;
        }
        if (run <= kMaxMarkerSize) {
            taken[run] = true;
        }
    };
    for (const LineSequence* sequence : {&model.base(), &model.ours(), &model.theirs()}) {
        for (const std::string& line : naiveSplit(sequence->bytes(), split)) {
            note(line);
        }
    }
    for (std::size_t k = 0; k < model.merge().chunks.size(); ++k) {
        if (model.take(k) == Take::Custom) {
            for (const std::string& line : naiveSplit(model.customText(k), split)) {
                note(line);
            }
        }
    }
    std::uint32_t size = 0;
    for (std::uint32_t n = std::clamp<std::uint32_t>(labels.minSize, 7, 32); n <= kMaxMarkerSize && size == 0; ++n) {
        size = taken[n] ? 0 : n;
    }
    auto rendered = model.renderWithMarkers(labels);
    if (size == 0) {
        if (rendered || rendered.error().code() != ErrorCode::OutOfRange) {
            return fail(ErrorCode::Internal, "every marker size collides, but renderWithMarkers did not refuse");
        }
        return {};
    }
    if (!rendered) {
        return fail(ErrorCode::Internal, std::format("renderWithMarkers failed although size {} is free: {}", size,
                                                     rendered.error().describe()));
    }
    if (auto chosen = model.markerSize(labels); !chosen || *chosen != size) {
        return fail(ErrorCode::Internal, std::format("markerSize disagrees with the rule's {}", size));
    }
    const MergeOutput& output = *rendered;
    auto regions = parseMarkers(output.text, size, split);
    if (!regions) {
        return fail(ErrorCode::Internal, std::format("markers do not parse: {}", regions.error().describe()));
    }
    const std::string_view eol = model.eol();
    const bool lsp = split == LineSplit::Lsp;
    const auto withBreak = [&](std::string_view bytes) {
        std::string s(bytes);
        if (!s.empty() && s.back() != '\n' && !(lsp && s.back() == '\r')) {
            s += eol;
        }
        return s;
    };
    std::vector<MarkedRegion> expected;
    for (std::size_t k = 0; k < model.merge().chunks.size(); ++k) {
        const MergeChunk& chunk = model.merge().chunks[k];
        if (chunk.kind == ChunkKind::Conflict && model.take(k) == Take::Unresolved) {
            MarkedRegion region;
            region.conflict = true;
            region.ours = withBreak(bytesOf(model.ours(), chunk.ours));
            region.base = withBreak(bytesOf(model.base(), chunk.base));
            region.theirs = withBreak(bytesOf(model.theirs(), chunk.theirs));
            region.oursLabel = escaped(labels.oursLabel);
            region.baseLabel = escaped(labels.baseLabel);
            region.theirsLabel = escaped(labels.theirsLabel);
            expected.push_back(std::move(region));
            continue;
        }
        const text::ByteRange range = output.chunks[k];
        if (expected.empty() || expected.back().conflict) {
            expected.push_back(MarkedRegion{});
        }
        expected.back().text += std::string_view(output.text).substr(range.begin.value(), range.size());
    }
    std::erase_if(expected, [](const MarkedRegion& r) { return !r.conflict && r.text.empty(); });
    if (regions->size() != expected.size()) {
        return fail(ErrorCode::Internal, std::format("{} regions parsed, {} expected in {}", regions->size(),
                                                     expected.size(), printable(output.text, 1000)));
    }
    for (std::size_t r = 0; r < expected.size(); ++r) {
        const MarkedRegion& got = (*regions)[r];
        const MarkedRegion& want = expected[r];
        if (got.conflict != want.conflict || got.text != want.text || got.ours != want.ours || got.base != want.base ||
            got.theirs != want.theirs || got.oursLabel != want.oursLabel || got.baseLabel != want.baseLabel ||
            got.theirsLabel != want.theirsLabel) {
            return fail(ErrorCode::Internal,
                        std::format("region {} does not round-trip in {}", r, printable(output.text, 1000)));
        }
    }
    return {};
}

} // namespace frost::diff::test
