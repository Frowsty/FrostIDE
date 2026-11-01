// SPDX-License-Identifier: Apache-2.0
//
// Keyboard-layout tables and the layout-aware mapping from physical codes to logical keys
// (PLAN.md section 2.2 "Keyboard and IME", ADR 0016 decision 3), strokeFromEvent and the
// read-only active-layout reader.
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/key.hpp"
#include "frost/input/key_event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace frost::input {

// The four character levels of a writing key.
enum class Level : std::uint8_t { Base, Shift, AltGr, AltGrShift };

// Dead and ligature mask bits, one per Level.
inline constexpr std::uint8_t kDeadBase = 1;
inline constexpr std::uint8_t kDeadShift = 2;
inline constexpr std::uint8_t kDeadAltGr = 4;
inline constexpr std::uint8_t kDeadAltGrShift = 8;

// What one writing key produces at each level. 0 means nothing is produced. Bit i of
// deadMask marks level i as a dead key, whose stored character is the spacing form
// (U+005E '^', U+00B4 '´', U+0060 '`', U+007E '~', U+00A8 '¨'); bit i of ligatureMask marks
// a level that produces more than one code point (the first one is stored).
struct KeyLevels {
    char32_t base = 0;
    char32_t shift = 0;
    char32_t altGr = 0;
    char32_t altGrShift = 0;
    std::uint8_t deadMask = 0;
    std::uint8_t ligatureMask = 0;

    [[nodiscard]] constexpr char32_t at(Level level) const noexcept {
        switch (level) {
        case Level::Base: return base;
        case Level::Shift: return shift;
        case Level::AltGr: return altGr;
        case Level::AltGrShift: return altGrShift;
        }
        return 0;
    }
    [[nodiscard]] constexpr bool dead(Level level) const noexcept {
        return (deadMask & (1U << static_cast<unsigned>(level))) != 0;
    }
    friend constexpr bool operator==(const KeyLevels&, const KeyLevels&) noexcept = default;
};

// How a table maps writing codes to logical keys (see the policy comment on detail::computeLayout below).
//   FullTable:      the complete policy over the table's characters (Windows, simulated layouts).
//   LettersByLabel: letters follow the event's keyCharHint, a letter-area key whose hint is
//                   punctuation follows it too (LayoutTable::logicalKey), everything else is
//                   positional (Linux in M1, where no full table is read; ADR 0016 decision 12).
enum class MappingMode : std::uint8_t { FullTable, LettersByLabel };

// One dead-key composition: `dead` (spacing form) followed by `base` produces `composed`.
struct Composition {
    char32_t dead = 0;
    char32_t base = 0;
    char32_t composed = 0;
    friend constexpr bool operator==(const Composition&, const Composition&) noexcept = default;
};

class LayoutTable;

namespace detail {

// Why a table failed validation; None means it is valid.
enum class LayoutError : std::uint8_t {
    None,
    IdTooLong,
    NotWritingKey,
    DuplicateCode,
    BadCodePoint,
    MaskWithoutChar,
    CompositionOrder,
    BadComposition
};

// A description of a LayoutError for messages.
[[nodiscard]] std::string_view describe(LayoutError error) noexcept;

// Validates the inputs and fills `table` (the shared constexpr core of make() and the
// consteval builder). O(K + C) for K key entries and C compositions.
[[nodiscard]] constexpr LayoutError computeLayout(LayoutTable& table, std::string_view id, MappingMode mode,
                                                  std::span<const std::pair<Code, KeyLevels>> keys,
                                                  std::span<const Composition> compositions) noexcept;

// Not constexpr: calling it during constant evaluation makes an invalid consteval-built table a compile error.
void invalidLayoutTable() noexcept;

// The key of one of the 11 US punctuation characters (` - = [ ] \ ; ' , . /), or None. O(1).
[[nodiscard]] constexpr Key usPunctuationKey(char32_t c) noexcept {
    switch (c) {
    case U'`': return Key::Backquote;
    case U'-': return Key::Minus;
    case U'=': return Key::Equal;
    case U'[': return Key::BracketLeft;
    case U']': return Key::BracketRight;
    case U'\\': return Key::Backslash;
    case U';': return Key::Semicolon;
    case U'\'': return Key::Quote;
    case U',': return Key::Comma;
    case U'.': return Key::Period;
    case U'/': return Key::Slash;
    default: return Key::None;
    }
}

} // namespace detail

