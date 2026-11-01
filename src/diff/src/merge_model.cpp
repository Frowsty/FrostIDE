// SPDX-License-Identifier: Apache-2.0
//
// MergeModel and OursPatch (frost/diff/merge.hpp): takes, resolved text, diff3-style markers,
// line-break joining and the self-contained patch for ours (ADR 0019, D11 and D12).

#include "frost/core/assert.hpp"
#include "frost/diff/merge.hpp"

#include "escape.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace frost::diff {
namespace {

std::string_view eolText(text::LineBreak kind) noexcept {
    switch (kind) {
    case text::LineBreak::Cr: return "\r";
    case text::LineBreak::CrLf: return "\r\n";
    case text::LineBreak::Lf:
    case text::LineBreak::None: break;
    }
    return "\n";
}

std::string_view bytesOf(const LineSequence& sequence, LineRange r) noexcept {
    const text::ByteRange bytes = sequence.byteRange(r);
    return sequence.bytes().substr(bytes.begin.value(), bytes.size());
}

bool isMarkerChar(char c) noexcept { return c == '<' || c == '|' || c == '=' || c == '>'; }

// The bit of a marker-character run of length n at a line start: bit n - 1 for n in
// [1, kMaxMarkerSize], else none. Only the first kMaxMarkerSize + 1 bytes are read.
std::uint64_t runBit(std::string_view line) noexcept {
    if (line.empty() || !isMarkerChar(line.front())) {
        return 0;
    }
    const std::string_view head = line.substr(0, kMaxMarkerSize + 1);
    const std::size_t run = std::min(head.find_first_not_of(head.front()), head.size());
    return run <= kMaxMarkerSize ? std::uint64_t{1} << (run - 1) : 0;
}

// The run lengths of every line of `sequence` (see runBit). O(lines).
std::uint64_t markerRuns(const LineSequence& sequence) noexcept {
    std::uint64_t runs = 0;
    for (std::uint32_t i = 0; i < sequence.size(); ++i) {
        runs |= runBit(sequence.line(LineIndex{i}));
    }
    return runs;
}

// The run lengths of every line of a custom text under `split`: lines start at 0 and after each
// LF (Lf), or after each LF or CR (Lsp; after the CR of a CRLF comes an LF, which is no marker
// character). O(|text|).
std::uint64_t markerRuns(std::string_view text, LineSplit split) noexcept {
    std::uint64_t runs = runBit(text);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' || (split == LineSplit::Lsp && text[i] == '\r')) {
            runs |= runBit(text.substr(i + 1));
        }
    }
    return runs;
}

Take defaultTake(ChunkKind kind) noexcept {
    switch (kind) {
    case ChunkKind::Theirs: return Take::Theirs;
    case ChunkKind::Conflict: return Take::Unresolved;
    case ChunkKind::Stable:
    case ChunkKind::Ours:
    case ChunkKind::Both: break;
    }
    return Take::Ours;
}

// A piece of output: bytes viewed from a sequence, a custom text or a marker line.
struct Piece {
    std::string_view bytes;
    bool fromSequence = false; // A side's lines (the joining rule applies).
    bool joinable = false;     // A side's lines or a custom text: gets eol() when it lacks a break.
};

// The rendering of a model: the text, per-chunk ranges and, per chunk, its output when it is
// exactly one view of a sequence (so a patch can reference it instead of copying).
struct Rendered {
    MergeOutput output;
    std::vector<std::optional<std::string_view>> singleView;
};

} // namespace

Result<MergeModel> MergeModel::create(LineSequence base, LineSequence ours, LineSequence theirs,
                                      const MergeOptions& options, std::stop_token stop) noexcept {
    auto merged = merge3(base, ours, theirs, options, std::move(stop));
    if (!merged) {
        return std::unexpected(std::move(merged.error()));
    }
    MergeModel model;
    model.m_merge = std::move(*merged);
    model.m_takes.reserve(model.m_merge.chunks.size());
    for (const MergeChunk& chunk : model.m_merge.chunks) {
        model.m_takes.push_back(defaultTake(chunk.kind));
    }
    model.m_custom.resize(model.m_merge.chunks.size());
    model.m_unresolved = model.m_merge.conflicts;
    if (options.eol != text::LineBreak::None) {
        model.m_eol = eolText(options.eol);
    } else {
        text::LineBreak detected = ours.firstLineBreak();
        if (detected == text::LineBreak::None) {
            detected = theirs.firstLineBreak();
        }
        if (detected == text::LineBreak::None) {
            detected = base.firstLineBreak();
        }
        model.m_eol = eolText(detected);
    }
    model.m_markerRuns = markerRuns(base) | markerRuns(ours) | markerRuns(theirs);
    model.m_base = std::move(base);
    model.m_ours = std::move(ours);
    model.m_theirs = std::move(theirs);
    return model;
}

