// SPDX-License-Identifier: Apache-2.0
// UI strings for the frost-lint parity-claim tests; the claims below are seeded.
#include "about.hpp"

// A comment may say "complete replacement"; only strings are UI text.
QString aboutText() {
    return text("FrostIDE is a complete "
                "replacement for JetBrains Rider.");
}

QString tagline() { return text("1:1 Rider parity for C++ and C#"); }

QString honest() { return text("Rider-style workflows for C++, Lua and FrostEngine; see the roadmap."); }
