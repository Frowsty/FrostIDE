// SPDX-License-Identifier: Apache-2.0
//
// frost::i18n (frost/core/i18n.hpp): the translator hook, the pseudo-localization switch and the
// pseudo-localization transform. Both switches are constinit atomics, so there is no global
// constructor or exit-time destructor (PLAN.md section 2.1).

#include "frost/core/i18n.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace frost::i18n {

namespace {

constinit std::atomic<const ITranslator*> g_translator{nullptr};
constinit std::atomic<bool> g_pseudo{false};

// Accented look-alikes of a-z and A-Z, UTF-8.
constexpr std::array<std::string_view, 26> kLower{
    "\xC3\xA1", "\xC6\x80", "\xC3\xA7", "\xC4\x8F",     "\xC3\xA9", "\xC6\x92",     "\xC4\x9D", "\xC4\xA5", "\xC3\xAD",
    "\xC4\xB5", "\xC4\xB7", "\xC4\xBA", "\xC9\xB1",     "\xC3\xB1", "\xC3\xB6",     "\xC3\xBE", "\xC7\xAB", "\xC5\x95",
    "\xC5\xA1", "\xC5\xA3", "\xC3\xBC", "\xE1\xB9\xBD", "\xC5\xB5", "\xE1\xBA\x8B", "\xC3\xBD", "\xC5\xBE"};
constexpr std::array<std::string_view, 26> kUpper{
    "\xC3\x85", "\xC6\x81", "\xC3\x87", "\xC4\x8E",     "\xC3\x89", "\xC6\x91",     "\xC4\x9C", "\xC4\xA4", "\xC3\x8D",
    "\xC4\xB4", "\xC4\xB6", "\xC4\xB9", "\xE1\xB9\x80", "\xC3\x91", "\xC3\x96",     "\xC3\x9E", "\xC7\xAA", "\xC5\x94",
    "\xC5\xA0", "\xC5\xA2", "\xC3\x9C", "\xE1\xB9\xBC", "\xC5\xB4", "\xE1\xBA\x8A", "\xC3\x9D", "\xC5\xBD"};

// True for the first byte of a UTF-8 sequence (or an ASCII byte).
constexpr bool isLeadByte(char c) noexcept { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }

} // namespace

void setTranslator(const ITranslator* translator) noexcept {
    g_translator.store(translator, std::memory_order_release);
}

const ITranslator* translator() noexcept { return g_translator.load(std::memory_order_acquire); }

void setPseudoLocalization(bool on) noexcept { g_pseudo.store(on, std::memory_order_relaxed); }

bool pseudoLocalization() noexcept { return g_pseudo.load(std::memory_order_relaxed); }

std::string tr(std::string_view context, std::string_view source) {
    std::string text;
    if (const ITranslator* active = translator(); active != nullptr) {
        if (std::optional<std::string> translated = active->translate(context, source); translated) {
            text = std::move(*translated);
        } else {
            text = std::string(source);
        }
    } else {
        text = std::string(source);
    }
    return pseudoLocalization() ? pseudoLocalize(text) : text;
}

std::string pseudoLocalize(std::string_view text) {
    // A menu label's shortcut column (after '\t') is a key name, not prose: keep it as is.
    const std::size_t tab = text.find('\t');
    const std::string_view body = text.substr(0, tab);
    const std::string_view tail = tab == std::string_view::npos ? std::string_view() : text.substr(tab);

    std::string out;
    out.reserve(body.size() * 2 + tail.size() + 8);
    out.push_back('[');
    std::size_t characters = 0;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (isLeadByte(c)) {
            ++characters;
        }
        const bool hasNext = i + 1 < body.size();
        if (c == '%' && hasNext) {
            const char next = body[i + 1];
            if ((next >= '1' && next <= '9') || next == 'n' || next == '%') {
                out.push_back(c);
                out.push_back(next);
                ++i;
                ++characters;
                continue;
            }
        }
        if (c == '&' && hasNext) {
            // The mnemonic letter (or a literal "&&") stays readable to the shortcut machinery.
            out.push_back(c);
            out.push_back(body[i + 1]);
            ++i;
            ++characters;
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            out += kLower[static_cast<std::size_t>(c - 'a')];
        } else if (c >= 'A' && c <= 'Z') {
            out += kUpper[static_cast<std::size_t>(c - 'A')];
        } else {
            out.push_back(c);
        }
    }
    // About 40% longer, rounded up, at least one character.
    const std::size_t padding = characters == 0 ? 1 : (characters * 2 + 4) / 5;
    out.push_back(' ');
    out.append(padding, '~');
    out.push_back(']');
    out += tail;
    return out;
}

} // namespace frost::i18n
