// SPDX-License-Identifier: Apache-2.0
//
// Shared fixture of the editor-model tests (ADR 0025): texts with caret and selection markers, and
// an Editor over a Document with a ManualClock and the monospace shaper double.
/*
 * Marker notation. '|' is a caret; "«" is a selection's anchor and "»" its head, so "«foo»" selects
 * foo forwards and "»foo«" backwards. The first marker group is the primary selection. parse()
 * strips the markers; show() puts them back for the editor's current state.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/text/document.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/editor/editor.hpp"
#include "frost/text/editor/language.hpp"
#include "frost/text/selection.hpp"
#include "frost/text/text_shaper.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::text::testing {

inline constexpr std::string_view kAnchorMark = "\xC2\xAB"; // «
inline constexpr std::string_view kHeadMark = "\xC2\xBB";   // »

// A marked text split into its text and selections (header comment).
struct Marked {
    std::string text;
    std::vector<Selection> selections;
};

inline Marked parse(std::string_view marked) {
    Marked out;
    std::optional<ByteOffset> anchor;
    std::optional<ByteOffset> head;
    for (std::size_t i = 0; i < marked.size();) {
        const ByteOffset here{out.text.size()};
        if (marked[i] == '|') {
            out.selections.push_back(Selection::caret(here));
            ++i;
        } else if (marked.substr(i, 2) == kAnchorMark) {
            anchor = here;
            i += 2;
        } else if (marked.substr(i, 2) == kHeadMark) {
            head = here;
            i += 2;
        } else {
            out.text += marked[i];
            ++i;
        }
        if (anchor && head) {
            out.selections.push_back(Selection{*anchor, *head});
            anchor.reset();
            head.reset();
        }
    }
    return out;
}

// `text` with the markers of `selections` inserted.
inline std::string show(std::string_view text, const SelectionSet& selections) {
    std::vector<std::pair<std::uint64_t, std::string_view>> marks;
    for (const Selection& s : selections.all()) {
        if (s.empty()) {
            marks.emplace_back(s.head.value(), "|");
        } else {
            marks.emplace_back(s.anchor.value(), kAnchorMark);
            marks.emplace_back(s.head.value(), kHeadMark);
        }
    }
    std::stable_sort(marks.begin(), marks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out;
    std::uint64_t at = 0;
    for (const auto& [offset, mark] : marks) {
        out.append(text.substr(static_cast<std::size_t>(at), static_cast<std::size_t>(offset - at)));
        out.append(mark);
        at = offset;
    }
    out.append(text.substr(static_cast<std::size_t>(at)));
    return out;
}

// An Editor over a Document holding a marked text.
class EditorFixture {
public:
    explicit EditorFixture(std::string_view marked, const LanguageInfo* language = languageById("cpp"),
                           EditorConfig config = {})
        : m_document(makeDocument(marked)), m_editor(m_document, m_shaper, withLanguage(config, language)) {
        const Marked parsed = parse(marked);
        std::vector<Selection> selections = parsed.selections;
        if (selections.empty()) {
            selections.push_back(Selection::caret(ByteOffset{0}));
        }
        m_editor.setState(EditorState::from(SelectionSet::fromSelections(selections, 0)));
    }

    // The text with the current selections marked.
    [[nodiscard]] std::string show() const {
        return testing::show(m_editor.text().text(), m_editor.state().selections);
    }
    [[nodiscard]] std::string text() const { return m_editor.text().text(); }

    // Runs a pure command on the editor's input and applies it; fails the test on an error.
    template<class Command, class... Args>
    void run(Command&& command, Args&&... args) {
        const Result<EditorUpdate> update = m_editor.run(std::forward<Command>(command), std::forward<Args>(args)...);
        ASSERT_TRUE(update.has_value()) << update.error().describe();
    }
    // Types `utf8` one code point at a time, as a keyboard would.
    void typeChars(std::string_view utf8) {
        for (std::size_t i = 0; i < utf8.size();) {
            const std::size_t bytes = decodeUtf8(utf8.substr(i)).bytes;
            run(commands::type, utf8.substr(i, bytes));
            i += bytes;
        }
    }
    void undo() {
        const Result<EditorUpdate> update = m_editor.undo();
        ASSERT_TRUE(update.has_value()) << update.error().describe();
    }
    void redo() {
        const Result<EditorUpdate> update = m_editor.redo();
        ASSERT_TRUE(update.has_value()) << update.error().describe();
    }

    [[nodiscard]] Editor& editor() noexcept { return m_editor; }
    [[nodiscard]] Document& document() noexcept { return m_document; }
    [[nodiscard]] ManualClock& clock() noexcept { return m_clock; }
    [[nodiscard]] const MonospaceTextShaper& shaper() const noexcept { return m_shaper; }

private:
    Document makeDocument(std::string_view marked) {
        Result<Document> document = Document::fromUtf8(parse(marked).text, {}, {}, m_clock);
        EXPECT_TRUE(document.has_value());
        return std::move(*document);
    }
    static EditorConfig withLanguage(EditorConfig config, const LanguageInfo* language) {
        config.language = language;
        return config;
    }

    ManualClock m_clock;
    MonospaceTextShaper m_shaper{1, 4};
    Document m_document;
    Editor m_editor;
};

// Runs one command on a marked C++ text and shows the text and selections afterwards.
template<class Command, class... Args>
std::string applied(std::string_view marked, Command&& command, Args&&... args) {
    EditorFixture fixture(marked);
    fixture.run(std::forward<Command>(command), std::forward<Args>(args)...);
    return fixture.show();
}

} // namespace frost::text::testing
