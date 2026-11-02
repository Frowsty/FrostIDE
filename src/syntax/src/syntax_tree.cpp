// SPDX-License-Identifier: Apache-2.0
//
// syntax_tree.cpp: frost::syntax::SyntaxTree (ADR 0023, D5 to D7): highlight spans, the outline,
// enclosing ranges and the test dump, all over an immutable shared tree-sitter tree.

#include "frost/syntax/syntax_tree.hpp"

#include "frost/core/assert.hpp"

#include "language_state.hpp"
#include "tree_state.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace frost::syntax {

namespace detail {

SyntaxTree makeTree(TreePtr tree, const Language* language, text::Snapshot text, std::uint64_t parsedVersion) noexcept {
    auto state = std::make_shared<TreeState>();
    state->tree = std::move(tree);
    state->language = language;
    state->text = std::move(text);
    state->parsedVersion = parsedVersion;
    return SyntaxTree(std::move(state));
}

namespace {

void appendEscaped(std::string& out, std::string_view type) {
    for (const char c : type) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '"':
        case '\\':
            out += '\\';
            out += c;
            break;
        default: out += c;
        }
    }
}

} // namespace

std::string dumpNode(TSNode root, bool withRanges) {
    std::string out;
    TreeCursor cursor(root);
    std::uint32_t depth = 0;
    for (;;) {
        const TSNode node = ts_tree_cursor_current_node(cursor.get());
        out.append(2 * static_cast<std::size_t>(depth), ' ');
        if (const char* field = ts_tree_cursor_current_field_name(cursor.get())) {
            out += field;
            out += ": ";
        }
        if (ts_node_is_missing(node)) {
            out += "MISSING ";
        }
        if (ts_node_is_named(node)) {
            out += ts_node_type(node);
        } else {
            out += '"';
            appendEscaped(out, ts_node_type(node));
            out += '"';
        }
        if (withRanges) {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            out += std::format(" [{}-{}] ({},{})-({},{})", ts_node_start_byte(node), ts_node_end_byte(node), start.row,
                               start.column, end.row, end.column);
        }
        out += '\n';
        if (ts_tree_cursor_goto_first_child(cursor.get())) {
            ++depth;
            continue;
        }
        while (!ts_tree_cursor_goto_next_sibling(cursor.get())) {
            if (depth == 0 || !ts_tree_cursor_goto_parent(cursor.get())) {
                return out;
            }
            --depth;
        }
    }
}

} // namespace detail

namespace {

// The deadline of a query call; the progress callback halts the cursor once it has passed.
struct QueryProgress {
    const IClock* clock = nullptr;
    IClock::SteadyTime deadline{};
    bool halted = false;
};

bool onQueryProgress(TSQueryCursorState* state) noexcept {
    auto& progress = *static_cast<QueryProgress*>(state->payload);
    if (progress.clock->steadyNow() >= progress.deadline) {
        progress.halted = true;
        return true;
    }
    return false;
}

// A query run over one tree: the cursor with the limits applied. The cursor keeps a pointer to
// its TSQueryCursorOptions (lib/src/query.c, ts_query_cursor_exec_with_options), so the options
// live in the run, which never moves.
class QueryRun {
public:
    QueryRun(const detail::TreeState& state, const detail::CompiledQuery& query, const QueryLimits& limits,
             std::uint64_t begin, std::uint64_t end) noexcept
        : m_cursor(ts_query_cursor_new()) {
        ts_query_cursor_set_match_limit(m_cursor.get(), limits.matchLimit);
        if (begin != 0 || end != state.text.size().value()) {
            ts_query_cursor_set_byte_range(m_cursor.get(), static_cast<std::uint32_t>(begin),
                                           static_cast<std::uint32_t>(end));
        }
        if (limits.budget > IClock::Duration::zero()) {
            m_progress.clock = limits.clock != nullptr ? limits.clock : &systemClock();
            m_progress.deadline = m_progress.clock->steadyNow() + limits.budget;
            m_options.payload = &m_progress;
            m_options.progress_callback = &onQueryProgress;
        }
        ts_query_cursor_exec_with_options(m_cursor.get(), query.query.get(), ts_tree_root_node(state.tree.get()),
                                          &m_options);
    }
    QueryRun(const QueryRun&) = delete ("the cursor points at m_options and m_progress");
    QueryRun& operator=(const QueryRun&) = delete ("the cursor points at m_options and m_progress");

