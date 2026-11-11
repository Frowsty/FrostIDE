// SPDX-License-Identifier: Apache-2.0
//
// UndoDriver (undo_ops.hpp): random operations against Document and UndoOracle, with per-op and
// full checks, the seeded runs and their reproduction settings.

#include "text/support/undo_ops.hpp"

#include "undo_tree.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace frost::text::test {
namespace {

constexpr std::string_view kKindNames[] = {"Other", "Typing", "DeleteBackward", "DeleteForward", "Paste", "External"};
constexpr std::string_view kOriginNames[] = {"Edit", "Undo", "Redo", "Rollback", "Composition"};

std::string_view name(EditKind kind) { return kKindNames[static_cast<std::size_t>(kind)]; }
std::string_view name(ChangeOrigin origin) { return kOriginNames[static_cast<std::size_t>(origin)]; }

// A printable rendering of up to 16 bytes.
std::string escape(std::string_view bytes) {
    std::string out;
    for (const char c : bytes.substr(0, 16)) {
        const auto b = static_cast<unsigned char>(c);
        if (b >= 0x20 && b < 0x7F && c != '\\') {
            out += c;
        } else {
            out += std::format("\\x{:02X}", b);
        }
    }
    return bytes.size() > 16 ? out + "..." : out;
}

std::string describe(const OracleSelections& s) {
    std::string out = "{";
    for (std::size_t i = 0; i < s.items.size(); ++i) {
        out +=
            std::format("{}{}{}->{}", i > 0 ? " " : "", i == s.primary ? "*" : "", s.items[i].anchor, s.items[i].head);
    }
    return out + "}";
}

std::string describe(std::span<const RawEdit> edits) {
    std::string out = "[";
    for (const RawEdit& e : edits) {
        out += std::format("{{[{}, {}) {}}}", e.range.begin, e.range.end, e.newBytes);
    }
    return out + "]";
}

bool isBoundary(std::string_view text, std::uint64_t x) noexcept {
    return x == text.size() || (x < text.size() && (static_cast<unsigned char>(text[x]) & 0xC0u) != 0x80u);
}
std::uint64_t floorBoundary(std::string_view text, std::uint64_t x) noexcept {
    x = std::min<std::uint64_t>(x, text.size());
    while (x > 0 && !isBoundary(text, x)) {
        --x;
    }
    return x;
}
std::uint64_t ceilBoundary(std::string_view text, std::uint64_t x) noexcept {
    x = std::min<std::uint64_t>(x, text.size());
    while (!isBoundary(text, x)) {
        ++x;
    }
    return x;
}

// Short typed text: mostly one ASCII letter, sometimes a token of 1-4 bytes.
std::string typedText(ByteSource& source) {
    if (source.next(8) != 0) {
        return std::string(1, static_cast<char>('a' + source.next(26)));
    }
    return tokenText(source, 1 + source.next(4));
}

// The final offsets of the raw edits' new ranges: [begin + shift, begin + shift + newBytes).
std::vector<ByteRange> finalRanges(std::span<const RawEdit> edits) {
    std::vector<ByteRange> out;
    std::int64_t shift = 0;
    for (const RawEdit& e : edits) {
        const std::uint64_t begin =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(e.range.begin.value()) + shift);
        out.push_back(ByteRange{ByteOffset{begin}, ByteOffset{begin + e.newBytes}});
        shift += static_cast<std::int64_t>(e.newBytes) - static_cast<std::int64_t>(e.range.size());
    }
    return out;
}

// The checks of one D8 step: points, CRLF interiors and the LSP replay with `replacement`.
std::optional<std::string> checkStep(std::string_view before, std::string_view after, const TextChange& c,
                                     std::string_view replacement) {
    const std::uint64_t s = c.start.value();
    const std::uint64_t eo = c.oldEnd.value();
    const std::uint64_t en = c.newEnd.value();
    if (s > eo || eo > before.size() || en > after.size()) {
        return std::format("change [{}, {}) -> {} is outside the texts", s, eo, en);
    }
    if (crlfInterior(before, s) || crlfInterior(before, eo) || crlfInterior(after, en)) {
        return std::format("change [{}, {}) -> {} has a CRLF-interior bound", s, eo, en);
    }
    if (!(c.startPoint == linearPointAt(before, s)) || !(c.oldEndPoint == linearPointAt(before, eo)) ||
        !(c.newEndPoint == linearPointAt(after, en))) {
        return std::format("change [{}, {}) -> {} has wrong points", s, eo, en);
    }
    const std::uint64_t lspStart = linearLspOffset(before, c.startPoint);
    const std::uint64_t lspEnd = linearLspOffset(before, c.oldEndPoint);
    std::string replayed(before.substr(0, static_cast<std::size_t>(lspStart)));
    replayed += replacement;
    replayed += before.substr(static_cast<std::size_t>(lspEnd));
    if (replayed != after) {
        return std::format("change [{}, {}) -> {} does not replay as an LSP edit", s, eo, en);
    }
    return std::nullopt;
}

constexpr std::string_view kSeedVariable = "FROST_UNDO_SEED";
constexpr std::string_view kOpsVariable = "FROST_UNDO_OPS";
constexpr std::string_view kCheckEveryVariable = "FROST_UNDO_CHECK_EVERY";
constexpr std::string_view kThreadsVariable = "FROST_UNDO_THREADS";

// A tripwire: a new UndoSeedRun field must also go into undoQuickSettings, reproduceCommand and
// the round-trip test.
static_assert(sizeof(UndoSeedRun) == 16, "UndoSeedRun changed: update undoQuickSettings and reproduceCommand");

Result<std::optional<std::uint64_t>> readNumber(const EnvironmentLookup& lookup, std::string_view variable,
                                                std::uint64_t min, std::uint64_t max) {
    const std::string value = lookup(variable);
    if (value.empty()) {
        return std::optional<std::uint64_t>();
    }
    std::uint64_t number = 0;
    const char* const end = value.data() + value.size();
    const auto [stop, error] = std::from_chars(value.data(), end, number);
    if (error != std::errc() || stop != end || number < min || number > max) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("{}={} is not a decimal number in [{}, {}]", variable, value, min, max)));
    }
    return std::optional<std::uint64_t>(number);
}

} // namespace

OracleSelections toOracle(const SelectionSet& selections) {
    OracleSelections out;
    for (const Selection& s : selections.all()) {
        out.items.push_back(OracleSelection{s.anchor.value(), s.head.value()});
    }
    out.primary = selections.primaryIndex();
    return out;
}

SelectionSet toDocument(const OracleSelections& selections) {
    std::vector<Selection> items;
    for (const OracleSelection& s : selections.items) {
        items.push_back(Selection{ByteOffset{s.anchor}, ByteOffset{s.head}});
    }
    return items.empty() ? SelectionSet() : SelectionSet::fromSelections(items, selections.primary);
}

