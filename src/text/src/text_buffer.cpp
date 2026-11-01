// SPDX-License-Identifier: Apache-2.0
//
// TextBuffer (frost/text/text_buffer.hpp): loading, validation in the shared check order, edits as
// split + split + concat + concat over the piece tree, typing coalescing, D8 change normalisation,
// batches and restore.

#include "frost/text/text_buffer.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/utf.hpp"

#include "piece_tree.hpp"
#include "storage.hpp"

#include <format>
#include <utility>

namespace frost::text {
namespace {

constexpr std::uint32_t kMinPieceBytes = 4;
constexpr std::uint32_t kMaxPieceBytes = 1u << 20;
constexpr std::uint64_t kMaxBufferBytes = 0xFFFF'FFFEULL;

bool validOptions(const TextBufferOptions& options) noexcept {
    return options.maxPieceBytes >= kMinPieceBytes && options.maxPieceBytes <= kMaxPieceBytes &&
           options.maxBufferBytes <= kMaxBufferBytes;
}

// A byte of the old text, or -1 when `i` is outside it ("absent", D8).
int byteOrAbsent(const detail::PieceTree& tree, std::uint64_t i) noexcept {
    return i < tree.summary().bytes ? static_cast<unsigned char>(detail::byteAt(tree, i)) : -1;
}

/*
 * The change of one edit step, normalised (ADR 0013, D8). The old text O had [a, b) replaced by
 * `text`, giving N. With s = a, eo = b, en = a + |text| (bytes outside a text are absent):
 *   start rule: if s > 0, O[s-1] == CR and (O[s] == LF or N[s] == LF), then s -= 1;
 *   end rule:   if O[eo] == LF and (O[eo-1] == CR or N[en-1] == CR), then eo += 1 and en += 1,
 *               tested with the requested bounds (before the start rule).
 * N's bytes at the bounds come from `text` or, when it is empty, from O beside the range, so the
 * rules take at most four byte reads of O; the points take three point queries.
 */
TextChange normalisedChange(const detail::PieceTree& before, const detail::PieceTree& after, std::uint64_t a,
                            std::uint64_t b, std::string_view text) noexcept {
    std::uint64_t s = a;
    std::uint64_t eo = b;
    std::uint64_t en = a + text.size();
    const int oldBeforeStart = a > 0 ? byteOrAbsent(before, a - 1) : -1;
    const int oldAtEnd = byteOrAbsent(before, b);
    const int newAtStart = !text.empty() ? static_cast<unsigned char>(text.front()) : oldAtEnd;
    const int newBeforeEnd = !text.empty() ? static_cast<unsigned char>(text.back()) : oldBeforeStart;
    if (s > 0 && oldBeforeStart == '\r' && (byteOrAbsent(before, a) == '\n' || newAtStart == '\n')) {
        --s;
    }
    const int oldBeforeEnd = b > 0 ? byteOrAbsent(before, b - 1) : -1;
    if (oldAtEnd == '\n' && (oldBeforeEnd == '\r' || newBeforeEnd == '\r')) {
        ++eo;
        ++en;
    }
    return TextChange{ByteOffset{s},
                      ByteOffset{eo},
                      ByteOffset{en},
                      detail::pointAt(before, s),
                      detail::pointAt(before, eo),
                      detail::pointAt(after, en)};
}

// The empty change of a no-op edit at `offset`.
TextChange emptyChange(const detail::PieceTree& tree, std::uint64_t offset) noexcept {
    const Point point = detail::pointAt(tree, offset);
    return TextChange{ByteOffset{offset}, ByteOffset{offset}, ByteOffset{offset}, point, point, point};
}

} // namespace

TextBuffer::TextBuffer() noexcept : TextBuffer(Options{}) {}

TextBuffer::TextBuffer(Options options) noexcept
    : TextBuffer(detail::Ref<detail::Storage>::make(options), detail::PieceTree(), options) {}

TextBuffer::TextBuffer(detail::Ref<detail::Storage> storage, detail::PieceTree tree, Options options) noexcept
    : m_storage(std::move(storage)), m_current(std::move(tree), m_storage, 0), m_version(0), m_options(options) {
    FROST_PRE(validOptions(options));
}

TextBuffer::TextBuffer(TextBuffer&& other) noexcept = default;
TextBuffer& TextBuffer::operator=(TextBuffer&& other) noexcept = default;
TextBuffer::~TextBuffer() = default;

Result<TextBuffer> TextBuffer::fromUtf8(std::string text, Options options) noexcept {
    FROST_PRE(validOptions(options));
    if (text.size() > options.maxBufferBytes) {
        return fail(ErrorCode::OutOfRange, std::format("the text has {} bytes, more than the buffer limit of {}",
                                                       text.size(), options.maxBufferBytes));
    }
    if (const std::size_t valid = io::validUtf8Prefix(text); valid != text.size()) {
        return fail(ErrorCode::InvalidArgument, std::format("ill-formed UTF-8 at byte {}", valid));
    }
    auto storage = detail::Ref<detail::Storage>::make(options, std::move(text));
    // summarizePieces, then buildTree (ADR 0013): the first is pure over the stored bytes and can
    // later run in parallel per range. Budget: B15.
    const std::vector<detail::Piece> pieces = detail::summarizePieces(storage->original(), options.maxPieceBytes);
    detail::PieceTree tree = detail::buildTree(pieces);
    return TextBuffer(std::move(storage), std::move(tree), options);
}

Result<void> TextBuffer::validate(ByteRange range, std::string_view text, std::string_view label) const noexcept {
    const detail::PieceTree& tree = m_current.pieceTree();
    if (range.begin > range.end) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{}range begin {} is after its end {}", label, range.begin, range.end));
    }
    if (range.end > m_current.size()) {
        return fail(ErrorCode::OutOfRange,
                    std::format("{}range end {} is beyond the text's {} bytes", label, range.end, m_current.size()));
    }
    for (const ByteOffset bound : {range.begin, range.end}) {
        if (!detail::isCharBoundary(tree, bound.value())) {
            return fail(ErrorCode::InvalidArgument, std::format("{}byte {} is inside a UTF-8 sequence", label, bound));
        }
    }
    if (const std::size_t valid = io::validUtf8Prefix(text); valid != text.size()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{}ill-formed UTF-8 at byte {} of the inserted text", label, valid));
    }
    return {};
}

