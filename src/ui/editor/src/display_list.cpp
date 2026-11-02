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

const DisplayText* EditorDisplayList::findLineNumber(int number) const noexcept {
    const QString wanted = QString::number(number);
    for (const DisplayText& run : gutterText) {
        if (run.text == wanted) {
            return &run;
        }
    }
    return nullptr;
}

} // namespace frost::ui