std::optional<std::string> checkLspReplayByExtraction(std::string_view oldText, std::string_view newText,
                                                      std::span<const TextChange> changes) {
    std::string text(oldText);
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const TextChange& c = changes[i];
        std::int64_t shift = 0;
        for (std::size_t j = i + 1; j < changes.size(); ++j) {
            shift += static_cast<std::int64_t>(changes[j].newEnd.value()) -
                     static_cast<std::int64_t>(changes[j].oldEnd.value());
        }
        const std::int64_t from = static_cast<std::int64_t>(c.start.value()) + shift;
        const std::uint64_t length = c.newEnd.value() - c.start.value();
        if (from < 0 || static_cast<std::uint64_t>(from) + length > newText.size() || c.oldEnd.value() > text.size()) {
            return std::format("change {} cannot be extracted from the new text", i);
        }
        const std::string_view replacement =
            newText.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(length));
        std::string next = text.substr(0, static_cast<std::size_t>(c.start.value()));
        next += replacement;
        next += std::string_view(text).substr(static_cast<std::size_t>(c.oldEnd.value()));
        if (std::optional<std::string> failure = checkStep(text, next, c, replacement)) {
            return std::format("change {}: {}", i, *failure);
        }
        text = std::move(next);
    }
    if (text != newText) {
        return std::string("the replayed changes do not give the new text");
    }
    return std::nullopt;
}

UndoDriver::UndoDriver(UndoDriverOptions options, std::string label, const std::string& initial)
    : m_options(options), m_label(std::move(label)),
      m_document(std::move(TextBuffer::fromUtf8(initial, options.buffer)).value(),
                 DocumentOptions{options.coalesceWindow, options.maxTransactions}, m_clock),
      m_oracle(initial, options.buffer.maxBufferBytes, options.coalesceWindow, options.maxTransactions) {
    sched::detail::SplitMix64 rng(0x7AC3ED ^ initial.size());
    for (std::uint32_t i = 0; i < m_options.trackedAnchors; ++i) {
        const std::uint64_t offset = floorBoundary(initial, rng.below(initial.size() + 1));
        const Bias bias = i % 2 == 0 ? Bias::Left : Bias::Right;
        const Result<AnchorId> id = m_document.createAnchor(ByteOffset{offset}, bias);
        const OracleResult<std::uint64_t> expected = m_oracle.createAnchor(offset, bias);
        FROST_ASSERT(id && expected && id->value == *expected);
        m_tracked.push_back(id->value);
    }
}

std::uint64_t UndoDriver::currentNodeId() const noexcept { return m_document.undoTreeForTesting().current; }

std::uint32_t UndoDriver::currentDepth() const noexcept { return m_document.undoTreeForTesting().currentDepth(); }

std::vector<std::uint64_t> UndoDriver::redoChain() const {
    const detail::UndoTree& tree = m_document.undoTreeForTesting();
    std::vector<std::uint64_t> chain;
    for (std::uint64_t at = tree.currentNode().redoChild; at != detail::kNoNode; at = tree.node(at).redoChild) {
        chain.push_back(at);
    }
    return chain;
}

Error UndoDriver::mismatch(const std::string& what) const {
    std::string message = std::format("{}: op {}: {}\nlast ops (oldest first):", m_label, m_opIndex, what);
    for (std::size_t i = 0; i < m_recent.size(); ++i) {
        const std::string& entry = m_recent[(m_opIndex + i) % m_recent.size()];
        if (!entry.empty()) {
            message += "\n  " + entry;
        }
    }
    return Error(ErrorCode::Internal, std::move(message));
}

void UndoDriver::record(std::string description) {
    m_recent[m_opIndex % m_recent.size()] = std::format("#{} {}", m_opIndex, description);
}

std::chrono::nanoseconds UndoDriver::now() const noexcept { return m_clock.steadyNow().time_since_epoch(); }

UndoDriver::DocState UndoDriver::state() const {
    return DocState{m_document.version(), m_document.text().size().value(), m_document.undoState(),
                    m_document.compositionRange(), m_document.anchors().size()};
}

std::uint64_t UndoDriver::randomBoundary(ByteSource& source) const {
    return m_oracle.textOracle().floorChar(source.next(m_oracle.text().size() + 1));
}

std::uint64_t UndoDriver::room(std::uint64_t removed) const noexcept {
    const std::uint64_t kept = m_oracle.text().size() - removed;
    return m_options.maxDocBytes > kept ? m_options.maxDocBytes - kept : 0;
}

std::uint64_t UndoDriver::primaryCaret(ByteSource& source) const {
    if (m_oracleSelections.items.empty()) {
        return randomBoundary(source);
    }
    return m_oracleSelections.items[m_oracleSelections.primary].head;
}

void UndoDriver::setSelections(SelectionSet selections, OracleSelections oracle) {
    m_selections = std::move(selections);
    m_oracleSelections = std::move(oracle);
}

Result<void> UndoDriver::checkRejected(const char* what, ErrorCode got, ErrorCode expected, const DocState& before) {
    ++m_stats.rejected;
    if (got != expected) {
        return std::unexpected(
            mismatch(std::format("{}: expected {}, got {}", what, toString(expected), toString(got))));
    }
    const DocState after = state();
    const bool same =
        after.version == before.version && after.size == before.size && after.undo.canUndo == before.undo.canUndo &&
        after.undo.canRedo == before.undo.canRedo && after.undo.redoBranches == before.undo.redoBranches &&
        after.undo.transactions == before.undo.transactions && after.undo.groupOpen == before.undo.groupOpen &&
        after.undo.composing == before.undo.composing && after.undo.stateId == before.undo.stateId &&
        after.composition == before.composition && after.anchors == before.anchors;
    if (!same) {
        return std::unexpected(mismatch(std::format("{}: a rejected op ({}) changed the state", what, toString(got))));
    }
    return {};
}

Result<void> UndoDriver::checkChangeSet(const ChangeSet& got, const OracleChangeSet& expected) {
    if (got.origin != expected.origin || got.baseVersion != expected.baseVersion || got.version != expected.version) {
        return std::unexpected(
            mismatch(std::format("ChangeSet {} {}->{}, expected {} {}->{}", name(got.origin), got.baseVersion,
                                 got.version, name(expected.origin), expected.baseVersion, expected.version)));
    }
    if (got.edits != expected.edits) {
        return std::unexpected(
            mismatch(std::format("ChangeSet edits {}, expected {}", describe(got.edits), describe(expected.edits))));
    }
    if (got.changes.size() != expected.changes.size()) {
        return std::unexpected(
            mismatch(std::format("{} changes, expected {}", got.changes.size(), expected.changes.size())));
    }
    for (std::size_t i = 0; i < got.changes.size(); ++i) {
        const TextChange& c = got.changes[i];
        const ByteChange& e = expected.changes[i];
        if (c.start.value() != e.start || c.oldEnd.value() != e.oldEnd || c.newEnd.value() != e.newEnd) {
            return std::unexpected(mismatch(std::format("change {} is [{}, {}) -> {}, expected [{}, {}) -> {}", i,
                                                        c.start, c.oldEnd, c.newEnd, e.start, e.oldEnd, e.newEnd)));
        }
    }
    const OracleSelections selections = toOracle(got.selections);
    if (!(selections == expected.selections)) {
        return std::unexpected(mismatch(
            std::format("ChangeSet selections {}, expected {}", describe(selections), describe(expected.selections))));
    }
    // The text around each change.
    const std::string& text = m_oracle.text();
    const Snapshot& snapshot = m_document.text();
    if (snapshot.size().value() != text.size()) {
        return std::unexpected(mismatch(std::format("size {}, expected {}", snapshot.size(), text.size())));
    }
    for (const ByteRange& r : finalRanges(got.edits)) {
        const std::uint64_t a = r.begin.value() > 8 ? r.begin.value() - 8 : 0;
        const std::uint64_t b = std::min<std::uint64_t>(text.size(), r.end.value() + 8);
        if (snapshot.text(ByteRange{ByteOffset{a}, ByteOffset{b}}) !=
            std::string_view(text).substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a))) {
            return std::unexpected(mismatch(std::format("the text around [{}, {}) differs", r.begin, r.end)));
        }
    }
    return {};
}