detail::PieceTree TextBuffer::replaceTree(const detail::PieceTree& tree, ByteRange range,
                                          std::string_view text) noexcept {
    auto [left, rest] = detail::splitAtByte(tree, range.begin.value());
    detail::PieceTree right = detail::splitAtByte(rest, range.size()).second;
    if (text.empty()) {
        return detail::PieceTree::concat(left, right);
    }
    detail::Storage& storage = *m_storage;
    const std::uint32_t maxPieceBytes = m_options.maxPieceBytes;
    /*
     * Typing coalescing (stateless): when the last piece before the insertion point ends exactly at
     * the storage's write cursor and the text fits both the cursor's chunk and the piece cap, the
     * text is appended right behind it and that piece is replaced by an extended copy (a new piece
     * in a new leaf; the old leaf, which older snapshots may share, is untouched). The bytes past
     * the cursor were never published, so no snapshot can see them change.
     */
    if (const detail::Piece* last = left.back();
        last != nullptr && text.size() < detail::Storage::kDedicatedChunkBytes && text.size() <= storage.cursorRoom() &&
        last->size() + text.size() <= maxPieceBytes && storage.endsAtCursor(last->data, last->size())) {
        const detail::Piece extended{last->data, combine(last->summary, summarize(text))};
        [[maybe_unused]] const char* stored = storage.append(text);
        FROST_ASSERT(stored == last->data + last->size());
        left = detail::PieceTree::concat(left.withoutBack(), detail::singlePiece(extended));
        return detail::PieceTree::concat(left, right);
    }
    const char* stored = storage.append(text);
    std::vector<detail::Piece> pieces;
    pieces.reserve(text.size() / maxPieceBytes + 1);
    detail::appendPieces(pieces, stored, text.size(), maxPieceBytes);
    const detail::PieceTree middle = detail::buildTree(pieces);
    return detail::PieceTree::concat(detail::PieceTree::concat(left, middle), right);
}