    [[nodiscard]] bool next(TSQueryMatch& match) noexcept { return ts_query_cursor_next_match(m_cursor.get(), &match); }
    // True when the deadline or the match limit cut the run short.
    [[nodiscard]] bool limited() const noexcept {
        return m_progress.halted || ts_query_cursor_did_exceed_match_limit(m_cursor.get());
    }

private:
    detail::QueryCursorPtr m_cursor;
    QueryProgress m_progress;
    TSQueryCursorOptions m_options{};
};

struct Capture {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint32_t pattern = 0;
    HighlightKind kind = HighlightKind::Comment;
};

/*
 * Paints sorted, properly nested captures into flat spans: every byte gets the kind of the
 * innermost capture covering it, an identical range keeps the first (lowest) pattern, and the
 * result is clipped to [lo, hi) with adjacent same-kind spans merged. Captures are tree nodes, so
 * they nest or are disjoint; a partial overlap (impossible for one tree) is clipped to its parent.
 * O(c log c).
 */
void paint(std::vector<Capture>& captures, std::uint64_t lo, std::uint64_t hi, std::vector<HighlightSpan>& out) {
    std::ranges::sort(captures, [](const Capture& a, const Capture& b) {
        if (a.begin != b.begin) {
            return a.begin < b.begin;
        }
        if (a.end != b.end) {
            return a.end > b.end;
        }
        return a.pattern < b.pattern;
    });
    const auto emit = [&](std::uint64_t begin, std::uint64_t end, HighlightKind kind) {
        begin = std::max(begin, lo);
        end = std::min(end, hi);
        if (begin >= end) {
            return;
        }
        if (!out.empty() && out.back().kind == kind && out.back().range.end.value() == begin) {
            out.back().range.end = ByteOffset{end};
            return;
        }
        out.push_back(HighlightSpan{text::ByteRange{ByteOffset{begin}, ByteOffset{end}}, kind});
    };
    std::vector<Capture> stack;
    std::uint64_t position = 0;
    for (const Capture& capture : captures) {
        while (!stack.empty() && stack.back().end <= capture.begin) {
            emit(position, stack.back().end, stack.back().kind);
            position = std::max(position, stack.back().end);
            stack.pop_back();
        }
        if (!stack.empty()) {
            if (position < capture.begin) {
                emit(position, capture.begin, stack.back().kind);
            }
            if (stack.back().begin == capture.begin && stack.back().end == capture.end) {
                position = std::max(position, capture.begin);
                continue;
            }
        }
        position = std::max(position, capture.begin);
        Capture inner = capture;
        if (!stack.empty()) {
            inner.end = std::min(inner.end, stack.back().end);
        }
        stack.push_back(inner);
    }
    while (!stack.empty()) {
        emit(position, stack.back().end, stack.back().kind);
        position = std::max(position, stack.back().end);
        stack.pop_back();
    }
}

[[nodiscard]] bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// The outline name of [begin, end): whitespace runs collapsed, trimmed, at most 256 bytes cut at a
// UTF-8 boundary.
[[nodiscard]] std::string outlineName(const text::Snapshot& text, std::uint64_t begin, std::uint64_t end) {
    std::array<char, 1024> buffer{};
    end = std::min(end, begin + buffer.size());
    const std::optional<std::string_view> raw = detail::readSmall(text, begin, end, buffer);
    std::string name;
    if (!raw) {
        return name;
    }
    bool pendingSpace = false;
    for (const char c : *raw) {
        if (isSpace(c)) {
            pendingSpace = !name.empty();
            continue;
        }
        if (pendingSpace) {
            name += ' ';
            pendingSpace = false;
        }
        name += c;
    }
    if (name.size() > 256) {
        std::size_t cut = 256;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        name.resize(cut);
    }
    return name;
}

} // namespace