Result<void> UndoDriver::compare(const char* what, const Result<ChangeSet>& got,
                                 const OracleResult<OracleChangeSet>& expected, const DocState& before,
                                 std::string oldText) {
    if (!expected) {
        if (got) {
            return std::unexpected(
                mismatch(std::format("{}: expected {}, but it succeeded", what, toString(expected.error()))));
        }
        return checkRejected(what, got.error().code(), expected.error(), before);
    }
    if (!got) {
        return std::unexpected(mismatch(std::format("{} failed: {}", what, got.error().describe())));
    }
    if (Result<void> checked = checkChangeSet(*got, *expected); !checked) {
        return checked;
    }
    if (got->version != got->baseVersion) {
        const bool history = got->origin == ChangeOrigin::Undo || got->origin == ChangeOrigin::Redo ||
                             got->origin == ChangeOrigin::Rollback;
        if (history && m_options.replayHistoryOps) {
            ++m_stats.replayChecks;
            if (std::optional<std::string> failure =
                    checkLspReplayByExtraction(oldText, m_document.text().text(), got->changes)) {
                return std::unexpected(mismatch(std::format("{} ChangeSet: {}", what, *failure)));
            }
        }
        m_last = LastChange{true, got->origin, std::move(oldText), expected->edits, expected->inserted, got->changes};
        m_step.changed = true;
    }
    m_step.changeSet = *got;
    return {};
}

Result<void> UndoDriver::compareVoid(const char* what, const Result<void>& got,
                                     const std::optional<ErrorCode>& expected, const DocState& before) {
    if (expected) {
        if (got) {
            return std::unexpected(
                mismatch(std::format("{}: expected {}, but it succeeded", what, toString(*expected))));
        }
        return checkRejected(what, got.error().code(), *expected, before);
    }
    if (!got) {
        return std::unexpected(mismatch(std::format("{} failed: {}", what, got.error().describe())));
    }
    return {};
}

Result<void> UndoDriver::checkAfterOp() {
    if (m_document.version() != m_oracle.version()) {
        return std::unexpected(
            mismatch(std::format("version {}, expected {}", m_document.version(), m_oracle.version())));
    }
    const UndoState got = m_document.undoState();
    const OracleUndoState expected = m_oracle.undoState();
    const OracleUndoState converted{got.canUndo,   got.canRedo,   got.redoBranches, got.transactions,
                                    got.groupOpen, got.composing, got.stateId};
    if (!(converted == expected)) {
        return std::unexpected(
            mismatch(std::format("undoState undo {} redo {} branches {} transactions {} group {} composing {} stateId "
                                 "{}, expected {} {} {} {} "
                                 "{} {} {}",
                                 got.canUndo, got.canRedo, got.redoBranches, got.transactions, got.groupOpen,
                                 got.composing, got.stateId, expected.canUndo, expected.canRedo, expected.redoBranches,
                                 expected.transactions, expected.groupOpen, expected.composing, expected.stateId)));
    }
    const AnchorSet& anchors = m_document.anchors();
    if (anchors.size() != m_oracle.anchors().size()) {
        return std::unexpected(
            mismatch(std::format("{} anchors, expected {}", anchors.size(), m_oracle.anchors().size())));
    }
    for (const OracleAnchor& a : m_oracle.anchors()) {
        const Result<Anchor> resolved = anchors.anchor(AnchorId{a.id});
        if (!resolved || resolved->offset.value() != a.offset || resolved->bias != a.bias) {
            return std::unexpected(mismatch(std::format("anchor {} at {}, expected {} ({})", a.id,
                                                        resolved ? resolved->offset.value() : ~0ull, a.offset,
                                                        a.bias == Bias::Left ? "Left" : "Right")));
        }
    }
    m_stats.maxBytes = std::max<std::uint64_t>(m_stats.maxBytes, m_oracle.text().size());
    m_stats.maxNodes = std::max<std::uint64_t>(m_stats.maxNodes, m_oracle.nodeCount());
    return {};
}

Result<void> UndoDriver::runEdit(const char* what, std::vector<OracleEdit> edits, EditKind kind,
                                 const SelectionSet& after, const OracleSelections& oracleAfter, bool viaReplace) {
    std::vector<Edit> docEdits;
    for (const OracleEdit& e : edits) {
        docEdits.push_back(Edit{ByteRange{ByteOffset{e.a}, ByteOffset{e.b}}, e.text});
    }
    record(std::format("{} {} {} edits, first [{}, {}) \"{}\"", what, name(kind), edits.size(),
                       edits.empty() ? 0 : edits[0].a, edits.empty() ? 0 : edits[0].b,
                       edits.empty() ? std::string() : escape(edits[0].text)));
    const DocState before = state();
    std::string oldText = m_oracle.text();
    const EditContext context{kind, m_selections, after};
    const Result<ChangeSet> got = viaReplace && docEdits.size() == 1
                                      ? m_document.replace(docEdits[0].range, docEdits[0].text, context)
                                      : m_document.edit(docEdits, context);
    const OracleResult<OracleChangeSet> expected = m_oracle.edit(edits, kind, m_oracleSelections, oracleAfter, now());
    if (Result<void> compared = compare(what, got, expected, before, std::move(oldText)); !compared) {
        return compared;
    }
    if (got) {
        ++m_stats.edits;
        if (got->edits.empty()) {
            ++m_stats.noOps;
        } else if (!m_oracle.groupOpen()) {
            m_step.recordedSelections = after;
        }
        for (const ByteRange& r : finalRanges(got->edits)) {
            m_edges.push_back(r.begin.value());
            m_edges.push_back(r.end.value());
        }
        if (m_edges.size() > 16) {
            m_edges.erase(m_edges.begin(), m_edges.end() - 16);
        }
        setSelections(after, oracleAfter);
    }
    return {};
}

Result<void> UndoDriver::typing(ByteSource& source) {
    if (m_oracleSelections.items.empty() || source.next(16) == 0) {
        const std::uint64_t at = randomBoundary(source);
        setSelections(SelectionSet::caret(ByteOffset{at}), OracleSelections{{OracleSelection{at, at}}, 0});
    }
    const OracleSelection& primary = m_oracleSelections.items[m_oracleSelections.primary];
    const std::uint64_t a = primary.lo();
    const std::uint64_t b = primary.hi();
    if (room(b - a) == 0 || (m_oracle.text().size() * 4 > m_options.maxDocBytes * 3 && source.next(2) == 0)) {
        return deletion(source);
    }
    std::string text = typedText(source);
    if (text.size() > room(b - a)) {
        text = "a";
    }
    const RawEdit raw{ByteRange{ByteOffset{a}, ByteOffset{b}}, text.size()};
    const SelectionSet after = m_selections.transformed(std::span<const RawEdit>(&raw, 1));
    const OracleSelections oracleAfter = oracleTransform(m_oracleSelections, std::span<const RawEdit>(&raw, 1));
    if (!(toOracle(after) == oracleAfter)) {
        return std::unexpected(mismatch(
            std::format("transformed selections {}, expected {}", describe(toOracle(after)), describe(oracleAfter))));
    }
    return runEdit("type", {OracleEdit{a, b, std::move(text)}}, EditKind::Typing, after, oracleAfter,
                   source.next(2) == 0);
}