/*
 * A keyboard layout: the characters of each writing key at four levels, the dead-key
 * compositions, and the logical-key mapping computed from them. A trivially copyable
 * value with no heap (about 1.7 KB); copies are cheap enough for KeymapService to hold one.
 *
 * Lifetime: the composition span must reference static storage, or storage that
 * outlives every copy of the table. The simulated layouts use constexpr arrays; tables
 * read from Windows have none, because the OS composes.
 *
 * Thread safety: immutable after construction, so concurrent reads are safe.
 */
class LayoutTable {
public:
    // Builds a table at run time. Validates: writing keys only, no duplicate codes, no
    // surrogates or values above U+10FFFF, dead and ligature bits only on non-zero
    // characters, compositions sorted by (dead, base) and unique, and an id of at most 23
    // bytes. Errors: InvalidArgument naming the problem. O(K + C).
    [[nodiscard]] static Result<LayoutTable> make(std::string_view id, MappingMode mode,
                                                  std::span<const std::pair<Code, KeyLevels>> keys,
                                                  std::span<const Composition> compositions);

    // The compile-time builder for constinit tables: the same validation, where a failure
    // is a compile error. The spans must reference static storage.
    [[nodiscard]] static consteval LayoutTable build(std::string_view id, MappingMode mode,
                                                     std::span<const std::pair<Code, KeyLevels>> keys,
                                                     std::span<const Composition> compositions) {
        LayoutTable table;
        if (detail::computeLayout(table, id, mode, keys, compositions) != detail::LayoutError::None) {
            detail::invalidLayoutTable();
        }
        return table;
    }

    // The levels of `code`; all zero for non-writing codes. O(1).
    [[nodiscard]] constexpr KeyLevels levels(Code code) const noexcept {
        return isWritingKey(code) ? m_levels[index(code) - index(Code::KeyA)] : KeyLevels{};
    }
    // True if any key has a non-zero AltGr or AltGr+Shift character.
    [[nodiscard]] constexpr bool hasAltGr() const noexcept { return m_hasAltGr; }

    /*
     * The logical key of `code`. keyCharHint (KeyEvent::keyCharHint) is used only in
     * LettersByLabel mode, for writing codes:
     *   - a hint that folds to a-z maps to that letter, on any writing code;
     *   - on a letter-area code (KeyA..KeyZ), any other printable ASCII hint means the key
     *     is not the US letter of its position: one of the 11 US punctuation characters
     *     maps to that key (character dispatch, as FullTable rule 3.3: French ',' on KeyM
     *     is Comma), anything else ('?', '!', a digit) to None;
     *   - otherwise (no hint, a non-ASCII hint such as a Cyrillic letter, or a code outside
     *     the letter area) the positional key.
     * So a punctuation-labelled letter-area key never fires its position's letter bindings.
     * O(1) via a precomputed array, noexcept. Budget: B5.
     */
    [[nodiscard]] constexpr Key logicalKey(Code code, char32_t keyCharHint = 0) const noexcept {
        if (index(code) >= kCodeCount) {
            return Key::None;
        }
        if (m_mode == MappingMode::LettersByLabel && isWritingKey(code) && keyCharHint != 0) {
            const char32_t hint = keyCharHint >= U'A' && keyCharHint <= U'Z' ? keyCharHint + 0x20 : keyCharHint;
            if (hint >= U'a' && hint <= U'z') {
                return static_cast<Key>(index(Key::A) + (hint - U'a'));
            }
            if (isLetterAreaKey(code) && hint > U' ' && hint < 0x7F) {
                return detail::usPunctuationKey(hint);
            }
        }
        return m_logical[index(code)];
    }

