// SPDX-License-Identifier: Apache-2.0
//
// The item data roles of the result rows in the Find in Files popup's list and the Find tool
// window's tree (ADR 0045), public so tests and later views (a Find Usages window) can read rows.
#pragma once

#include <Qt>

namespace frost::ui {

enum FindItemRole : int {
    kFindFileIndexRole = Qt::UserRole + 1, // int: the file in the session.
    kFindMatchIndexRole,                   // int: the match in the file; -1 for a file row.
    kFindHighlightStartRole,               // int: UTF-16 start of the match in the display text.
    kFindHighlightLengthRole,              // int: UTF-16 length of the match in the display text.
    kFindDetailRole,                       // QString: drawn right-aligned in the secondary colour.
};

} // namespace frost::ui