Take MergeModel::take(std::size_t chunk) const noexcept {
    FROST_PRE(chunk < m_takes.size());
    return m_takes[chunk];
}

Result<void> MergeModel::resolve(std::size_t chunk, Take take, std::string customText) noexcept {
    if (chunk >= m_takes.size()) {
        return fail(ErrorCode::OutOfRange, std::format("chunk {} of {}", chunk, m_takes.size()));
    }
    const ChunkKind kind = m_merge.chunks[chunk].kind;
    if (kind == ChunkKind::Stable) {
        return fail(ErrorCode::InvalidArgument, std::format("chunk {}: a Stable chunk has nothing to resolve", chunk));
    }
    if (take == Take::Unresolved && kind != ChunkKind::Conflict) {
        return fail(ErrorCode::InvalidArgument, std::format("chunk {}: only a conflict can be unresolved", chunk));
    }
    if (!customText.empty() && take != Take::Custom) {
        return fail(ErrorCode::InvalidArgument, std::format("chunk {}: custom text needs Take::Custom", chunk));
    }
    const bool was = m_takes[chunk] == Take::Unresolved;
    const bool now = take == Take::Unresolved;
    if (was && !now) {
        --m_unresolved;
    } else if (!was && now) {
        ++m_unresolved;
    }
    m_takes[chunk] = take;
    m_custom[chunk] = std::move(customText);
    return {};
}

namespace {

/*
 * Renders `model`. With markers == nullptr an Unresolved conflict is an error; otherwise it is
 * written with the four marker lines in markers[0..3].
 */
Result<Rendered> render(const MergeModel& model, std::span<const std::string> markers,
                        std::span<const std::string> custom, std::span<const Take> takes) noexcept {
    const std::vector<MergeChunk>& chunks = model.merge().chunks;
    const bool lsp = model.ours().split() == LineSplit::Lsp;
    std::vector<Piece> pieces;
    std::vector<std::size_t> firstPiece(chunks.size() + 1, 0);
    for (std::size_t k = 0; k < chunks.size(); ++k) {
        firstPiece[k] = pieces.size();
        const MergeChunk& chunk = chunks[k];
        const auto side = [&](const LineSequence& sequence, LineRange r) {
            pieces.push_back(Piece{bytesOf(sequence, r), true, true});
        };
        const Take take = chunk.kind == ChunkKind::Stable ? Take::Ours : takes[k];
        switch (take) {
        case Take::Ours: side(model.ours(), chunk.ours); break;
        case Take::Theirs: side(model.theirs(), chunk.theirs); break;
        case Take::Base: side(model.base(), chunk.base); break;
        case Take::OursThenTheirs:
            side(model.ours(), chunk.ours);
            side(model.theirs(), chunk.theirs);
            break;
        case Take::TheirsThenOurs:
            side(model.theirs(), chunk.theirs);
            side(model.ours(), chunk.ours);
            break;
        case Take::Custom: pieces.push_back(Piece{custom[k], false, true}); break;
        case Take::Unresolved:
            if (markers.empty()) {
                return fail(ErrorCode::InvalidArgument, std::format("chunk {}: conflict unresolved", k));
            }
            pieces.push_back(Piece{markers[0], false, false});
            side(model.ours(), chunk.ours);
            pieces.push_back(Piece{markers[1], false, false});
            side(model.base(), chunk.base);
            pieces.push_back(Piece{markers[2], false, false});
            side(model.theirs(), chunk.theirs);
            pieces.push_back(Piece{markers[3], false, false});
            break;
        }
    }
    firstPiece[chunks.size()] = pieces.size();
    std::size_t lastNonEmpty = pieces.size();
    for (std::size_t p = pieces.size(); p-- > 0;) {
        if (!pieces[p].bytes.empty()) {
            lastNonEmpty = p;
            break;
        }
    }
    Rendered rendered;
    std::string& text = rendered.output.text;
    rendered.output.chunks.reserve(chunks.size());
    rendered.singleView.resize(chunks.size());
    const std::string_view eol = model.eol();
    for (std::size_t k = 0; k < chunks.size(); ++k) {
        const std::size_t begin = text.size();
        bool joined = false;
        std::size_t nonEmpty = 0;
        std::size_t last = 0;
        for (std::size_t p = firstPiece[k]; p < firstPiece[k + 1]; ++p) {
            const Piece& piece = pieces[p];
            if (piece.bytes.empty()) {
                continue;
            }
            text += piece.bytes;
            ++nonEmpty;
            last = p;
            const char tail = piece.bytes.back();
            const bool hasBreak = tail == '\n' || (lsp && tail == '\r');
            if (piece.joinable && !hasBreak && p < lastNonEmpty) {
                text += eol;
                joined = true;
            }
        }
        rendered.output.chunks.push_back(text::ByteRange{ByteOffset{begin}, ByteOffset{text.size()}});
        if (nonEmpty == 0) {
            rendered.singleView[k] = std::string_view();
        } else if (nonEmpty == 1 && !joined && pieces[last].fromSequence) {
            rendered.singleView[k] = pieces[last].bytes;
        }
    }
    return rendered;
}

} // namespace