    // The base character of the preferred code mapped to `key` (a code mapped by letter or
    // character dispatch beats a positional one; within a class, Code order decides), or
    // 0 when the key is unreachable or is not a writing key. Example: German Key::Minus has
    // the label '-' (code Slash), not 'ß'. O(1).
    [[nodiscard]] constexpr char32_t labelFor(Key key) const noexcept {
        return index(key) < kKeyCount ? m_labelFor[index(key)] : 0;
    }
    // The preferred code of `key`, or Unknown when unreachable. O(1).
    [[nodiscard]] constexpr Code codeFor(Key key) const noexcept {
        return index(key) < kKeyCount ? m_codeFor[index(key)] : Code::Unknown;
    }
    // True if some code maps to `key` (Key::None is never reachable). O(1).
    [[nodiscard]] constexpr bool reachable(Key key) const noexcept {
        return key != Key::None && codeFor(key) != Code::Unknown;
    }

    // The composition of `dead` (spacing form) and `base`, if the table defines one. O(log C).
    [[nodiscard]] std::optional<char32_t> compose(char32_t dead, char32_t base) const noexcept;
    [[nodiscard]] constexpr std::span<const Composition> compositions() const noexcept { return m_compositions; }

    // "<locale>:<KLID>" for read tables, "de-DE:00000407" and so on for simulated ones.
    [[nodiscard]] constexpr std::string_view id() const noexcept { return {m_id.data(), m_idLength}; }
    [[nodiscard]] constexpr MappingMode mappingMode() const noexcept { return m_mode; }

    // Equal when id, mode, levels and composition contents match.
    friend bool operator==(const LayoutTable& a, const LayoutTable& b) noexcept;

private:
    friend constexpr detail::LayoutError detail::computeLayout(LayoutTable&, std::string_view, MappingMode,
                                                               std::span<const std::pair<Code, KeyLevels>>,
                                                               std::span<const Composition>) noexcept;

    constexpr LayoutTable() noexcept = default;

    std::array<KeyLevels, kWritingKeyCount> m_levels{};
    std::array<Key, kCodeCount> m_logical{};
    std::array<Code, kKeyCount> m_codeFor{};
    std::array<char32_t, kKeyCount> m_labelFor{};
    std::span<const Composition> m_compositions;
    std::array<char, 24> m_id{};
    std::uint8_t m_idLength = 0;
    MappingMode m_mode = MappingMode::FullTable;
    bool m_hasAltGr = false;
};

/*
 * The stroke a press matches as, or nullopt when it cannot be a shortcut: releases,
 * modifier keys, IME events, dead keys without Ctrl/Alt/Meta, AltGr text, and keys that map
 * to Key::None. Applies the logical-key mapping, the NumLock-off numpad remap (8 Up, 2
 * Down, 4 Left, 6 Right, 7 Home, 1 End, 9 PageUp, 3 PageDown, 0 Insert, Decimal Delete, 5
 * nothing) and the AltGr policy of ADR 0016 decision 4: an AltGr press that produces text
 * (or whose level in `layout` holds a character or dead key) is text; one that produces
 * nothing matches as Ctrl+Alt (+Shift, +Meta when held); physical Ctrl+Alt matches as
 * Ctrl+Alt even when the OS produced AltGr text. The matcher uses it for steps 6-8, and the
 * "press a shortcut" capture of the Keymap page will. O(1), noexcept. Budget: B5.
 */
[[nodiscard]] std::optional<Stroke> strokeFromEvent(const KeyEvent& event, const LayoutTable& layout) noexcept;

/*
 * The active-layout reader's OS seam. buildLayoutTable asks it, per writing key, for the
 * virtual key of the key's scan code and for the characters at each level, so the reader
 * logic is OS-independent and testable with a scripted query. Implementations only query;
 * they never change the user's input configuration.
 */
