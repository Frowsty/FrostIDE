// SPDX-License-Identifier: Apache-2.0
//
// Lookups on the editor display list (frost/ui/editor/display_list.hpp) that tests and the view use.

#include "frost/ui/editor/display_list.hpp"

namespace frost::ui {

const DisplayText* EditorDisplayList::findText(const QString& wanted) const noexcept {
    for (const DisplayText& run : text) {
        if (run.text == wanted) {
            return &run;
        }
    }
    return nullptr;
}

const DisplayText* EditorDisplayList::findTextAt(ByteOffset offset) const noexcept {
    for (const DisplayText& run : text) {
        if (run.source.begin <= offset && offset < run.source.end) {
            return &run;
        }
    }
    return nullptr;
}

const DisplayText* EditorDisplayList::findLineNumber(int number) const noexcept {
    const QString wanted = QString::number(number);
    for (const DisplayText& run : gutterText) {
        if (run.text == wanted) {
            return &run;
        }
    }
    return nullptr;
}

std::vector<const DisplayDecoration*> EditorDisplayList::decorationsWithStyle(DecorationStyle style) const {
    std::vector<const DisplayDecoration*> out;
    for (const DisplayDecoration& decoration : decorations) {
        if (decoration.style == style) {
            out.push_back(&decoration);
        }
    }
    return out;
}

} // namespace frost::ui