LanguageId SyntaxTree::language() const noexcept { return m_state ? m_state->language->id : LanguageId::None; }

text::Snapshot SyntaxTree::text() const noexcept { return m_state ? m_state->text : text::Snapshot{}; }

std::uint64_t SyntaxTree::version() const noexcept { return m_state ? m_state->text.version() : 0; }

std::uint64_t SyntaxTree::parsedVersion() const noexcept { return m_state ? m_state->parsedVersion : 0; }

bool SyntaxTree::hasError() const noexcept {
    return m_state && ts_node_has_error(ts_tree_root_node(m_state->tree.get()));
}

HighlightResult SyntaxTree::highlights(text::ByteRange range, const QueryLimits& limits) const noexcept {
    FROST_PRE(range.begin <= range.end);
    HighlightResult result;
    if (!m_state || !m_state->language->queriesReady()) {
        result.complete = false;
        return result;
    }
    const detail::TreeState& state = *m_state;
    const std::uint64_t size = state.text.size().value();
    const std::uint64_t begin = std::min(range.begin.value(), size);
    const std::uint64_t end = std::min(range.end.value(), size);
    if (begin == end) {
        return result;
    }
    const detail::CompiledQuery& query = state.language->highlights();
    QueryRun run(state, query, limits, begin, end);
    std::vector<Capture> captures;
    bool truncated = false;
    TSQueryMatch match{};
    while (!truncated && run.next(match)) {
        if (!detail::predicatesHold(query, match, state.text)) {
            continue;
        }
        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture = match.captures[i];
            const std::optional<HighlightKind> kind = query.highlightKinds[capture.index];
            const std::uint64_t captureBegin = ts_node_start_byte(capture.node);
            const std::uint64_t captureEnd = ts_node_end_byte(capture.node);
            if (!kind || captureBegin >= captureEnd) {
                continue;
            }
            if (captures.size() >= limits.maxResults) {
                truncated = true;
                break;
            }
            captures.push_back(Capture{captureBegin, captureEnd, match.pattern_index, *kind});
        }
    }
    result.complete = !truncated && !run.limited();
    paint(captures, begin, end, result.spans);
    return result;
}

std::optional<text::ByteRange> SyntaxTree::luaOpaqueAt(ByteOffset at) const noexcept {
    if (!m_state || m_state->language->id != LanguageId::Lua || at.value() >= m_state->text.size().value()) {
        return std::nullopt;
    }
    TSNode node = ts_node_named_descendant_for_byte_range(ts_tree_root_node(m_state->tree.get()),
                                                          static_cast<std::uint32_t>(at.value()),
                                                          static_cast<std::uint32_t>(at.value() + 1));
    while (!ts_node_is_null(node)) {
        const std::string_view kind(ts_node_type(node));
        if (kind == "string" || kind == "comment") {
            return text::ByteRange{ByteOffset{ts_node_start_byte(node)}, ByteOffset{ts_node_end_byte(node)}};
        }
        node = ts_node_parent(node);
    }
    return std::nullopt;
}

ByteOffset SyntaxTree::luaErrorStartAt(ByteOffset at) const noexcept {
    if (!m_state || m_state->language->id != LanguageId::Lua || at.value() >= m_state->text.size().value()) {
        return at;
    }
    TSNode node = ts_node_named_descendant_for_byte_range(ts_tree_root_node(m_state->tree.get()),
                                                          static_cast<std::uint32_t>(at.value()),
                                                          static_cast<std::uint32_t>(at.value() + 1));
    ByteOffset result = at;
    while (!ts_node_is_null(node)) {
        if (ts_node_is_error(node)) {
            result = ByteOffset{ts_node_start_byte(node)};
        }
        node = ts_node_parent(node);
    }
    return result;
}