Result<void> UndoDriver::deletion(ByteSource& source) {
    if (m_oracleSelections.items.empty()) {
        const std::uint64_t at = randomBoundary(source);
        setSelections(SelectionSet::caret(ByteOffset{at}), OracleSelections{{OracleSelection{at, at}}, 0});
    }
    const OracleSelection& primary = m_oracleSelections.items[m_oracleSelections.primary];
    const bool backward = source.next(3) != 0;
    std::uint64_t a = primary.lo();
    std::uint64_t b = primary.hi();
    const std::string& text = m_oracle.text();
    if (a == b) {
        if (backward) {
            a = a == 0 ? 0 : floorBoundary(text, a - 1);
        } else {
            b = b == text.size() ? b : ceilBoundary(text, b + 1);
        }
    }
    const RawEdit raw{ByteRange{ByteOffset{a}, ByteOffset{b}}, 0};
    const SelectionSet after = m_selections.transformed(std::span<const RawEdit>(&raw, 1));
    const OracleSelections oracleAfter = oracleTransform(m_oracleSelections, std::span<const RawEdit>(&raw, 1));
    return runEdit(backward ? "backspace" : "delete", {OracleEdit{a, b, {}}},
                   backward ? EditKind::DeleteBackward : EditKind::DeleteForward, after, oracleAfter,
                   source.next(2) == 0);
}

Result<void> UndoDriver::replaceOp(ByteSource& source) {
    std::uint64_t a = randomBoundary(source);
    std::uint64_t b = source.next(3) == 0 ? a : randomBoundary(source);
    if (a > b) {
        std::swap(a, b);
    }
    if (b - a > 512 && source.next(2) == 0) {
        b = floorBoundary(m_oracle.text(), a + source.next(512));
    }
    const std::uint64_t r = source.next(100);
    std::uint64_t length = r < 80 ? source.next(16) : r < 98 ? source.next(256) : source.next(2048);
    length = std::min(length, room(b - a));
    std::string text = tokenText(source, length);
    constexpr EditKind kinds[] = {EditKind::Other, EditKind::Paste, EditKind::External};
    const EditKind kind = kinds[source.next(3)];
    const std::uint64_t caret = a + text.size();
    return runEdit("replace", {OracleEdit{a, b, std::move(text)}}, kind, SelectionSet::caret(ByteOffset{caret}),
                   OracleSelections{{OracleSelection{caret, caret}}, 0}, source.next(2) == 0);
}

Result<void> UndoDriver::moveSelections(ByteSource& source) {
    const std::size_t count = 1 + static_cast<std::size_t>(source.next(m_options.maxCarets));
    std::vector<Selection> items;
    std::vector<OracleSelection> oracleItems;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t anchor = randomBoundary(source);
        std::uint64_t head = anchor;
        if (source.next(5) < 2) {
            head =
                source.next(2) == 0 ? randomBoundary(source) : floorBoundary(m_oracle.text(), anchor + source.next(16));
        }
        items.push_back(Selection{ByteOffset{anchor}, ByteOffset{head}});
        oracleItems.push_back(OracleSelection{anchor, head});
    }
    const std::size_t primary = static_cast<std::size_t>(source.next(count));
    std::vector<std::uint32_t> origins;
    std::vector<std::uint32_t> oracleOrigins;
    SelectionSet selections = SelectionSet::fromSelections(items, primary, &origins);
    OracleSelections oracle = oracleNormalise(oracleItems, primary, &oracleOrigins);
    record(std::format("move {} selections to {}", count, describe(oracle)));
    if (!(toOracle(selections) == oracle) || origins != oracleOrigins) {
        return std::unexpected(mismatch(
            std::format("normalised selections {}, expected {}", describe(toOracle(selections)), describe(oracle))));
    }
    if (Result<void> checked = selections.checkInvariants(); !checked) {
        return std::unexpected(mismatch(checked.error().message()));
    }
    setSelections(std::move(selections), std::move(oracle));
    return {};
}

Result<void> UndoDriver::multiCaret(ByteSource& source) {
    if (m_oracleSelections.items.size() < 2 || source.next(2) == 0) {
        if (Result<void> moved = moveSelections(source); !moved) {
            return moved;
        }
    }
    const std::string text = source.next(4) == 0 ? tokenText(source, 1 + source.next(2)) : std::string("x");
    std::vector<OracleEdit> edits;
    std::vector<RawEdit> raw;
    std::uint64_t removed = 0;
    for (const OracleSelection& s : m_oracleSelections.items) {
        edits.push_back(OracleEdit{s.lo(), s.hi(), text});
        raw.push_back(RawEdit{ByteRange{ByteOffset{s.lo()}, ByteOffset{s.hi()}}, text.size()});
        removed += s.hi() - s.lo();
    }
    if (text.size() * edits.size() > room(removed)) {
        return deletion(source);
    }
    const SelectionSet after = m_selections.transformed(raw);
    const OracleSelections oracleAfter = oracleTransform(m_oracleSelections, raw);
    if (!(toOracle(after) == oracleAfter)) {
        return std::unexpected(mismatch(
            std::format("transformed selections {}, expected {}", describe(toOracle(after)), describe(oracleAfter))));
    }
    return runEdit("multi-caret", std::move(edits), EditKind::Typing, after, oracleAfter, false);
}

Result<void> UndoDriver::undoOp() {
    record("undo");
    const DocState before = state();
    std::string oldText = m_oracle.text();
    const Result<ChangeSet> got = m_document.undo();
    const OracleResult<OracleChangeSet> expected = m_oracle.undo();
    if (Result<void> compared = compare("undo", got, expected, before, std::move(oldText)); !compared) {
        return compared;
    }
    if (got) {
        ++m_stats.undos;
        setSelections(got->selections, expected->selections);
    }
    return {};
}

Result<void> UndoDriver::redoOp(std::optional<std::uint32_t> branch) {
    record(branch ? std::format("redo branch {}", *branch) : std::string("redo"));
    const DocState before = state();
    std::string oldText = m_oracle.text();
    const Result<ChangeSet> got = m_document.redo(branch);
    const OracleResult<OracleChangeSet> expected = m_oracle.redo(branch);
    if (Result<void> compared = compare("redo", got, expected, before, std::move(oldText)); !compared) {
        return compared;
    }
    if (got) {
        ++m_stats.redos;
        m_step.recordedSelections = got->selections;
        setSelections(got->selections, expected->selections);
    }
    return {};
}

Result<void> UndoDriver::undoOnce() {
    ++m_opIndex;
    const std::uint64_t nodeBefore = currentNodeId();
    m_step = UndoStepInfo{"undo", false, {}, {}, nodeBefore, 0};
    Result<void> result = undoOp();
    if (result) {
        result = checkAfterOp();
    }
    noteStep(nodeBefore);
    return result;
}