class ILayoutQuery {
public:
    // The virtual key for a 9-bit scan code (0 when the layout assigns none).
    [[nodiscard]] virtual std::uint8_t vkFromScan(std::uint16_t scan) const = 0;
    // The characters `vk`/`scan` produce at `level`: -1 for a dead key (out[0] holds the
    // spacing form), 0 for none, n >= 1 for n code points in out (only out[0..min(n,4)) is
    // written). Supplementary characters are whole code points (the Win32 implementation
    // joins UTF-16 surrogate pairs).
    [[nodiscard]] virtual int toUnicode(std::uint8_t vk, std::uint16_t scan, Level level,
                                        std::span<char32_t, 4> out) const = 0;
    // The locale name ("pl-PL") and keyboard layout id ("00000415") of the layout.
    [[nodiscard]] virtual std::string_view localeName() const = 0;
    [[nodiscard]] virtual std::string_view klid() const = 0;

protected:
    ILayoutQuery() = default;
    ILayoutQuery(const ILayoutQuery&) = default;
    ILayoutQuery& operator=(const ILayoutQuery&) = default;
    ~ILayoutQuery() = default;
};

// Builds a FullTable layout from `query`: id "<locale>:<KLID>" (the locale cut so the id
// fits 23 bytes), no compositions. A level producing n > 1 code points stores the first and
// sets its ligature bit; control characters, surrogates and values above U+10FFFF count as
// nothing; codes whose scan code has no virtual key are left empty. Errors: those of
// LayoutTable::make (none for a well-behaved query). About 200 toUnicode calls.
[[nodiscard]] Result<LayoutTable> buildLayoutTable(const ILayoutQuery& query);

/*
 * Reads the calling thread's active keyboard layout.
 *
 * Windows: GetKeyboardLayout(0), then per writing key MapVirtualKeyExW(scan,
 * MAPVK_VSC_TO_VK_EX) and ToUnicodeEx with a zeroed key state whose VK_SHIFT, VK_CONTROL
 * and VK_MENU bits select the level, with flag 0x4 ("do not change keyboard state",
 * Windows 10 1607 and later) so a dead key the user has pending is never consumed. The
 * locale comes from LCIDToLocaleName and the layout id from GetKeyboardLayoutNameW. It
 * only queries the thread's current layout and never changes the user's input
 * configuration. Errors: IoError (with the OS message) when the thread has no layout;
 * callers then fall back to simulatedLayout(SimulatedLayoutId::EnUs) and log.
 *
 * Elsewhere: returns linuxM1Layout(), the en-US table in LettersByLabel mode. This is an
 * M1 limitation (ADR 0016 decision 12): letters follow labels, a letter-area key labelled
 * with punctuation follows its label (or maps to None), everything else is positional.
 *
 * Must be called on the UI thread (the layout is per thread). About 200 ToUnicodeEx
 * calls, well under 1 ms. Called at start-up and on QEvent::KeyboardLayoutChange.
 */
[[nodiscard]] Result<LayoutTable> readActiveLayout();

/*
 * The mapping core, a constexpr function shared by make() and build(). The policy
 * (FullTable mode), per writing code; ADR 0016 decision 3 records the reasons:
 *   1. Non-writing keys (named keys, F-keys, numpad, arrows, Space) map to themselves;
 *      NumpadEnter maps to Enter; CapsLock, NumLock, ScrollLock and modifiers to None.
 *   2. Digit-row keys map positionally to their US digit, whatever they produce (AZERTY
 *      Digit1 types '&' but is Digit1, so Alt+1 stays Alt+1).
 *   3. Other writing keys: let c be the base character, ASCII case-folded.
 *      3.1 c == 0: None.
 *      3.2 c is a Latin letter a-z: that letter (QWERTZ Ctrl+Z is the key labelled Z).
 *      3.3 c is one of the 11 US punctuation characters: that key (character dispatch;
 *          French ',' on code KeyM is Comma).
 *      3.4 otherwise (non-ASCII, a dead key, or other ASCII such as '#', '+', '<'): on a US
 *          punctuation position, the US key of that position; on a letter-area code, the
 *          US letter of the position if no writing key's base folds to that letter, else
 *          None; on IntlBackslash, IntlRo and IntlYen, None.
 *      3.5 a table with no Latin letter at base on any letter-area key (a non-Latin
 *          layout) maps every writing key positionally (Intl* keys are None).
 *   4. LettersByLabel mode: writing keys are positional; logicalKey() lets a hint letter win,
 *      and dispatches a letter-area key with a punctuation hint by character (or to None).
 * Two physical keys may map to the same logical key (harmless); a logical key no code maps
 * to is unreachable, and KeymapService reports bindings on it.
 */