std::optional<ByteOffset> SyntaxTree::luaCloserOpener(ByteOffset at) const noexcept {
    if (!m_state || m_state->language->id != LanguageId::Lua || at.value() >= m_state->text.size().value()) {
        return std::nullopt;
    }
    TSNode node = ts_node_descendant_for_byte_range(ts_tree_root_node(m_state->tree.get()),
                                                    static_cast<std::uint32_t>(at.value()),
                                                    static_cast<std::uint32_t>(at.value() + 1));
    const std::string_view token(ts_node_type(node));
    if (token != "end" && token != "until") {
        return std::nullopt;
    }
    for (node = ts_node_parent(node); !ts_node_is_null(node); node = ts_node_parent(node)) {
        const std::string_view kind(ts_node_type(node));
        if (token == "until" && kind == "repeat_statement") {
            return ByteOffset{ts_node_start_byte(node)};
        }
        if (token == "end" && ts_node_end_byte(node) == at.value() + 3 &&
            (kind == "function_declaration" || kind == "function_definition" || kind == "if_statement" ||
             kind == "for_statement" || kind == "while_statement" || kind == "do_statement")) {
            return ByteOffset{ts_node_start_byte(node)};
        }
    }
    return std::nullopt;
}

FoldResult SyntaxTree::foldRanges(const QueryLimits& limits) const noexcept {
    FoldResult result;
    if (!m_state || !m_state->language->queriesReady()) {
        result.complete = false;
        return result;
    }
    if (m_state->language->id != LanguageId::Lua) {
        return result;
    }
    const auto& state = *m_state;
    const auto& query = state.language->folds();
    QueryRun run(state, query, limits, 0, state.text.size().value());
    TSQueryMatch match{};
    bool truncated = false;
    while (!truncated && run.next(match)) {
        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSNode node = match.captures[i].node;
            if (ts_node_start_point(node).row == ts_node_end_point(node).row) {
                continue;
            }
            if (result.ranges.size() >= limits.maxResults) {
                truncated = true;
                break;
            }
            result.ranges.push_back({ByteOffset{ts_node_start_byte(node)}, ByteOffset{ts_node_end_byte(node)}});
        }
    }
    std::ranges::sort(result.ranges, [](const auto& a, const auto& b) {
        return a.begin < b.begin || (a.begin == b.begin && a.end > b.end);
    });
    result.complete = !truncated && !run.limited();
    return result;
}

