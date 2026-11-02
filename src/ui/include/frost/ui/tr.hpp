// SPDX-License-Identifier: Apache-2.0
//
// qtr: frost::i18n::tr for Qt widgets (PLAN.md section 2.2 "i18n readiness", ADR 0038). Every
// user-visible QString the UI layer builds from source text goes through it. It is not called
// tr, because inside a QObject member an unqualified tr(...) would silently resolve to
// QObject::tr with the second argument taken as a disambiguation.
#pragma once

#include <QString>

#include <string_view>

namespace frost::ui {

// frost::i18n::tr(context, source) as a QString (UTF-8 to UTF-16). Thread-safe. O(n).
[[nodiscard]] QString qtr(std::string_view context, std::string_view source);

// `text` with every '&' doubled, so a label taken from data (an action or file name) shows its
// ampersands instead of turning one into a mnemonic. O(n).
[[nodiscard]] QString escapeMnemonics(const QString& text);

} // namespace frost::ui