void UndoDriver::noteStep(std::uint64_t nodeBefore) {
    m_step.nodeBefore = nodeBefore;
    m_step.nodeAfter = currentNodeId();
    if (m_step.recordedSelections) {
        m_recordedByNode.insert_or_assign(m_step.nodeAfter, *m_step.recordedSelections);
    }
    if (m_step.changeSet && m_step.changeSet->origin == ChangeOrigin::Undo) {
        m_undoneByNode.insert_or_assign(nodeBefore, m_step.changeSet->selections);
    }
    // Forget pruned and rolled-back nodes now and then, so long runs stay O(live nodes) in memory.
    const detail::UndoTree& tree = m_document.undoTreeForTesting();
    const std::size_t bound = 2 * static_cast<std::size_t>(tree.nodeCount) + 64;
    for (std::map<std::uint64_t, SelectionSet>* byNode : {&m_recordedByNode, &m_undoneByNode}) {
        if (byNode->size() > bound) {
            std::erase_if(*byNode, [&](const auto& entry) { return !tree.contains(entry.first); });
        }
    }
}

std::optional<SelectionSet> UndoDriver::recordedSelectionsOf(std::uint64_t id) const {
    const auto found = m_recordedByNode.find(id);
    return found == m_recordedByNode.end() ? std::nullopt : std::optional<SelectionSet>(found->second);
}

std::optional<SelectionSet> UndoDriver::undoSelectionsOf(std::uint64_t id) const {
    const auto found = m_undoneByNode.find(id);
    return found == m_undoneByNode.end() ? std::nullopt : std::optional<SelectionSet>(found->second);
}

Result<void> UndoDriver::redoOnce(std::optional<std::uint32_t> branch) {
    ++m_opIndex;
    const std::uint64_t nodeBefore = currentNodeId();
    m_step = UndoStepInfo{"redo", false, {}, {}, nodeBefore, 0};
    Result<void> result = redoOp(branch);
    if (result) {
        result = checkAfterOp();
    }
    noteStep(nodeBefore);
    return result;
}

Result<void> UndoDriver::endGroupOp() {
    record("endGroup");
    const DocState before = state();
    const bool outermost = m_oracle.groupDepth() == 1;
    const bool hasNode = outermost && m_document.undoTreeForTesting().groupNode != detail::kNoNode;
    const Result<void> got = m_document.endGroup(m_selections);
    const std::optional<ErrorCode> expected = m_oracle.endGroup(m_oracleSelections);
    if (Result<void> compared = compareVoid("endGroup", got, expected, before); !compared) {
        return compared;
    }
    if (got && hasNode) {
        m_step.recordedSelections = m_selections;
    }
    return {};
}

Result<void> UndoDriver::rollbackOp() {
    record("rollbackGroup");
    const DocState before = state();
    std::string oldText = m_oracle.text();
    const Result<ChangeSet> got = m_document.rollbackGroup();
    const OracleResult<OracleChangeSet> expected = m_oracle.rollbackGroup();
    if (Result<void> compared = compare("rollbackGroup", got, expected, before, std::move(oldText)); !compared) {
        return compared;
    }
    if (got) {
        ++m_stats.rollbacks;
        setSelections(got->selections, expected->selections);
    }
    return {};
}

Result<void> UndoDriver::groupOp(ByteSource& source) {
    const std::uint32_t depth = m_oracle.groupDepth();
    const std::uint64_t r = source.next(4);
    if (depth == 0 || (r == 0 && depth < 3)) {
        constexpr EditKind kinds[] = {EditKind::Other, EditKind::Typing, EditKind::Paste};
        const EditKind kind = kinds[source.next(3)];
        record(std::format("beginGroup {} at depth {}", name(kind), depth));
        const DocState before = state();
        const Result<void> got = m_document.beginGroup(EditContext{kind, m_selections, {}});
        const std::optional<ErrorCode> expected = m_oracle.beginGroup(kind, m_oracleSelections);
        if (got && depth == 0) {
            ++m_stats.groups;
        }
        return compareVoid("beginGroup", got, expected, before);
    }
    return r <= 2 ? endGroupOp() : rollbackOp();
}

Result<void> UndoDriver::cancelOp() {
    record("cancelComposition");
    const DocState before = state();
    std::string oldText = m_oracle.text();
    const Result<ChangeSet> got = m_document.cancelComposition();
    const OracleResult<OracleChangeSet> expected = m_oracle.cancelComposition();
    if (got) {
        ++m_stats.cancels;
    }
    return compare("cancelComposition", got, expected, before, std::move(oldText));
}

Result<void> UndoDriver::commitOp(ByteSource& source, bool withRange) {
    const std::optional<std::pair<std::uint64_t, std::uint64_t>> composition = m_oracle.compositionRange();
    const std::string& base = m_oracle.committedText();
    std::optional<std::pair<std::uint64_t, std::uint64_t>> replace;
    std::uint64_t rb = composition ? composition->first : 0;
    std::uint64_t re = rb;
    if (withRange && composition) {
        rb = floorBoundary(base, rb > 3 ? rb - source.next(4) : rb - source.next(rb + 1));
        re = ceilBoundary(base, std::min<std::uint64_t>(base.size(), re + source.next(4)));
        replace = std::pair(rb, re);
    }
    std::string text;
    if (source.next(10) != 0) {
        const std::uint64_t space = m_options.maxDocBytes > base.size() ? m_options.maxDocBytes - base.size() : 0;
        text = tokenText(source, std::min<std::uint64_t>(space + (re - rb), 1 + source.next(6)));
    }
    record(std::format("commit \"{}\" replacing [{}, {})", escape(text), rb, re));
    const RawEdit net{ByteRange{ByteOffset{rb}, ByteOffset{re}}, text.size()};
    SelectionSet after = m_selections.transformed(std::span<const RawEdit>(&net, 1));
    OracleSelections oracleAfter = oracleTransform(m_oracleSelections, std::span<const RawEdit>(&net, 1));
    const DocState before = state();
    std::string oldText = m_oracle.text();
    const Result<ChangeSet> got = m_document.commitComposition(
        text, EditContext{EditKind::Other, m_selections, after},
        replace ? std::optional<ByteRange>(ByteRange{ByteOffset{rb}, ByteOffset{re}}) : std::nullopt);
    const OracleResult<OracleChangeSet> expected =
        m_oracle.commitComposition(text, m_oracleSelections, oracleAfter, replace, now());
    if (Result<void> compared = compare("commitComposition", got, expected, before, std::move(oldText)); !compared) {
        return compared;
    }
    if (got && got->origin == ChangeOrigin::Edit) {
        ++m_stats.commits;
        m_step.recordedSelections = after;
        setSelections(std::move(after), std::move(oracleAfter));
    } else if (got) {
        ++m_stats.cancels;
    }
    return {};
}

Result<void> UndoDriver::imeOp(ByteSource& source) {
    if (!m_oracle.composing()) {
        const std::uint64_t at = source.next(4) == 0 ? randomBoundary(source) : primaryCaret(source);
        record(std::format("beginComposition at {}", at));
        const DocState before = state();
        const Result<void> got = m_document.beginComposition(ByteOffset{at});
        const std::optional<ErrorCode> expected = m_oracle.beginComposition(at);
        return compareVoid("beginComposition", got, expected, before);
    }
    const std::uint64_t r = source.next(10);
    if (r < 5) {
        const std::uint64_t base = m_oracle.committedText().size();
        const std::uint64_t space = m_options.maxDocBytes > base ? m_options.maxDocBytes - base : 0;
        const std::string preedit = tokenText(source, std::min<std::uint64_t>(space, source.next(7)));
        record(std::format("updateComposition \"{}\"", escape(preedit)));
        const DocState before = state();
        std::string oldText = m_oracle.text();
        const Result<ChangeSet> got = m_document.updateComposition(preedit);
        const OracleResult<OracleChangeSet> expected = m_oracle.updateComposition(preedit);
        if (got) {
            ++m_stats.preedits;
        }
        return compare("updateComposition", got, expected, before, std::move(oldText));
    }
    if (r < 8) {
        return commitOp(source, source.next(2) == 0);
    }
    return cancelOp();
}