Outline SyntaxTree::outline(const QueryLimits& limits) const noexcept {
    Outline result;
    if (!m_state || !m_state->language->queriesReady()) {
        result.complete = false;
        return result;
    }
    const detail::TreeState& state = *m_state;
    const detail::CompiledQuery& query = state.language->outline();
    QueryRun run(state, query, limits, 0, state.text.size().value());

    struct Raw {
        std::uint64_t begin, end, nameBegin, nameEnd;
        std::uint32_t pattern;
        OutlineKind kind;
    };
    std::vector<Raw> raws;
    bool truncated = false;
    TSQueryMatch match{};
    while (!truncated && run.next(match)) {
        if (!detail::predicatesHold(query, match, state.text)) {
            continue;
        }
        const TSNode* item = nullptr;
        const TSNode* name = nullptr;
        OutlineKind kind = OutlineKind::Function;
        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const detail::OutlineRole& role = query.outlineRoles[match.captures[i].index];
            if (role.kind && item == nullptr) {
                item = &match.captures[i].node;
                kind = *role.kind;
            } else if (role.isName && name == nullptr) {
                name = &match.captures[i].node;
            }
        }
        if (item == nullptr) {
            continue;
        }
        if (raws.size() >= limits.maxResults) {
            truncated = true;
            break;
        }
        const std::uint64_t begin = ts_node_start_byte(*item);
        const std::uint64_t nameBegin = name != nullptr ? ts_node_start_byte(*name) : begin;
        const std::uint64_t nameEnd = name != nullptr ? ts_node_end_byte(*name) : begin;
        raws.push_back(Raw{begin, ts_node_end_byte(*item), nameBegin, nameEnd, match.pattern_index, kind});
    }
    result.complete = !truncated && !run.limited();
    std::ranges::sort(raws, [](const Raw& a, const Raw& b) {
        if (a.begin != b.begin) {
            return a.begin < b.begin;
        }
        if (a.end != b.end) {
            return a.end > b.end;
        }
        return a.pattern < b.pattern;
    });
    std::vector<std::uint32_t> stack;
    const Raw* previous = nullptr;
    for (const Raw& raw : raws) {
        // One declaration matched by several patterns keeps the first pattern's kind.
        if (previous != nullptr && previous->begin == raw.begin && previous->end == raw.end) {
            continue;
        }
        previous = &raw;
        while (!stack.empty()) {
            const OutlineItem& top = result.items[stack.back()];
            if (top.range.begin.value() <= raw.begin && raw.end <= top.range.end.value()) {
                break;
            }
            stack.pop_back();
        }
        if (stack.size() >= limits.maxDepth) {
            result.complete = false;
            continue;
        }
        OutlineItem item;
        item.kind = raw.kind;
        item.parent = stack.empty() ? kNoParent : stack.back();
        item.depth = static_cast<std::uint32_t>(stack.size());
        item.range = text::ByteRange{ByteOffset{raw.begin}, ByteOffset{raw.end}};
        item.nameRange = text::ByteRange{ByteOffset{raw.nameBegin}, ByteOffset{raw.nameEnd}};
        item.name = outlineName(state.text, raw.nameBegin, raw.nameEnd);
        stack.push_back(static_cast<std::uint32_t>(result.items.size()));
        result.items.push_back(std::move(item));
    }
    return result;
}

std::vector<text::ByteRange> SyntaxTree::enclosingRanges(text::ByteRange selection) const noexcept {
    FROST_PRE(selection.begin <= selection.end);
    std::vector<text::ByteRange> ranges;
    if (!m_state) {
        return ranges;
    }
    const std::uint64_t size = m_state->text.size().value();
    const auto begin = static_cast<std::uint32_t>(std::min(selection.begin.value(), size));
    const auto end = static_cast<std::uint32_t>(std::min(selection.end.value(), size));
    const TSNode root = ts_tree_root_node(m_state->tree.get());
    const auto rangeOf = [](TSNode node) {
        return text::ByteRange{ByteOffset{ts_node_start_byte(node)}, ByteOffset{ts_node_end_byte(node)}};
    };
    ranges.push_back(rangeOf(root));
    detail::TreeCursor cursor(root);
    for (;;) {
        if (ts_tree_cursor_goto_first_child_for_byte(cursor.get(), begin) < 0) {
            break;
        }
        bool found = false;
        TSNode child{};
        do {
            child = ts_tree_cursor_current_node(cursor.get());
            if (ts_node_start_byte(child) > begin) {
                break;
            }
            if (end <= ts_node_end_byte(child) && ts_node_start_byte(child) < ts_node_end_byte(child)) {
                found = true;
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(cursor.get()));
        if (!found) {
            break;
        }
        if (ts_node_is_named(child)) {
            ranges.push_back(rangeOf(child));
        }
    }
    std::ranges::reverse(ranges);
    const auto [first, last] = std::ranges::unique(ranges);
    ranges.erase(first, last);
    return ranges;
}

std::string SyntaxTree::dumpForTesting(bool withRanges) const {
    if (!m_state) {
        return {};
    }
    return detail::dumpNode(ts_tree_root_node(m_state->tree.get()), withRanges);
}

} // namespace frost::syntax
