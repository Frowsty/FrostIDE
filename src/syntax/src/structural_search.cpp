// SPDX-License-Identifier: Apache-2.0
// Fresh parser/query execution and bounded capture-template proposals; no textual source scanning.
#include "frost/syntax/structural_search.hpp"

#include "frost/text/text_buffer.hpp"

#include "language_state.hpp"
#include "parse.hpp"
#include "tree_state.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
namespace frost::syntax {
namespace {
constexpr std::size_t kBytes = 4 * 1024 * 1024, kOutput = 8 * 1024 * 1024;
Result<SyntaxTree> parse(const Language* language, text::Snapshot source, std::stop_token stop) {
    detail::ParserPtr parser(ts_parser_new());
    if (!parser || !ts_parser_set_language(parser.get(), language->grammar))
        return fail(ErrorCode::Unsupported, "Structural search grammar is unavailable");
    detail::ParseJob job(std::move(parser), source, nullptr);
    auto parsed =
        detail::runParse(job, systemClock(), systemClock().steadyNow() + std::chrono::seconds(2), &stop, nullptr);
    if (parsed.status != detail::ParseStatus::Done)
        return fail(stop.stop_requested() ? ErrorCode::Cancelled : ErrorCode::Timeout,
                    "Structural parse cancelled or exceeded its budget");
    auto tree = detail::makeTree(std::move(parsed.tree), language, source, source.version());
    if (tree.hasError())
        return fail(ErrorCode::Unsupported, "Structural search requires error-free syntax in the selected grammar");
    return tree;
}
bool captureName(std::string_view name) {
    if (name.empty() || name.size() > 64 ||
        !((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_'))
        return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}
Result<std::string> expand(std::string_view replacement, const StructuralMatch& match) {
    std::string output;
    for (std::size_t i = 0; i < replacement.size();) {
        if (replacement[i] != '$') {
            output.push_back(replacement[i++]);
            continue;
        }
        if (i + 1 < replacement.size() && replacement[i + 1] == '$') {
            output.push_back('$');
            i += 2;
            continue;
        }
        if (i + 1 >= replacement.size() || replacement[i + 1] != '{')
            return fail(ErrorCode::InvalidArgument, "Replacement uses ${capture} or $$ only");
        const auto end = replacement.find('}', i + 2);
        if (end == std::string_view::npos)
            return fail(ErrorCode::InvalidArgument, "Unclosed replacement capture");
        const auto found = match.captures.find(replacement.substr(i + 2, end - i - 2));
        if (found == match.captures.end())
            return fail(ErrorCode::InvalidArgument, "Replacement references an unknown capture");
        if (output.size() + found->second.text.size() > kOutput)
            return fail(ErrorCode::OutOfRange, "Structural replacement expansion exceeds budget");
        output += found->second.text;
        i = end + 1;
    }
    return output;
}
struct Progress {
    std::stop_token stop;
    IClock::SteadyTime deadline;
    bool halted = false;
};
bool progress(TSQueryCursorState* state) {
    auto& p = *static_cast<Progress*>(state->payload);
    p.halted = p.stop.stop_requested() || systemClock().steadyNow() >= p.deadline;
    return p.halted;
}
} // namespace
Result<StructuralResult> structuralSearch(LanguageId id, text::Snapshot source, std::string_view pattern,
                                          std::optional<std::string_view> replacement, std::stop_token stop) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Structural search cancelled");
    if (source.size().value() > kBytes || pattern.empty() || pattern.size() > 16384 ||
        (replacement && replacement->size() > 16384))
        return fail(ErrorCode::OutOfRange, "Structural source/pattern/template exceeds budget");
    LanguageRegistry registry;
    auto language = registry.get(id);
    if (!language)
        return std::unexpected(language.error());
    auto query =
        detail::compileQuery((*language)->grammar, pattern, detail::QueryKind::Structural, "Structural pattern");
    if (!query)
        return std::unexpected(query.error());
    if (ts_query_pattern_count(query->query.get()) != 1)
        return fail(ErrorCode::Unsupported, "Structural search accepts one pattern, not a pattern union");
    const auto count = ts_query_capture_count(query->query.get());
    if (!count || count > 32)
        return fail(ErrorCode::OutOfRange, "Structural capture budget exceeded");
    std::vector<std::string> names;
    bool hasMatch = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t length = 0;
        const char* name = ts_query_capture_name_for_id(query->query.get(), i, &length);
        names.emplace_back(name, length);
        if (!captureName(names.back()) ||
            ts_query_capture_quantifier_for_id(query->query.get(), 0, i) != TSQuantifierOne)
            return fail(ErrorCode::Unsupported,
                        "Each structural capture must have one simple name and bind exactly one node");
        hasMatch |= names.back() == "match";
    }
    if (!hasMatch)
        return fail(ErrorCode::InvalidArgument, "Structural pattern requires @match");
    for (const auto& predicate : query->predicates[0])
        if (predicate.otherCapture ||
            (predicate.op != detail::PredicateOp::Eq && predicate.op != detail::PredicateOp::NotEq &&
             predicate.op != detail::PredicateOp::AnyOf && predicate.op != detail::PredicateOp::NotAnyOf))
            return fail(ErrorCode::Unsupported, "Structural predicates support literal equality/set membership only; "
                                                "semantic/regex predicates are unavailable");
    if (replacement) {
        StructuralMatch prototype;
        for (const auto& name : names)
            prototype.captures.emplace(name, StructuralCapture{});
        if (auto syntax = expand(*replacement, prototype); !syntax)
            return std::unexpected(syntax.error());
    }
    auto tree = parse(*language, source, stop);
    if (!tree)
        return std::unexpected(tree.error());
    detail::QueryCursorPtr cursor(ts_query_cursor_new());
    if (!cursor)
        return fail(ErrorCode::Unknown, "Cannot allocate structural query cursor");
    ts_query_cursor_set_match_limit(cursor.get(), 4096);
    Progress p{stop, systemClock().steadyNow() + std::chrono::seconds(2)};
    TSQueryCursorOptions options{};
    options.payload = &p;
    options.progress_callback = &progress;
    ts_query_cursor_exec_with_options(cursor.get(), query->query.get(), ts_tree_root_node(tree->state()->tree.get()),
                                      &options);
    StructuralResult result{source, {}, {}};
    std::size_t capturedBytes = 0;
    TSQueryMatch found{};
    while (ts_query_cursor_next_match(cursor.get(), &found)) {
        if (stop.stop_requested() || systemClock().steadyNow() >= p.deadline)
            return fail(stop.stop_requested() ? ErrorCode::Cancelled : ErrorCode::Timeout,
                        "Structural query cancelled or exceeded budget");
        if (!detail::predicatesHold(*query, found, source))
            continue;
        StructuralMatch match;
        for (std::uint16_t i = 0; i < found.capture_count; ++i) {
            const auto& capture = found.captures[i];
            if (capture.index >= names.size())
                return fail(ErrorCode::Corrupt, "Invalid structural capture");
            const text::ByteRange range{ByteOffset{ts_node_start_byte(capture.node)},
                                        ByteOffset{ts_node_end_byte(capture.node)}};
            if (range.end.value() > source.size().value() || range.begin >= range.end)
                return fail(ErrorCode::Unsupported, "Empty or out-of-range structural capture");
            capturedBytes += range.end.value() - range.begin.value();
            if (capturedBytes > kOutput)
                return fail(ErrorCode::OutOfRange, "Structural capture bytes exceed budget");
            if (!match.captures.emplace(names[capture.index], StructuralCapture{range, source.text(range)}).second)
                return fail(ErrorCode::Unsupported, "Repeated structural capture is ambiguous");
            if (names[capture.index] == "match")
                match.range = range;
        }
        if (match.captures.size() != names.size() || !match.captures.contains("match"))
            return fail(ErrorCode::Unsupported, "Structural match has an unbound capture");
        if (result.matches.size() >= 2000)
            return fail(ErrorCode::OutOfRange, "Structural result budget exceeded");
        result.matches.push_back(std::move(match));
    }
    if (p.halted || stop.stop_requested() || ts_query_cursor_did_exceed_match_limit(cursor.get()))
        return fail(stop.stop_requested() ? ErrorCode::Cancelled : ErrorCode::OutOfRange,
                    "Structural query was incomplete");
    std::ranges::sort(result.matches, {}, [](const auto& match) { return match.range.begin; });
    if (!replacement)
        return result;
    for (std::size_t i = 1; i < result.matches.size(); ++i)
        if (result.matches[i].range.begin < result.matches[i - 1].range.end)
            return fail(ErrorCode::Unsupported,
                        "Overlapping/nested structural replacements require a narrower pattern");
    result.after = source.text();
    for (auto it = result.matches.rbegin(); it != result.matches.rend(); ++it) {
        auto changed = expand(*replacement, *it);
        if (!changed)
            return std::unexpected(changed.error());
        if (result.after->size() + changed->size() > kOutput)
            return fail(ErrorCode::OutOfRange, "Structural output exceeds budget");
        result.after->replace(it->range.begin.value(), it->range.end.value() - it->range.begin.value(), *changed);
    }
    auto buffer = text::TextBuffer::fromUtf8(*result.after);
    if (!buffer)
        return std::unexpected(buffer.error());
    auto validated = parse(*language, buffer->current(), stop);
    if (!validated)
        return std::unexpected(validated.error());
    return result;
}
} // namespace frost::syntax