constexpr detail::LayoutError detail::computeLayout(LayoutTable& table, std::string_view id, MappingMode mode,
                                                    std::span<const std::pair<Code, KeyLevels>> keys,
                                                    std::span<const Composition> compositions) noexcept {
    constexpr auto validChar = [](char32_t c) { return c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF); };
    constexpr auto fold = [](char32_t c) { return c >= U'A' && c <= U'Z' ? c + 0x20 : c; };
    constexpr auto isPunct = [](char32_t c) { return usPunctuationKey(c) != Key::None; };
    // The key a US-positional mapping gives a writing code: its usBaseChar's key (Intl* None).
    constexpr auto positional = [](Code code) -> Key {
        const std::size_t i = index(code);
        if (isLetterAreaKey(code)) {
            return static_cast<Key>(index(Key::A) + (i - index(Code::KeyA)));
        }
        if (isDigitRowKey(code)) {
            return static_cast<Key>(index(Key::Digit0) + (i - index(Code::Digit0)));
        }
        switch (code) {
        case Code::Minus: return Key::Minus;
        case Code::Equal: return Key::Equal;
        case Code::BracketLeft: return Key::BracketLeft;
        case Code::BracketRight: return Key::BracketRight;
        case Code::Backslash: return Key::Backslash;
        case Code::Semicolon: return Key::Semicolon;
        case Code::Quote: return Key::Quote;
        case Code::Backquote: return Key::Backquote;
        case Code::Comma: return Key::Comma;
        case Code::Period: return Key::Period;
        case Code::Slash: return Key::Slash;
        default: return Key::None;
        }
    };

    if (id.size() > table.m_id.size() - 1) {
        return LayoutError::IdTooLong;
    }
    std::array<bool, kCodeCount> seen{};
    for (const auto& [code, levels] : keys) {
        if (!isWritingKey(code)) {
            return LayoutError::NotWritingKey;
        }
        if (seen[index(code)]) {
            return LayoutError::DuplicateCode;
        }
        seen[index(code)] = true;
        if ((levels.deadMask | levels.ligatureMask) > 0x0F) {
            return LayoutError::MaskWithoutChar;
        }
        for (unsigned level = 0; level < 4; ++level) {
            const char32_t c = levels.at(static_cast<Level>(level));
            if (!validChar(c)) {
                return LayoutError::BadCodePoint;
            }
            const unsigned bit = 1U << level;
            if (c == 0 && ((levels.deadMask & bit) != 0 || (levels.ligatureMask & bit) != 0)) {
                return LayoutError::MaskWithoutChar;
            }
        }
    }
    for (std::size_t i = 0; i < compositions.size(); ++i) {
        const Composition& c = compositions[i];
        if (c.dead == 0 || c.base == 0 || c.composed == 0 || !validChar(c.dead) || !validChar(c.base) ||
            !validChar(c.composed)) {
            return LayoutError::BadComposition;
        }
        if (i > 0) {
            const Composition& p = compositions[i - 1];
            if (p.dead > c.dead || (p.dead == c.dead && p.base >= c.base)) {
                return LayoutError::CompositionOrder;
            }
        }
    }

    // Validation passed: fill the table.
    table = LayoutTable();
    for (std::size_t i = 0; i < id.size(); ++i) {
        table.m_id[i] = id[i];
    }
    table.m_idLength = static_cast<std::uint8_t>(id.size());
    table.m_mode = mode;
    table.m_compositions = compositions;
    for (const auto& [code, levels] : keys) {
        table.m_levels[index(code) - index(Code::KeyA)] = levels;
        if (levels.altGr != 0 || levels.altGrShift != 0) {
            table.m_hasAltGr = true;
        }
    }

    // Which Latin letters some writing key produces at base, and whether the layout is Latin at all.
    std::array<bool, 26> letterAtBase{};
    bool latin = false;
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const KeyLevels& levels = table.m_levels[i - index(Code::KeyA)];
        const char32_t c = fold(levels.base);
        if (!levels.dead(Level::Base) && c >= U'a' && c <= U'z') {
            letterAtBase[c - U'a'] = true;
            if (isLetterAreaKey(static_cast<Code>(i))) {
                latin = true;
            }
        }
    }

    // dispatched[c]: the code's key came from its character (rules 3.2 and 3.3), which
    // makes it the preferred code of that key over positional ones.
    std::array<bool, kCodeCount> dispatched{};
    for (std::size_t i = 0; i < kCodeCount; ++i) {
        const Code code = static_cast<Code>(i);
        Key key = Key::None;
        if (!isWritingKey(code)) {
            // Rule 1.
            if (code >= Code::Space && code <= Code::ArrowDown) {
                key = static_cast<Key>(index(Key::Space) + (i - index(Code::Space)));
            } else if (code >= Code::F1 && code <= Code::F24) {
                key = static_cast<Key>(index(Key::F1) + (i - index(Code::F1)));
            } else if (code >= Code::Numpad0 && code <= Code::NumpadDecimal) {
                key = static_cast<Key>(index(Key::Numpad0) + (i - index(Code::Numpad0)));
            } else if (code == Code::NumpadEnter) {
                key = Key::Enter;
            } else if (code == Code::PrintScreen) {
                key = Key::PrintScreen;
            } else if (code == Code::Pause) {
                key = Key::Pause;
            } else if (code == Code::ContextMenu) {
                key = Key::ContextMenu;
            }
        } else if (mode == MappingMode::LettersByLabel || !latin || isDigitRowKey(code)) {
            // Rules 2, 3.5 and 4.
            key = positional(code);
        } else {
            const KeyLevels& levels = table.m_levels[i - index(Code::KeyA)];
            const char32_t c = fold(levels.base);
            if (c == 0) {
                key = Key::None; // Rule 3.1.
            } else if (!levels.dead(Level::Base) && c >= U'a' && c <= U'z') {
                key = static_cast<Key>(index(Key::A) + (c - U'a')); // Rule 3.2.
                dispatched[i] = true;
            } else if (!levels.dead(Level::Base) && isPunct(c)) {
                key = usPunctuationKey(c); // Rule 3.3.
                dispatched[i] = true;
            } else if (isUsPunctuationKey(code)) {
                key = positional(code); // Rule 3.4, punctuation position.
            } else if (isLetterAreaKey(code)) {
                const std::size_t letter = i - index(Code::KeyA);
                key = letterAtBase[letter] ? Key::None : positional(code); // Rule 3.4, letter area.
            }
            // Rule 3.4, Intl* keys: None.
        }
        table.m_logical[i] = key;
    }

    // Reverse arrays: the preferred code per key (dispatched beats positional; then Code order).
    std::array<bool, kKeyCount> haveDispatched{};
    for (std::size_t i = 0; i < kCodeCount; ++i) {
        const Key key = table.m_logical[i];
        if (key == Key::None) {
            continue;
        }
        const std::size_t k = index(key);
        const bool better = table.m_codeFor[k] == Code::Unknown || (dispatched[i] && !haveDispatched[k]);
        if (better) {
            table.m_codeFor[k] = static_cast<Code>(i);
            haveDispatched[k] = dispatched[i];
            table.m_labelFor[k] = isWritingKey(static_cast<Code>(i)) ? table.m_levels[i - index(Code::KeyA)].base : 0;
        }
    }
    return LayoutError::None;
}

} // namespace frost::input
