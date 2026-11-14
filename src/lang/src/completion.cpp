// SPDX-License-Identifier: Apache-2.0
//
// Code completion (completion.hpp): decoding, triggers, requests, the filtered model and commits.

#include "frost/lang/completion.hpp"

#include "frost/lang/snippet.hpp"

#include <algorithm>
#include <array>

namespace frost::lang {
namespace {

// clangd prefixes labels with a space (or "•" when it would insert an #include); drop both.
[[nodiscard]] std::string displayLabel(std::string_view label) {
    for (;;) {
        if (label.starts_with(' ')) {
            label.remove_prefix(1);
        } else if (label.starts_with("\xE2\x80\xA2")) { // U+2022 BULLET
            label.remove_prefix(3);
        } else {
            break;
        }
    }
    return std::string(label);
}

[[nodiscard]] bool isIdentifierStartByte(char c) noexcept {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || static_cast<unsigned char>(c) >= 0x80;
}

[[nodiscard]] bool isIncludeLine(std::string_view line) noexcept {
    const std::size_t hash = line.find_first_not_of(" \t");
    if (hash == std::string_view::npos || line[hash] != '#') {
        return false;
    }
    std::string_view rest = line.substr(hash + 1);
    rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
    return rest.starts_with("include") || rest.starts_with("import");
}

} // namespace

std::string_view toString(CompletionItemKind kind) noexcept {
    static constexpr std::array<std::string_view, 25> kNames = {
        "text",      "method", "function", "constructor",   "field",  "variable",    "class",
        "interface", "module", "property", "unit",          "value",  "enum",        "keyword",
        "snippet",   "color",  "file",     "reference",     "folder", "enum member", "constant",
        "struct",    "event",  "operator", "type parameter"};
    const auto index = static_cast<std::size_t>(kind);
    return index >= 1 && index <= kNames.size() ? kNames[index - 1] : std::string_view("text");
}

bool isIdentifierByte(char c) noexcept { return isIdentifierStartByte(c) || (c >= '0' && c <= '9'); }

bool completionRequestsParameterHints(const CompletionItem& item) {
    const auto* command = item.raw.find("command");
    if (command && command->stringAt("command") == "editor.action.triggerParameterHints")
        return true;
    const bool call = item.kind == CompletionItemKind::Function || item.kind == CompletionItemKind::Method ||
                      item.kind == CompletionItemKind::Constructor;
    return call && item.snippet && item.insertText.find('(') != std::string::npos;
}

CompletionItem parseCompletionItem(const Json& json) {
    CompletionItem item;
    item.label = displayLabel(json.stringAt("label"));
    if (const Json* details = json.find("labelDetails"); details != nullptr && details->isObject()) {
        item.label += std::string(details->stringAt("detail"));
    }
    item.filterText = std::string(json.stringAt("filterText"));
    if (item.filterText.empty()) {
        item.filterText = displayLabel(json.stringAt("label"));
    }
    item.sortText = std::string(json.stringAt("sortText"));
    if (item.sortText.empty()) {
        item.sortText = item.label;
    }
    item.detail = std::string(json.stringAt("detail"));
    if (const Json* documentation = json.find("documentation"); documentation != nullptr) {
        item.documentation = parseMarkup(*documentation);
        if (documentation->isString()) {
            item.documentation.markdown = false; // A plain string is plaintext for completion items.
        }
    }
    const std::int64_t kind = json.integerAt("kind").value_or(1);
    item.kind = kind >= 1 && kind <= 25 ? static_cast<CompletionItemKind>(kind) : CompletionItemKind::Text;
    item.deprecated = json.boolAt("deprecated").value_or(false);
    if (const Json* tags = json.find("tags"); tags != nullptr) {
        for (const Json& tag : tags->items()) {
            item.deprecated = item.deprecated || tag.asInteger() == 1; // CompletionItemTag.Deprecated
        }
    }
    item.preselect = json.boolAt("preselect").value_or(false);
    item.snippet = json.integerAt("insertTextFormat") == 2;
    if (const Json* textEdit = json.find("textEdit"); textEdit != nullptr) {
        if (auto edit = parseTextEdit(*textEdit)) {
            item.editRange = edit->range;
            item.insertText = std::move(edit->newText);
        }
    }
    if (!item.editRange) {
        item.insertText = std::string(json.stringAt("insertText"));
        if (item.insertText.empty()) {
            item.insertText = item.filterText;
        }
    }
    if (const Json* additional = json.find("additionalTextEdits"); additional != nullptr) {
        item.additionalTextEdits = parseTextEdits(*additional);
    }
    if (const Json* commit = json.find("commitCharacters"); commit != nullptr) {
        for (const Json& character : commit->items()) {
            if (const std::string* text = character.asString()) {
                item.commitCharacters.push_back(*text);
            }
        }
    }
    item.score = json.find("score") != nullptr ? json.find("score")->asDouble().value_or(0.0) : 0.0;
    item.raw = json;
    return item;
}

CompletionList parseCompletionList(const Json& json) {
    CompletionList list;
    const Json* items = &json;
    if (json.isObject()) {
        list.isIncomplete = json.boolAt("isIncomplete").value_or(false);
        items = json.find("items");
    }
    if (items != nullptr) {
        for (const Json& item : items->items()) {
            if (item.isObject()) {
                list.items.push_back(parseCompletionItem(item));
            }
        }
    }
    return list;
}

std::size_t identifierStart(std::string_view lineBeforeCaret) noexcept {
    std::size_t start = lineBeforeCaret.size();
    while (start > 0 && isIdentifierByte(lineBeforeCaret[start - 1])) {
        --start;
    }
    return start;
}

std::optional<CompletionTrigger> autoCompletionTrigger(std::string_view line) noexcept {
    if (line.empty()) {
        return std::nullopt;
    }
    const char last = line.back();
    const char before = line.size() >= 2 ? line[line.size() - 2] : '\0';
    const auto character = [](char c) {
        return CompletionTrigger{.kind = CompletionTriggerKind::TriggerCharacter, .character = std::string(1, c)};
    };
    if (isIncludeLine(line) && (last == '<' || last == '"' || last == '/')) {
        return character(last);
    }
    if (last == '.') {
        const std::string_view head = line.substr(0, line.size() - 1);
        const std::size_t word = identifierStart(head);
        if (word < head.size()) {
            const char first = head[word];
            return (first >= '0' && first <= '9') ? std::nullopt : std::optional(character('.'));
        }
        return (before == ')' || before == ']') ? std::optional(character('.')) : std::nullopt;
    }
    if (last == '>' && before == '-') {
        return character('>');
    }
    if (last == ':' && before == ':') {
        return character(':');
    }
    if (isIdentifierStartByte(last) && !isIdentifierByte(before)) {
        // The first byte of a word; a UTF-8 continuation byte is never a first byte.
        if (static_cast<unsigned char>(last) >= 0x80 && static_cast<unsigned char>(last) < 0xC0) {
            return std::nullopt;
        }
        return CompletionTrigger{};
    }
    return std::nullopt;
}

sched::Task<Result<CompletionList>> requestCompletion(ILspRequester& server, std::string path, LspPosition position,
                                                      CompletionTrigger trigger, std::stop_token token) {
    auto params = textDocumentPosition(path, position);
    if (!params) {
        co_return std::unexpected(std::move(params.error()));
    }
    Json context = Json::object().set("triggerKind", static_cast<int>(trigger.kind));
    if (trigger.kind == CompletionTriggerKind::TriggerCharacter) {
        context.set("triggerCharacter", trigger.character);
    }
    params->set("context", std::move(context));
    auto result = co_await server.request("textDocument/completion", std::move(*params), std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    co_return parseCompletionList(*result);
}

sched::Task<Result<CompletionItem>> resolveCompletionItem(ILspRequester& server, CompletionItem item,
                                                          std::stop_token token) {
    const std::optional<ServerCapabilities> capabilities = server.serverCapabilities();
    if (item.resolved || !capabilities || !capabilities->completionResolve) {
        item.resolved = true;
        co_return item;
    }
    auto result = co_await server.request("completionItem/resolve", item.raw, std::move(token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    if (!result->isObject())
        co_return fail(ErrorCode::Corrupt, "Completion resolve returned no item");
    Json merged = item.raw;
    for (std::size_t i = 0; i < result->keys().size(); ++i)
        merged.set(result->keys()[i], result->items()[i]);
    CompletionItem resolved = parseCompletionItem(merged);
    // Keep what the popup sorted and filtered by; take the resolved details.
    resolved.label = std::move(item.label);
    resolved.filterText = std::move(item.filterText);
    resolved.sortText = std::move(item.sortText);
    if (!resolved.editRange && item.editRange) {
        resolved.editRange = item.editRange;
        resolved.insertText = std::move(item.insertText);
    }
    resolved.resolved = true;
    co_return resolved;
}

void CompletionModel::setList(CompletionList list) {
    m_items = std::move(list.items);
    m_incomplete = list.isIncomplete;
    refilter();
}

void CompletionModel::setPrefix(std::string_view prefix) {
    m_prefix = std::string(prefix);
    refilter();
}

void CompletionModel::replaceItem(std::size_t index, CompletionItem item) {
    if (index < m_items.size()) {
        m_items[index] = std::move(item);
    }
}

void CompletionModel::refilter() {
    m_entries.clear();
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        if (auto match = fuzzyMatch(m_prefix, m_items[i].filterText)) {
            m_entries.push_back(CompletionEntry{.item = i, .match = std::move(*match)});
        }
    }
    std::ranges::stable_sort(m_entries, [this](const CompletionEntry& a, const CompletionEntry& b) {
        if (a.match.kind != b.match.kind) {
            return a.match.kind < b.match.kind;
        }
        const CompletionItem& x = m_items[a.item];
        const CompletionItem& y = m_items[b.item];
        if (x.deprecated != y.deprecated) {
            return !x.deprecated;
        }
        if ((a.match.kind == MatchKind::WordStarts || a.match.kind == MatchKind::Fuzzy) &&
            a.match.score != b.match.score) {
            return a.match.score > b.match.score;
        }
        if (x.sortText != y.sortText) {
            return x.sortText < y.sortText;
        }
        if (x.label.size() != y.label.size()) {
            return x.label.size() < y.label.size();
        }
        return x.label < y.label;
    });
    m_selected = 0;
    if (!m_entries.empty()) {
        const auto preferred = std::ranges::find_if(m_entries, [this](const auto& entry) {
            const auto& item = m_items[entry.item];
            return item.preselect && !item.deprecated && entry.match.kind == m_entries.front().match.kind;
        });
        if (preferred != m_entries.end())
            m_selected = static_cast<std::size_t>(preferred - m_entries.begin());
    }
}

std::optional<std::size_t> CompletionModel::selected() const noexcept {
    return m_entries.empty() ? std::nullopt : std::optional<std::size_t>(m_selected);
}

void CompletionModel::select(std::size_t index) noexcept {
    if (!m_entries.empty()) {
        m_selected = std::min(index, m_entries.size() - 1);
    }
}

void CompletionModel::moveSelection(int delta) noexcept {
    if (m_entries.empty()) {
        return;
    }
    const auto size = static_cast<std::int64_t>(m_entries.size());
    std::int64_t next = (static_cast<std::int64_t>(m_selected) + delta) % size;
    if (next < 0) {
        next += size;
    }
    m_selected = static_cast<std::size_t>(next);
}

const CompletionItem* CompletionModel::selectedItem() const noexcept {
    const std::optional<std::size_t> index = selectedItemIndex();
    return index ? &m_items[*index] : nullptr;
}

std::optional<std::size_t> CompletionModel::selectedItemIndex() const noexcept {
    return m_entries.empty() ? std::nullopt : std::optional<std::size_t>(m_entries[m_selected].item);
}

Result<CompletionCommit> commitCompletion(const CompletionItem& item, const text::Snapshot& snapshot,
                                          ByteOffset wordStart, ByteOffset caret, PositionEncoding encoding) {
    ByteOffset start = wordStart;
    ByteOffset end = caret;
    if (item.editRange) {
        auto from = fromLspPosition(snapshot, item.editRange->start, encoding);
        auto to = fromLspPosition(snapshot, item.editRange->end, encoding);
        if (!from) {
            return std::unexpected(std::move(from.error()));
        }
        if (!to) {
            return std::unexpected(std::move(to.error()));
        }
        start = std::min(*from, caret);
        end = std::max(*to, caret);
    }
    ParsedSnippet text;
    if (item.snippet) {
        text = parseSnippet(item.insertText);
    } else {
        text.text = item.insertText;
    }
    auto additional = toByteEdits(snapshot, item.additionalTextEdits, encoding);
    if (!additional) {
        return std::unexpected(std::move(additional.error()));
    }
    CompletionCommit out;
    std::int64_t delta = 0; // Size change of the additional edits before the main edit.
    bool placed = false;
    const auto placeMain = [&]() {
        out.edits.push_back(ByteEdit{.range = {start, end}, .text = text.text});
        placed = true;
    };
    for (ByteEdit& edit : *additional) {
        if (edit.range.end <= start) { // Before the main edit (an insert at its start included): shifts it.
            delta += static_cast<std::int64_t>(edit.text.size()) - static_cast<std::int64_t>(edit.range.size());
            out.edits.push_back(std::move(edit));
            continue;
        }
        if (edit.range.begin < end) {
            return fail(ErrorCode::InvalidArgument, "an additional completion edit overlaps the completed word");
        }
        if (!placed) {
            placeMain();
        }
        out.edits.push_back(std::move(edit));
    }
    if (!placed) {
        placeMain();
    }
    const auto shifted = [&](std::uint32_t offsetInText) {
        return ByteOffset{static_cast<std::uint64_t>(static_cast<std::int64_t>(start.value()) + delta + offsetInText)};
    };
    out.caret = shifted(text.caret());
    for (const SnippetTabStop& stop : text.tabStops) {
        out.tabStops.push_back(text::ByteRange{shifted(stop.begin), shifted(stop.end)});
    }
    return out;
}

} // namespace frost::lang