Result<void> UndoDriver::createAnchorOp(ByteSource& source) {
    if (m_churn.size() >= m_options.maxChurnAnchors) {
        return releaseAnchorOp(source);
    }
    const std::string& text = m_oracle.text();
    std::uint64_t offset = randomBoundary(source);
    const std::uint64_t r = source.next(4);
    if (r == 0 && !m_edges.empty()) {
        offset = floorBoundary(text, m_edges[static_cast<std::size_t>(source.next(m_edges.size()))]);
    } else if (r == 1) {
        const std::size_t crlf = text.find("\r\n", static_cast<std::size_t>(source.next(text.size() + 1)));
        if (crlf != std::string::npos) {
            offset = crlf + 1;
        }
    }
    const Bias bias = source.next(2) == 0 ? Bias::Left : Bias::Right;
    record(std::format("createAnchor {} {}", offset, bias == Bias::Left ? "Left" : "Right"));
    const DocState before = state();
    const Result<AnchorId> got = m_document.createAnchor(ByteOffset{offset}, bias);
    const OracleResult<std::uint64_t> expected = m_oracle.createAnchor(offset, bias);
    if (!expected) {
        if (got) {
            return std::unexpected(
                mismatch("createAnchor succeeded, expected " + std::string(toString(expected.error()))));
        }
        return checkRejected("createAnchor", got.error().code(), expected.error(), before);
    }
    if (!got || got->value != *expected) {
        return std::unexpected(
            mismatch(std::format("createAnchor gave {}, expected id {}",
                                 got ? std::to_string(got->value) : got.error().describe(), *expected)));
    }
    ++m_stats.anchorOps;
    m_churn.push_back(*expected);
    m_step.changed = true;
    return {};
}

Result<void> UndoDriver::releaseAnchorOp(ByteSource& source) {
    std::uint64_t id = 0;
    if (!m_released.empty() && (m_churn.empty() || source.next(8) == 0)) {
        id = m_released[static_cast<std::size_t>(source.next(m_released.size()))]; // NotFound.
    } else if (!m_churn.empty()) {
        const std::size_t index = static_cast<std::size_t>(source.next(m_churn.size()));
        id = m_churn[index];
        m_churn.erase(m_churn.begin() + static_cast<std::ptrdiff_t>(index));
        m_released.push_back(id);
        if (m_released.size() > 16) {
            m_released.erase(m_released.begin());
        }
    } else {
        id = (std::uint64_t{1} << 40) + source.next(1000); // Never created: NotFound.
    }
    record(std::format("releaseAnchor {}", id));
    const DocState before = state();
    const Result<void> got = m_document.releaseAnchor(AnchorId{id});
    const std::optional<ErrorCode> expected = m_oracle.releaseAnchor(id);
    if (got) {
        ++m_stats.anchorOps;
        m_step.changed = true;
    }
    return compareVoid("releaseAnchor", got, expected, before);
}

Result<void> UndoDriver::invalidOp(ByteSource& source) {
    const std::string& text = m_oracle.text();
    const std::uint64_t n = text.size();
    // An offset inside a UTF-8 sequence, if the text has one near a random point.
    std::optional<std::uint64_t> inside;
    for (std::uint64_t x = source.next(n + 1), tries = 0; tries < 64 && x < n; ++x, ++tries) {
        if (!isBoundary(text, x)) {
            inside = x;
            break;
        }
    }
    const SelectionSet& keep = m_selections;
    switch (source.next(14)) {
    case 0: {
        record("invalid: createAnchor beyond the text");
        const DocState before = state();
        const Result<AnchorId> got = m_document.createAnchor(ByteOffset{n + 1 + source.next(4)}, Bias::Left);
        if (got) {
            return std::unexpected(mismatch("createAnchor beyond the text succeeded"));
        }
        const OracleResult<std::uint64_t> expected = m_oracle.createAnchor(n + 1, Bias::Left);
        return checkRejected("createAnchor", got.error().code(), expected.error(), before);
    }
    case 1: {
        if (!inside) {
            return releaseAnchorOp(source);
        }
        record(std::format("invalid: createAnchor inside a sequence at {}", *inside));
        const DocState before = state();
        const Result<AnchorId> got = m_document.createAnchor(ByteOffset{*inside}, Bias::Right);
        if (got) {
            return std::unexpected(mismatch("createAnchor inside a UTF-8 sequence succeeded"));
        }
        const OracleResult<std::uint64_t> expected = m_oracle.createAnchor(*inside, Bias::Right);
        return checkRejected("createAnchor", got.error().code(), expected.error(), before);
    }
    case 2: {
        // begin > end, end > size, a bound inside a sequence, or ill-formed text.
        const std::uint64_t which = source.next(4);
        std::uint64_t a = randomBoundary(source);
        std::uint64_t b = a;
        std::string inserted = "q";
        if (which == 0 && n > 0) {
            a = std::max<std::uint64_t>(a, 1);
            b = a - 1;
        } else if (which == 1) {
            b = n + 1 + source.next(3);
        } else if (which == 2 && inside) {
            a = *inside;
            b = ceilBoundary(text, a);
        } else {
            inserted = "\xFF";
        }
        return runEdit("invalid edit", {OracleEdit{a, b, inserted}}, EditKind::Typing, keep, m_oracleSelections,
                       source.next(2) == 0);
    }
    case 3: {
        // Over the buffer limit (meaningful with small limits; otherwise a large valid insert is
        // not generated and this becomes an out-of-order batch).
        const std::uint64_t limit = m_options.buffer.maxBufferBytes;
        if (limit <= 64 * 1024 && limit >= n) {
            const std::uint64_t at = randomBoundary(source);
            return runEdit("invalid over-limit insert", {OracleEdit{at, at, std::string(limit - n + 1, 'z')}},
                           EditKind::Typing, keep, m_oracleSelections, source.next(2) == 0);
        }
        [[fallthrough]];
    }
    case 4: {
        std::uint64_t a = randomBoundary(source);
        std::uint64_t b = randomBoundary(source);
        if (a > b) {
            std::swap(a, b);
        }
        if (a == b) {
            return runEdit("two inserts at one offset", {OracleEdit{a, a, "x"}, OracleEdit{a, a, "y"}},
                           EditKind::Typing, keep, m_oracleSelections, false); // Valid, as a contrast.
        }
        return runEdit("invalid unsorted batch", {OracleEdit{a, b, "x"}, OracleEdit{a, a, "y"}}, EditKind::Other, keep,
                       m_oracleSelections, false);
    }
    case 5: return redoOp(static_cast<std::uint32_t>(m_oracle.undoState().redoBranches + source.next(3)));
    case 6: return m_oracle.groupOpen() ? undoOp() : endGroupOp();
    case 7: return m_oracle.groupOpen() ? redoOp(std::nullopt) : rollbackOp();
    case 8: {
        record("invalid: beginComposition beyond the text");
        const DocState before = state();
        const Result<void> got = m_document.beginComposition(ByteOffset{n + 1});
        const std::optional<ErrorCode> expected = m_oracle.beginComposition(n + 1);
        return compareVoid("beginComposition", got, expected, before);
    }
    case 9: {
        if (!inside) {
            return cancelOp();
        }
        record(std::format("invalid: beginComposition inside a sequence at {}", *inside));
        const DocState before = state();
        const Result<void> got = m_document.beginComposition(ByteOffset{*inside});
        const std::optional<ErrorCode> expected = m_oracle.beginComposition(*inside);
        return compareVoid("beginComposition", got, expected, before);
    }
    case 10: {
        record("invalid: updateComposition");
        const DocState before = state();
        std::string oldText = m_oracle.text();
        const std::string preedit = source.next(2) == 0 ? std::string("\xC3") : std::string("p");
        const Result<ChangeSet> got = m_document.updateComposition(preedit);
        const OracleResult<OracleChangeSet> expected = m_oracle.updateComposition(preedit);
        return compare("updateComposition", got, expected, before, std::move(oldText));
    }
    case 11: {
        // commitComposition with a bad replaceInBase (or no composition).
        const std::string& base = m_oracle.committedText();
        const std::optional<std::pair<std::uint64_t, std::uint64_t>> composition = m_oracle.compositionRange();
        const std::uint64_t start = composition ? composition->first : 0;
        std::pair<std::uint64_t, std::uint64_t> range{start, start};
        switch (source.next(4)) {
        case 0: range = {start + 1, start}; break;
        case 1: range = {start, base.size() + 1}; break;
        case 2:
            range = start < base.size() ? std::pair(ceilBoundary(base, start + 1), ceilBoundary(base, start + 1))
                                        : std::pair(start + 1, start + 1);
            break;
        default: {
            std::optional<std::uint64_t> bad;
            for (std::uint64_t x = start; x < base.size() && x < start + 8; ++x) {
                if (!isBoundary(base, x)) {
                    bad = x;
                    break;
                }
            }
            range = bad ? std::pair(start, *bad) : std::pair(start + 1, start);
            break;
        }
        }
        record(std::format("invalid: commitComposition replacing [{}, {})", range.first, range.second));
        const DocState before = state();
        std::string oldText = m_oracle.text();
        const Result<ChangeSet> got =
            m_document.commitComposition("c", EditContext{EditKind::Typing, m_selections, m_selections},
                                         ByteRange{ByteOffset{range.first}, ByteOffset{range.second}});
        const OracleResult<OracleChangeSet> expected =
            m_oracle.commitComposition("c", m_oracleSelections, m_oracleSelections, range, now());
        if (expected) {
            return std::unexpected(mismatch("the invalid commit was expected to succeed (driver bug)"));
        }
        return compare("commitComposition", got, expected, before, std::move(oldText));
    }
    case 12: return m_oracle.composing() ? imeOp(source) : cancelOp();
    default: return releaseAnchorOp(source);
    }
}