Result<TextChange> TextBuffer::insert(ByteOffset at, std::string_view utf8) noexcept {
    return replace(ByteRange{at, at}, utf8);
}

Result<TextChange> TextBuffer::erase(ByteRange range) noexcept { return replace(range, {}); }

Result<TextChange> TextBuffer::replace(ByteRange range, std::string_view utf8) noexcept {
    if (auto valid = validate(range, utf8, {}); !valid) {
        return std::unexpected(std::move(valid).error());
    }
    const std::uint64_t oldSize = m_current.size().value();
    const std::uint64_t newSize = oldSize - range.size() + utf8.size();
    if (newSize > m_options.maxBufferBytes) {
        return fail(ErrorCode::OutOfRange, std::format("the edit would make the text {} bytes, more than the buffer "
                                                       "limit of {}",
                                                       newSize, m_options.maxBufferBytes));
    }
    const detail::PieceTree& before = m_current.pieceTree();
    if (range.empty() && utf8.empty()) {
        return emptyChange(before, range.begin.value());
    }
    detail::PieceTree after = replaceTree(before, range, utf8);
    const TextChange change = normalisedChange(before, after, range.begin.value(), range.end.value(), utf8);
    m_current = Snapshot(std::move(after), m_storage, ++m_version);
    return change;
}

Result<std::vector<TextChange>> TextBuffer::applyEdits(std::span<const Edit> edits) noexcept {
    std::vector<TextChange> changes;
    if (edits.empty()) {
        return changes;
    }
    // Validate everything before applying anything.
    std::uint64_t newSize = m_current.size().value();
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const std::string label = std::format("edit {}: ", i);
        if (auto valid = validate(edits[i].range, edits[i].text, label); !valid) {
            return std::unexpected(std::move(valid).error());
        }
        if (i > 0 && edits[i - 1].range.end > edits[i].range.begin) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("{}range [{}, {}) starts before the end {} of edit {} (edits must be sorted and "
                                    "must not overlap)",
                                    label, edits[i].range.begin, edits[i].range.end, edits[i - 1].range.end, i - 1));
        }
        newSize = newSize - edits[i].range.size() + edits[i].text.size();
    }
    if (newSize > m_options.maxBufferBytes) {
        return fail(ErrorCode::OutOfRange, std::format("the edits would make the text {} bytes, more than the buffer "
                                                       "limit of {}",
                                                       newSize, m_options.maxBufferBytes));
    }
    // Apply last to first, so the offsets of the edits still to apply stay valid.
    detail::PieceTree tree = m_current.pieceTree();
    changes.reserve(edits.size());
    for (std::size_t i = edits.size(); i-- > 0;) {
        const Edit& edit = edits[i];
        if (edit.range.empty() && edit.text.empty()) {
            continue;
        }
        detail::PieceTree after = replaceTree(tree, edit.range, edit.text);
        changes.push_back(normalisedChange(tree, after, edit.range.begin.value(), edit.range.end.value(), edit.text));
        tree = std::move(after);
    }
    if (!changes.empty()) {
        m_current = Snapshot(std::move(tree), m_storage, ++m_version);
    }
    return changes;
}

Result<TextChange> TextBuffer::restore(const Snapshot& earlier) noexcept {
    if (!earlier.sharesStorageWith(m_current)) {
        return fail(ErrorCode::InvalidArgument, "the snapshot does not come from this buffer");
    }
    // Copy first: `earlier` may alias m_current.
    detail::PieceTree tree = earlier.pieceTree();
    const TextSummary& oldTotal = m_current.summary();
    const TextSummary& newTotal = tree.summary();
    const TextChange change{ByteOffset{0},
                            ByteOffset{oldTotal.bytes},
                            ByteOffset{newTotal.bytes},
                            Point{},
                            Point{LineIndex{oldTotal.lineBreaks}, ByteColumn{oldTotal.lastLineBytes}},
                            Point{LineIndex{newTotal.lineBreaks}, ByteColumn{newTotal.lastLineBytes}}};
    m_current = Snapshot(std::move(tree), m_storage, ++m_version);
    return change;
}

} // namespace frost::text