Result<MergeOutput> MergeModel::resolvedText() const noexcept {
    auto rendered = render(*this, {}, m_custom, m_takes);
    if (!rendered) {
        return std::unexpected(std::move(rendered.error()));
    }
    return std::move(rendered->output);
}

std::string_view MergeModel::customText(std::size_t chunk) const noexcept {
    FROST_PRE(chunk < m_custom.size());
    return m_custom[chunk];
}

Result<std::uint32_t> MergeModel::markerSize(const MarkerOptions& options) const noexcept {
    const std::uint32_t minSize = std::clamp<std::uint32_t>(options.minSize, 7, 32);
    std::uint64_t runs = m_markerRuns;
    for (std::size_t k = 0; k < m_takes.size(); ++k) {
        if (m_takes[k] == Take::Custom) {
            runs |= markerRuns(m_custom[k], m_ours.split());
        }
    }
    for (std::uint32_t size = minSize; size <= kMaxMarkerSize; ++size) {
        if ((runs >> (size - 1) & 1) == 0) {
            return size;
        }
    }
    return fail(ErrorCode::OutOfRange,
                std::format("no conflict-marker size from {} to {} is free: lines start with runs of marker "
                            "characters of every such length",
                            minSize, kMaxMarkerSize));
}

Result<MergeOutput> MergeModel::renderWithMarkers(const MarkerOptions& options) const noexcept {
    auto chosen = markerSize(options);
    if (!chosen) {
        return std::unexpected(std::move(chosen.error()));
    }
    const std::uint32_t size = *chosen;
    const auto marker = [&](char c, std::string_view label) {
        std::string line(size, c);
        if (!label.empty()) {
            line += ' ';
            detail::appendEscapedLine(line, label);
        }
        line += m_eol;
        return line;
    };
    const std::string markers[4] = {marker('<', options.oursLabel), marker('|', options.baseLabel),
                                    marker('=', std::string_view()), marker('>', options.theirsLabel)};
    auto rendered = render(*this, markers, m_custom, m_takes);
    FROST_ASSERT(rendered.has_value()); // Markers render every take.
    if (!rendered) {
        return std::unexpected(std::move(rendered.error()));
    }
    return std::move(rendered->output);
}

Result<OursPatch> MergeModel::editsForOurs() const noexcept {
    auto rendered = render(*this, {}, m_custom, m_takes);
    if (!rendered) {
        return std::unexpected(std::move(rendered.error()));
    }
    const std::vector<MergeChunk>& chunks = m_merge.chunks;
    const std::string& text = rendered->output.text;
    // Which chunks change, and the arena of the outputs that are not one sequence view.
    struct Pending {
        std::size_t chunk;
        std::size_t arenaOffset; // Valid when !rendered->singleView[chunk].
    };
    std::vector<Pending> pending;
    auto arena = std::make_unique<std::string>();
    for (std::size_t k = 0; k < chunks.size(); ++k) {
        const text::ByteRange out = rendered->output.chunks[k];
        const std::string_view produced = std::string_view(text).substr(out.begin.value(), out.size());
        if (produced == bytesOf(m_ours, chunks[k].ours)) {
            continue;
        }
        Pending item{k, arena->size()};
        if (!rendered->singleView[k]) {
            arena->append(produced);
        }
        pending.push_back(item);
    }
    // The arena is complete; views into it are stable from here on (it lives on the heap).
    OursPatch patch;
    patch.m_edits.reserve(pending.size());
    for (const Pending& item : pending) {
        const text::ByteRange out = rendered->output.chunks[item.chunk];
        const std::string_view view = rendered->singleView[item.chunk]
                                          ? *rendered->singleView[item.chunk]
                                          : std::string_view(*arena).substr(item.arenaOffset, out.size());
        patch.m_edits.push_back(text::Edit{m_ours.byteRange(chunks[item.chunk].ours), view});
    }
    patch.m_arena = std::move(arena);
    patch.m_base = m_base;
    patch.m_ours = m_ours;
    patch.m_theirs = m_theirs;
    return patch;
}

} // namespace frost::diff
