// SPDX-License-Identifier: Apache-2.0
//
// queries.cpp: frost::syntax's embedded queries (ADR 0023, D8): the highlights.scm and outline.scm
// texts compiled into the library, their compilation with capture-name and predicate validation,
// and predicate evaluation against snapshot text.

#include "language_state.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>

namespace frost::syntax::detail {

namespace {

// The query texts, generated from src/syntax/queries by frost_embed_files (cmake/FrostEmbed.cmake).
constexpr unsigned char kCHighlights[] = {
#include "queries_c_highlights_scm.inc"
};
constexpr unsigned char kCOutline[] = {
#include "queries_c_outline_scm.inc"
};
constexpr unsigned char kCppHighlights[] = {
#include "queries_cpp_highlights_scm.inc"
};
constexpr unsigned char kCppOutline[] = {
#include "queries_cpp_outline_scm.inc"
};
constexpr unsigned char kCSharpHighlights[] = {
#include "queries_csharp_highlights_scm.inc"
};
constexpr unsigned char kCSharpOutline[] = {
#include "queries_csharp_outline_scm.inc"
};
constexpr unsigned char kCMakeHighlights[] = {
#include "queries_cmake_highlights_scm.inc"
};
constexpr unsigned char kCMakeOutline[] = {
#include "queries_cmake_outline_scm.inc"
};
constexpr unsigned char kXmlHighlights[] = {
#include "queries_xml_highlights_scm.inc"
};
constexpr unsigned char kXmlOutline[] = {
#include "queries_xml_outline_scm.inc"
};
constexpr unsigned char kJsonHighlights[] = {
#include "queries_json_highlights_scm.inc"
};
constexpr unsigned char kJsonOutline[] = {
#include "queries_json_outline_scm.inc"
};
constexpr unsigned char kMarkdownHighlights[] = {
#include "queries_markdown_highlights_scm.inc"
};
constexpr unsigned char kMarkdownOutline[] = {
#include "queries_markdown_outline_scm.inc"
};

constexpr unsigned char kLuaHighlights[] = {
#include "queries_lua_highlights_scm.inc"
};
constexpr unsigned char kLuaFolds[] = {
#include "queries_lua_folds_scm.inc"
};
constexpr unsigned char kLuaOutline[] = {
#include "queries_lua_outline_scm.inc"
};

template<std::size_t N>
[[nodiscard]] std::string_view view(const unsigned char (&bytes)[N]) noexcept {
    return std::string_view(reinterpret_cast<const char*>(bytes), N);
}

[[nodiscard]] std::string_view queryErrorName(TSQueryError error) noexcept {
    switch (error) {
    case TSQueryErrorSyntax: return "TSQueryErrorSyntax";
    case TSQueryErrorNodeType: return "TSQueryErrorNodeType";
    case TSQueryErrorField: return "TSQueryErrorField";
    case TSQueryErrorCapture: return "TSQueryErrorCapture";
    case TSQueryErrorStructure: return "TSQueryErrorStructure";
    case TSQueryErrorLanguage: return "TSQueryErrorLanguage";
    case TSQueryErrorNone: break;
    }
    return "TSQueryErrorNone";
}

[[nodiscard]] std::string_view stringValue(const TSQuery* query, std::uint32_t id) noexcept {
    std::uint32_t length = 0;
    const char* value = ts_query_string_value_for_id(query, id, &length);
    return std::string_view(value, length);
}

[[nodiscard]] std::string_view captureNameOf(const TSQuery* query, std::uint32_t id) noexcept {
    std::uint32_t length = 0;
    const char* value = ts_query_capture_name_for_id(query, id, &length);
    return std::string_view(value, length);
}

// Parses one predicate (steps up to, excluding, the Done step). Errors name the problem.
[[nodiscard]] Result<Predicate> parsePredicate(const TSQuery* query, std::span<const TSQueryPredicateStep> steps,
                                               std::string_view label, std::uint32_t pattern) noexcept {
    auto bad = [&](std::string_view why) {
        return fail(ErrorCode::Corrupt, std::format("{}: pattern {}: {}", label, pattern, why));
    };
    if (steps.empty() || steps[0].type != TSQueryPredicateStepTypeString) {
        return bad("a predicate must start with its name");
    }
    const std::string_view name = stringValue(query, steps[0].value_id);
    Predicate predicate;
    if (name == "eq?") {
        predicate.op = PredicateOp::Eq;
    } else if (name == "not-eq?") {
        predicate.op = PredicateOp::NotEq;
    } else if (name == "any-of?") {
        predicate.op = PredicateOp::AnyOf;
    } else if (name == "not-any-of?") {
        predicate.op = PredicateOp::NotAnyOf;
    } else if (name == "frost-upper?") {
        predicate.op = PredicateOp::Upper;
    } else if (name == "frost-doc?") {
        predicate.op = PredicateOp::DocComment;
    } else {
        return bad(std::format("unknown predicate #{}", name));
    }
    const std::span<const TSQueryPredicateStep> args = steps.subspan(1);
    if (args.empty() || args[0].type != TSQueryPredicateStepTypeCapture) {
        return bad(std::format("#{} needs a capture as its first argument", name));
    }
    predicate.capture = args[0].value_id;
    const std::span<const TSQueryPredicateStep> rest = args.subspan(1);
    switch (predicate.op) {
    case PredicateOp::Eq:
    case PredicateOp::NotEq:
        if (rest.size() != 1) {
            return bad(std::format("#{} takes exactly two arguments", name));
        }
        if (rest[0].type == TSQueryPredicateStepTypeCapture) {
            predicate.otherCapture = rest[0].value_id;
        } else {
            predicate.strings.emplace_back(stringValue(query, rest[0].value_id));
        }
        break;
    case PredicateOp::AnyOf:
    case PredicateOp::NotAnyOf:
        if (rest.empty()) {
            return bad(std::format("#{} needs at least one string", name));
        }
        for (const TSQueryPredicateStep& step : rest) {
            if (step.type != TSQueryPredicateStepTypeString) {
                return bad(std::format("#{} takes strings after its capture", name));
            }
            predicate.strings.emplace_back(stringValue(query, step.value_id));
        }
        break;
    case PredicateOp::Upper:
    case PredicateOp::DocComment:
        if (!rest.empty()) {
            return bad(std::format("#{} takes one capture", name));
        }
        break;
    }
    return predicate;
}

// The first node of `capture` in the match, if any.
[[nodiscard]] const TSNode* findCapture(const TSQueryMatch& match, std::uint32_t capture) noexcept {
    for (std::uint16_t i = 0; i < match.capture_count; ++i) {
        if (match.captures[i].index == capture) {
            return &match.captures[i].node;
        }
    }
    return nullptr;
}

[[nodiscard]] bool isUpperName(std::string_view text) noexcept {
    if (text.size() < 2 || text[0] < 'A' || text[0] > 'Z') {
        return false;
    }
    return std::ranges::all_of(text,
                               [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
}

[[nodiscard]] bool isDocComment(std::string_view text) noexcept {
    if (text.starts_with("///") || text.starts_with("//!") || text.starts_with("/*!")) {
        return true;
    }
    return text.starts_with("/**") && !text.starts_with("/**/");
}

// Node text equality with `expected`, reading at most |expected| bytes.
[[nodiscard]] bool nodeEquals(const TSNode& node, std::string_view expected, const text::Snapshot& text) noexcept {
    const std::uint64_t begin = ts_node_start_byte(node);
    const std::uint64_t end = ts_node_end_byte(node);
    if (end - begin != expected.size()) {
        return false;
    }
    std::array<char, 256> buffer{};
    if (expected.size() <= buffer.size()) {
        const std::optional<std::string_view> got = readSmall(text, begin, end, buffer);
        return got && *got == expected;
    }
    return text.text(text::ByteRange{ByteOffset{begin}, ByteOffset{end}}) == expected;
}

// Text equality of two nodes; nodes longer than 4096 bytes compare unequal (bounded cost).
[[nodiscard]] bool nodesEqual(const TSNode& a, const TSNode& b, const text::Snapshot& text) noexcept {
    const std::uint64_t aBegin = ts_node_start_byte(a);
    const std::uint64_t aEnd = ts_node_end_byte(a);
    const std::uint64_t bBegin = ts_node_start_byte(b);
    const std::uint64_t bEnd = ts_node_end_byte(b);
    if (aEnd - aBegin != bEnd - bBegin || aEnd - aBegin > 4096) {
        return false;
    }
    const std::uint64_t size = text.size().value();
    const auto clamp = [size](std::uint64_t x) { return ByteOffset{std::min(x, size)}; };
    return text.text(text::ByteRange{clamp(aBegin), clamp(aEnd)}) ==
           text.text(text::ByteRange{clamp(bBegin), clamp(bEnd)});
}

[[nodiscard]] bool predicateHolds(const Predicate& predicate, const TSQueryMatch& match,
                                  const text::Snapshot& text) noexcept {
    const TSNode* node = findCapture(match, predicate.capture);
    if (node == nullptr) {
        // An optional capture that did not match: tree-sitter's text predicates pass then.
        return true;
    }
    switch (predicate.op) {
    case PredicateOp::Eq:
    case PredicateOp::NotEq: {
        bool equal = false;
        if (predicate.otherCapture) {
            const TSNode* other = findCapture(match, *predicate.otherCapture);
            if (other == nullptr) {
                return true;
            }
            equal = nodesEqual(*node, *other, text);
        } else {
            equal = nodeEquals(*node, predicate.strings[0], text);
        }
        return (predicate.op == PredicateOp::Eq) == equal;
    }
    case PredicateOp::AnyOf:
    case PredicateOp::NotAnyOf: {
        const bool any =
            std::ranges::any_of(predicate.strings, [&](const std::string& s) { return nodeEquals(*node, s, text); });
        return (predicate.op == PredicateOp::AnyOf) == any;
    }
    case PredicateOp::Upper: {
        std::array<char, 256> buffer{};
        const std::optional<std::string_view> got =
            readSmall(text, ts_node_start_byte(*node), ts_node_end_byte(*node), buffer);
        return got && isUpperName(*got);
    }
    case PredicateOp::DocComment: {
        std::array<char, 4> buffer{};
        const std::uint64_t begin = ts_node_start_byte(*node);
        const std::uint64_t end = std::min<std::uint64_t>(ts_node_end_byte(*node), begin + buffer.size());
        const std::optional<std::string_view> got = readSmall(text, begin, end, buffer);
        return got && isDocComment(*got);
    }
    }
    return false;
}

} // namespace

std::string_view querySource(LanguageId language, QueryKind kind) noexcept {
    if (kind == QueryKind::Folds) {
        return language == LanguageId::Lua ? view(kLuaFolds) : std::string_view{};
    }
    const bool highlights = kind == QueryKind::Highlights;
    switch (language) {
    case LanguageId::C: return highlights ? view(kCHighlights) : view(kCOutline);
    case LanguageId::Cpp: return highlights ? view(kCppHighlights) : view(kCppOutline);
    case LanguageId::CSharp: return highlights ? view(kCSharpHighlights) : view(kCSharpOutline);
    case LanguageId::CMake: return highlights ? view(kCMakeHighlights) : view(kCMakeOutline);
    case LanguageId::Xml: return highlights ? view(kXmlHighlights) : view(kXmlOutline);
    case LanguageId::Json: return highlights ? view(kJsonHighlights) : view(kJsonOutline);
    case LanguageId::Markdown: return highlights ? view(kMarkdownHighlights) : view(kMarkdownOutline);
    case LanguageId::Lua: return highlights ? view(kLuaHighlights) : view(kLuaOutline);
    case LanguageId::None: break;
    }
    return {};
}

Result<CompiledQuery> compileQuery(const TSLanguage* grammar, std::string_view source, QueryKind kind,
                                   std::string_view label) noexcept {
    std::uint32_t errorOffset = 0;
    TSQueryError error = TSQueryErrorNone;
    CompiledQuery compiled;
    compiled.query.reset(
        ts_query_new(grammar, source.data(), static_cast<std::uint32_t>(source.size()), &errorOffset, &error));
    if (!compiled.query) {
        // The line helps whoever fixes the .scm file.
        const std::string_view before = source.substr(0, std::min<std::size_t>(errorOffset, source.size()));
        const auto line = 1 + std::ranges::count(before, '\n');
        return fail(ErrorCode::Corrupt,
                    std::format("{}: {} at byte {} (line {})", label, queryErrorName(error), errorOffset, line));
    }
    const TSQuery* query = compiled.query.get();
    const std::uint32_t captures = ts_query_capture_count(query);
    for (std::uint32_t id = 0; id < captures; ++id) {
        const std::string_view name = captureNameOf(query, id);
        const bool internal = name.starts_with('_');
        if (kind == QueryKind::Highlights) {
            const std::optional<HighlightKind> highlight = internal ? std::nullopt : highlightKindFromCapture(name);
            if (!internal && !highlight) {
                return fail(ErrorCode::Corrupt, std::format("{}: unknown capture @{}", label, name));
            }
            compiled.highlightKinds.push_back(highlight);
            continue;
        }
        if (kind == QueryKind::Folds) {
            if (name != "fold") {
                return fail(ErrorCode::Corrupt, std::format("{}: unknown capture @{}", label, name));
            }
            continue;
        }
        OutlineRole role;
        if (name == "outline.name") {
            role.isName = true;
        } else if (!internal) {
            const std::string_view suffix = name.starts_with("outline.") ? name.substr(8) : std::string_view{};
            for (std::size_t k = 0; k < kOutlineKindCount; ++k) {
                if (!suffix.empty() && outlineCaptureSuffix(static_cast<OutlineKind>(k)) == suffix) {
                    role.kind = static_cast<OutlineKind>(k);
                }
            }
            if (!role.kind) {
                return fail(ErrorCode::Corrupt, std::format("{}: unknown capture @{}", label, name));
            }
        }
        compiled.outlineRoles.push_back(role);
    }
    const std::uint32_t patterns = ts_query_pattern_count(query);
    compiled.predicates.resize(patterns);
    for (std::uint32_t pattern = 0; pattern < patterns; ++pattern) {
        std::uint32_t count = 0;
        const TSQueryPredicateStep* steps = ts_query_predicates_for_pattern(query, pattern, &count);
        const std::span<const TSQueryPredicateStep> all(steps, count);
        std::size_t start = 0;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i].type != TSQueryPredicateStepTypeDone) {
                continue;
            }
            Result<Predicate> predicate = parsePredicate(query, all.subspan(start, i - start), label, pattern);
            if (!predicate) {
                return std::unexpected(std::move(predicate.error()));
            }
            compiled.predicates[pattern].push_back(std::move(*predicate));
            start = i + 1;
        }
    }
    return compiled;
}

bool predicatesHold(const CompiledQuery& query, const TSQueryMatch& match, const text::Snapshot& text) noexcept {
    if (match.pattern_index >= query.predicates.size()) {
        return true;
    }
    return std::ranges::all_of(query.predicates[match.pattern_index],
                               [&](const Predicate& predicate) { return predicateHolds(predicate, match, text); });
}

std::optional<std::string_view> readSmall(const text::Snapshot& text, std::uint64_t begin, std::uint64_t end,
                                          std::span<char> buffer) noexcept {
    const std::uint64_t size = text.size().value();
    end = std::min(end, size);
    begin = std::min(begin, end);
    if (end - begin > buffer.size()) {
        return std::nullopt;
    }
    std::uint64_t at = begin;
    std::size_t used = 0;
    while (at < end) {
        const std::string_view chunk = text.chunkAt(ByteOffset{at});
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), end - at));
        if (take == 0) {
            break;
        }
        std::memcpy(buffer.data() + used, chunk.data(), take);
        used += take;
        at += take;
    }
    return std::string_view(buffer.data(), used);
}

} // namespace frost::syntax::detail