Result<void> UndoDriver::dispatch(ByteSource& source) {
    std::uint64_t r = source.next(100);
    // While composing, most edit, history and group ops become IME ops. The selections stay those
    // of beginComposition (base-text coordinates, which a commit records and undo restores), so
    // selection moves become IME ops too and the remaining edits (Busy) go through replaceOp,
    // which leaves the selections alone.
    const bool editLike = r < 45 || (r >= 60 && r < 82);
    if (m_oracle.composing() && ((editLike && source.next(4) != 0) || (r >= 45 && r < 50))) {
        r = 82;
    } else if (m_oracle.composing() && r < 45) {
        r = 32;
    }
    if (r < 22) {
        return typing(source);
    }
    if (r < 32) {
        return deletion(source);
    }
    if (r < 40) {
        return replaceOp(source);
    }
    if (r < 45) {
        return multiCaret(source);
    }
    if (r < 50) {
        return moveSelections(source);
    }
    if (r < 58) {
        const std::uint64_t ms =
            source.next(10) == 0 ? static_cast<std::uint64_t>(m_options.coalesceWindow.count()) : source.next(301);
        record(std::format("advance {} ms", ms));
        m_clock.advance(std::chrono::milliseconds{ms});
        return {};
    }
    if (r < 60) {
        const auto ms = static_cast<std::uint64_t>(m_options.coalesceWindow.count()) + 1 + source.next(500);
        record(std::format("advance {} ms", ms));
        m_clock.advance(std::chrono::milliseconds{ms});
        return {};
    }
    if (r < 70) {
        return undoOp();
    }
    if (r < 76) {
        return redoOp(std::nullopt);
    }
    if (r < 78) {
        return redoOp(static_cast<std::uint32_t>(source.next(m_oracle.undoState().redoBranches + 1)));
    }
    if (r < 82) {
        return groupOp(source);
    }
    if (r < 87) {
        return imeOp(source);
    }
    if (r < 91) {
        return createAnchorOp(source);
    }
    if (r < 93) {
        return releaseAnchorOp(source);
    }
    if (r < 94) {
        record("breakCoalescing");
        m_document.breakCoalescing();
        m_oracle.breakCoalescing();
        return {};
    }
    if (r < 97) {
        return invalidOp(source);
    }
    record("full check");
    return verifyFull();
}

Result<void> UndoDriver::step(ByteSource& source) {
    ++m_opIndex;
    ++m_stats.ops;
    const std::uint64_t nodeBefore = currentNodeId();
    m_step = UndoStepInfo{"op", false, {}, {}, nodeBefore, 0};
    Result<void> result = dispatch(source);
    if (result) {
        result = checkAfterOp();
    }
    noteStep(nodeBefore);
    if (result && m_options.checkEvery != 0 && m_stats.ops % m_options.checkEvery == 0) {
        result = verifyFull();
    }
    return result;
}

Result<void> UndoDriver::settle(ByteSource& source) {
    // One op through the per-op checks and the per-node bookkeeping.
    const auto settleOp = [&](auto&& op) -> Result<void> {
        ++m_opIndex;
        const std::uint64_t nodeBefore = currentNodeId();
        m_step = UndoStepInfo{"settle", false, {}, {}, nodeBefore, 0};
        Result<void> result = op();
        if (result) {
            result = checkAfterOp();
        }
        noteStep(nodeBefore);
        return result;
    };
    while (m_oracle.groupOpen()) {
        const bool rollback = source.next(3) == 0;
        if (Result<void> result = settleOp([&] { return rollback ? rollbackOp() : endGroupOp(); }); !result) {
            return result;
        }
    }
    if (m_oracle.composing()) {
        const bool cancel = source.next(2) == 0;
        const bool withRange = !cancel && source.next(2) == 0;
        if (Result<void> result = settleOp([&] { return cancel ? cancelOp() : commitOp(source, withRange); });
            !result) {
            return result;
        }
        if (m_oracle.composing()) { // The commit failed (over the limit): cancel.
            if (Result<void> result = settleOp([&] { return cancelOp(); }); !result) {
                return result;
            }
        }
    }
    return {};
}

