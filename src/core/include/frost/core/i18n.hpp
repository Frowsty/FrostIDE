// SPDX-License-Identifier: Apache-2.0
//
// frost::i18n: the one entry point for user-visible text (PLAN.md section 2.2 "i18n readiness",
// ADR 0035). Every string a user can read goes through tr(context, source); translations are a
// non-goal in v1, so tr() returns the English source unless a translator is installed, and
// pseudo-localization (accented, 40% longer text) can be switched on to find truncation and
// concatenated sentences. Qt-free; the Qt wrapper is frost::ui::qtr (frost/ui/tr.hpp).
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace frost::i18n {

/*
 * Maps (context, source) to translated text. Implementations must be thread-safe (tr() may run on
 * any thread) and must not call tr() themselves. Never deleted through this interface.
 */
class ITranslator {
public:
    // The translation of `source` (English UTF-8) in `context`, or nullopt to keep the source.
    [[nodiscard]] virtual std::optional<std::string> translate(std::string_view context,
                                                               std::string_view source) const = 0;

protected:
    ITranslator() = default;
    ITranslator(const ITranslator&) = default;
    ITranslator& operator=(const ITranslator&) = default;
    ~ITranslator() = default;
};

// Installs `translator` (nullptr removes it). It must stay alive while installed. Thread-safe
// (an atomic pointer); strings already shown keep their old text until the UI re-reads them.
void setTranslator(const ITranslator* translator) noexcept;
// The installed translator, or nullptr.
[[nodiscard]] const ITranslator* translator() noexcept;

// Switches pseudo-localization of every tr() result on or off. Thread-safe.
void setPseudoLocalization(bool on) noexcept;
[[nodiscard]] bool pseudoLocalization() noexcept;

/*
 * The text to show for `source` in `context`: the translator's result (or `source`), then
 * pseudo-localized when that is on. `context` names the caller's area ("frost::ui::Shell");
 * `source` is a whole English sentence or label in UTF-8, never a fragment to concatenate. Qt
 * placeholders (%1..%9, %n) and mnemonic markers (&) survive pseudo-localization. O(n).
 */
[[nodiscard]] std::string tr(std::string_view context, std::string_view source);

/*
 * The pseudo-localized form of `text`: ASCII letters become accented look-alikes, the text is
 * padded by about 40% (at least one character) with '~' and wrapped in "[" "]". Placeholders
 * (%1..%9, %n, %%), the character after '&' (a mnemonic) and everything after a tab (a menu
 * shortcut column) are kept unchanged. Pure and deterministic. O(n).
 */
[[nodiscard]] std::string pseudoLocalize(std::string_view text);

} // namespace frost::i18n