Result<void> UndoDriver::verifyFull() {
    ++m_stats.fullChecks;
    if (Result<void> checked = m_document.checkInvariants(); !checked) {
        return std::unexpected(mismatch("invariants: " + checked.error().message()));
    }
    if (m_document.text().text() != m_oracle.text()) {
        return std::unexpected(mismatch("the text differs"));
    }
    if (m_document.committedText().text() != m_oracle.committedText()) {
        return std::unexpected(mismatch("the committed text differs"));
    }
    std::vector<OracleAnchor> expected = m_oracle.anchors();
    std::sort(expected.begin(), expected.end(), [](const OracleAnchor& x, const OracleAnchor& y) {
        return std::tuple(x.offset, x.bias, x.id) < std::tuple(y.offset, y.bias, y.id);
    });
    const std::vector<ResolvedAnchor> all = m_document.anchors().all();
    if (all.size() != expected.size()) {
        return std::unexpected(
            mismatch(std::format("all() lists {} anchors, expected {}", all.size(), expected.size())));
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].id.value != expected[i].id || all[i].offset.value() != expected[i].offset ||
            all[i].bias != expected[i].bias) {
            return std::unexpected(
                mismatch(std::format("all()[{}] is anchor {} at {}, expected {} at {}", i, all[i].id.value,
                                     all[i].offset, expected[i].id, expected[i].offset)));
        }
    }
    const detail::UndoTree& tree = m_document.undoTreeForTesting();
    if (tree.current != m_oracle.currentNode() || tree.nodeCount != m_oracle.nodeCount() ||
        tree.currentDepth() != m_oracle.currentDepth() || redoChain() != m_oracle.redoChain()) {
        return std::unexpected(mismatch(std::format(
            "tree: current {} of {} nodes at depth {}, expected {} of {} at {}", tree.current, tree.nodeCount,
            tree.currentDepth(), m_oracle.currentNode(), m_oracle.nodeCount(), m_oracle.currentDepth())));
    }
    const std::vector<OracleRegion> regions = m_oracle.currentRegions();
    const std::vector<detail::Region>* got = tree.current == tree.root ? nullptr : &tree.currentNode().regions;
    const std::size_t count = got != nullptr ? got->size() : 0;
    bool same = count == regions.size();
    for (std::size_t i = 0; same && i < count; ++i) {
        const detail::Region& r = (*got)[i];
        same = r.before.begin.value() == regions[i].beforeBegin && r.before.end.value() == regions[i].beforeEnd &&
               r.after.begin.value() == regions[i].afterBegin && r.after.end.value() == regions[i].afterEnd;
    }
    if (!same) {
        std::string list;
        for (const OracleRegion& r : regions) {
            list += std::format(" [{}, {})->[{}, {})", r.beforeBegin, r.beforeEnd, r.afterBegin, r.afterEnd);
        }
        std::string have;
        for (std::size_t i = 0; i < count; ++i) {
            const detail::Region& r = (*got)[i];
            have += std::format(" [{}, {})->[{}, {})", r.before.begin, r.before.end, r.after.begin, r.after.end);
        }
        return std::unexpected(mismatch(std::format("regions{}, expected{}", have, list)));
    }
    // The last text-changing op, step by step.
    if (m_last.valid) {
        std::string text = m_last.oldText;
        std::size_t k = 0;
        for (std::size_t i = m_last.edits.size(); i-- > 0;) {
            const RawEdit& e = m_last.edits[i];
            std::string next = text;
            next.replace(static_cast<std::size_t>(e.range.begin.value()), static_cast<std::size_t>(e.range.size()),
                         m_last.inserted[i]);
            if (k >= m_last.changes.size()) {
                return std::unexpected(mismatch("the last op has fewer changes than raw steps"));
            }
            const TextChange& c = m_last.changes[k++];
            const std::string_view replacement =
                std::string_view(next).substr(static_cast<std::size_t>(c.start.value()),
                                              static_cast<std::size_t>(c.newEnd.value() - c.start.value()));
            if (std::optional<std::string> failure = checkStep(text, next, c, replacement)) {
                return std::unexpected(
                    mismatch(std::format("last {} op, step {}: {}", name(m_last.origin), k - 1, *failure)));
            }
            text = std::move(next);
        }
        const std::string& final = m_oracle.text();
        const bool extraction = m_last.origin == ChangeOrigin::Undo || m_last.origin == ChangeOrigin::Redo ||
                                m_last.origin == ChangeOrigin::Rollback;
        if (extraction && text == final) {
            if (std::optional<std::string> failure =
                    checkLspReplayByExtraction(m_last.oldText, final, m_last.changes)) {
                return std::unexpected(mismatch(std::format("last {} op: {}", name(m_last.origin), *failure)));
            }
        }
    }
    return {};
}

UndoDriverOptions undoSeedOptions(std::uint64_t seed) {
    constexpr std::uint32_t caps[] = {4, 5, 7, 16, 4096};
    constexpr std::uint32_t histories[] = {1000, 8, 1};
    UndoDriverOptions options;
    options.buffer.maxPieceBytes = caps[seed % 5];
    options.maxTransactions = histories[seed % 3];
    if (const std::optional<std::uint64_t> limit = seedBufferLimit(seed)) {
        options.buffer.maxBufferBytes = *limit;
        options.maxDocBytes = std::min<std::uint64_t>(options.maxDocBytes, *limit + 32);
    }
    return options;
}

std::string undoSeedInitialText(std::uint64_t seed, const UndoDriverOptions& options) {
    if ((seed / 15) % 4 == 0) {
        return {};
    }
    SplitMixSource source(seed * 0x9E37'79B9'7F4A'7C15ULL + 1);
    const std::uint64_t size = std::min(source.next(4097), options.buffer.maxBufferBytes);
    std::string text = tokenText(source, size);
    return text;
}

Result<UndoDriverStats> runUndoSeed(const UndoSeedRun& run) {
    UndoDriverOptions options = undoSeedOptions(run.seed);
    options.checkEvery = run.checkEvery;
    options.maxOps = run.ops;
    UndoDriver driver(options, std::format("seed {}", run.seed), undoSeedInitialText(run.seed, options));
    SplitMixSource source(run.seed);
    while (!driver.done()) {
        if (Result<void> stepped = driver.step(source); !stepped) {
            return std::unexpected(stepped.error());
        }
    }
    if (Result<void> checked = driver.verifyFull(); !checked) {
        return std::unexpected(checked.error());
    }
    return driver.stats();
}

Result<UndoQuickSettings> undoQuickSettings(const EnvironmentLookup& lookup) {
    constexpr std::uint64_t k32 = 0xFFFF'FFFFu;
    const auto seed = readNumber(lookup, kSeedVariable, 0, ~std::uint64_t{0});
    const auto ops = readNumber(lookup, kOpsVariable, 0, k32);
    const auto checkEvery = readNumber(lookup, kCheckEveryVariable, 0, k32);
    const auto threads = readNumber(lookup, kThreadsVariable, 1, 1024);
    for (const auto* read : {&seed, &ops, &checkEvery, &threads}) {
        if (!*read) {
            return std::unexpected(read->error());
        }
    }
    UndoQuickSettings settings;
    settings.onlySeed = *seed;
    settings.run.seed = seed->value_or(0);
    settings.run.ops = static_cast<std::uint32_t>(ops->value_or(settings.run.ops));
    settings.run.checkEvery = static_cast<std::uint32_t>(checkEvery->value_or(settings.run.checkEvery));
    if (*threads) {
        settings.threads = static_cast<unsigned>(**threads);
    }
    return settings;
}

std::string reproduceCommand(const UndoSeedRun& run) {
    return std::format("{}={} {}={} {}={} text_tests --gtest_filter=UndoDifferential.Quick", kSeedVariable, run.seed,
                       kOpsVariable, run.ops, kCheckEveryVariable, run.checkEvery);
}

} // namespace frost::text::test
