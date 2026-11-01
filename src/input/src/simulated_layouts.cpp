// SPDX-License-Identifier: Apache-2.0
//
// The simulated layout tables (simulated_keyboard.hpp): en-US, de-DE, pl-PL and fr-FR as
// constinit LayoutTables built by the consteval builder, so an invalid table is a compile
// error and no constructor runs at start-up.
/*
 * A model of the Windows layouts named; X11 layouts define more level-3 characters.
 * Every cell was transcribed from, and checked against, Microsoft's public layout charts
 * (learn.microsoft.com/globalization/windows-keyboard-layouts, generated from Windows 11
 * 26H2): US (KBDUS, 00000409), German (KBDGR, 00000407), Polish (Programmers) (KBDPL1,
 * 00000415) and French (Legacy, AZERTY) (KBDFR, 0000040C). Columns: base, Shift, AltGr
 * (Ctrl+Alt), AltGr+Shift, dead mask. The compositions are every dead key's entries in the
 * charts (the vowels and y, n, c and the Polish letters where defined, their uppercase,
 * and Space).
 */

#include "frost/input/layout.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include <array>
#include <utility>

namespace frost::input {

namespace {

constexpr std::pair<Code, KeyLevels> key(Code code, char32_t base, char32_t shift, char32_t altGr = 0,
                                         char32_t altGrShift = 0, std::uint8_t deadMask = 0) {
    return {code, KeyLevels{base, shift, altGr, altGrShift, deadMask, 0}};
}

// en-US (KBDUS). IntlBackslash is the ISO key, which types '\' and '|' on US.
constexpr std::array kEnUsKeys = {
    key(Code::Backquote, U'`', U'~'),    key(Code::Digit1, U'1', U'!'),         key(Code::Digit2, U'2', U'@'),
    key(Code::Digit3, U'3', U'#'),       key(Code::Digit4, U'4', U'$'),         key(Code::Digit5, U'5', U'%'),
    key(Code::Digit6, U'6', U'^'),       key(Code::Digit7, U'7', U'&'),         key(Code::Digit8, U'8', U'*'),
    key(Code::Digit9, U'9', U'('),       key(Code::Digit0, U'0', U')'),         key(Code::Minus, U'-', U'_'),
    key(Code::Equal, U'=', U'+'),        key(Code::KeyQ, U'q', U'Q'),           key(Code::KeyW, U'w', U'W'),
    key(Code::KeyE, U'e', U'E'),         key(Code::KeyR, U'r', U'R'),           key(Code::KeyT, U't', U'T'),
    key(Code::KeyY, U'y', U'Y'),         key(Code::KeyU, U'u', U'U'),           key(Code::KeyI, U'i', U'I'),
    key(Code::KeyO, U'o', U'O'),         key(Code::KeyP, U'p', U'P'),           key(Code::BracketLeft, U'[', U'{'),
    key(Code::BracketRight, U']', U'}'), key(Code::KeyA, U'a', U'A'),           key(Code::KeyS, U's', U'S'),
    key(Code::KeyD, U'd', U'D'),         key(Code::KeyF, U'f', U'F'),           key(Code::KeyG, U'g', U'G'),
    key(Code::KeyH, U'h', U'H'),         key(Code::KeyJ, U'j', U'J'),           key(Code::KeyK, U'k', U'K'),
    key(Code::KeyL, U'l', U'L'),         key(Code::Semicolon, U';', U':'),      key(Code::Quote, U'\'', U'"'),
    key(Code::Backslash, U'\\', U'|'),   key(Code::IntlBackslash, U'\\', U'|'), key(Code::KeyZ, U'z', U'Z'),
    key(Code::KeyX, U'x', U'X'),         key(Code::KeyC, U'c', U'C'),           key(Code::KeyV, U'v', U'V'),
    key(Code::KeyB, U'b', U'B'),         key(Code::KeyN, U'n', U'N'),           key(Code::KeyM, U'm', U'M'),
    key(Code::Comma, U',', U'<'),        key(Code::Period, U'.', U'>'),         key(Code::Slash, U'/', U'?'),
};

// de-DE (KBDGR). Dead keys: '^' (Backquote), '´' and '`' (Equal, base and Shift).
constexpr std::array kDeDeKeys = {
    key(Code::Backquote, U'^', U'°', 0, 0, kDeadBase),
    key(Code::Digit1, U'1', U'!'),
    key(Code::Digit2, U'2', U'"', U'²'),
    key(Code::Digit3, U'3', U'§', U'³'),
    key(Code::Digit4, U'4', U'$'),
    key(Code::Digit5, U'5', U'%'),
    key(Code::Digit6, U'6', U'&'),
    key(Code::Digit7, U'7', U'/', U'{'),
    key(Code::Digit8, U'8', U'(', U'['),
    key(Code::Digit9, U'9', U')', U']'),
    key(Code::Digit0, U'0', U'=', U'}'),
    key(Code::Minus, U'ß', U'?', U'\\', U'ẞ'),
    key(Code::Equal, U'´', U'`', 0, 0, kDeadBase | kDeadShift),
    key(Code::KeyQ, U'q', U'Q', U'@'),
    key(Code::KeyW, U'w', U'W'),
    key(Code::KeyE, U'e', U'E', U'€'),
    key(Code::KeyR, U'r', U'R'),
    key(Code::KeyT, U't', U'T'),
    key(Code::KeyY, U'z', U'Z'),
    key(Code::KeyU, U'u', U'U'),
    key(Code::KeyI, U'i', U'I'),
    key(Code::KeyO, U'o', U'O'),
    key(Code::KeyP, U'p', U'P'),
    key(Code::BracketLeft, U'ü', U'Ü'),
    key(Code::BracketRight, U'+', U'*', U'~'),
    key(Code::KeyA, U'a', U'A'),
    key(Code::KeyS, U's', U'S'),
    key(Code::KeyD, U'd', U'D'),
    key(Code::KeyF, U'f', U'F'),
    key(Code::KeyG, U'g', U'G'),
    key(Code::KeyH, U'h', U'H'),
    key(Code::KeyJ, U'j', U'J'),
    key(Code::KeyK, U'k', U'K'),
    key(Code::KeyL, U'l', U'L'),
    key(Code::Semicolon, U'ö', U'Ö'),
    key(Code::Quote, U'ä', U'Ä'),
    key(Code::Backslash, U'#', U'\''),
    key(Code::IntlBackslash, U'<', U'>', U'|'),
    key(Code::KeyZ, U'y', U'Y'),
    key(Code::KeyX, U'x', U'X'),
    key(Code::KeyC, U'c', U'C'),
    key(Code::KeyV, U'v', U'V'),
    key(Code::KeyB, U'b', U'B'),
    key(Code::KeyN, U'n', U'N'),
    key(Code::KeyM, U'm', U'M', U'µ'),
    key(Code::Comma, U',', U';'),
    key(Code::Period, U'.', U':'),
    key(Code::Slash, U'-', U'_'),
};

constexpr std::array kDeDeCompositions = {
    Composition{U'^', U' ', U'^'}, Composition{U'^', U'A', U'Â'}, Composition{U'^', U'E', U'Ê'},
    Composition{U'^', U'I', U'Î'}, Composition{U'^', U'O', U'Ô'}, Composition{U'^', U'U', U'Û'},
    Composition{U'^', U'a', U'â'}, Composition{U'^', U'e', U'ê'}, Composition{U'^', U'i', U'î'},
    Composition{U'^', U'o', U'ô'}, Composition{U'^', U'u', U'û'}, Composition{U'`', U' ', U'`'},
    Composition{U'`', U'A', U'À'}, Composition{U'`', U'E', U'È'}, Composition{U'`', U'I', U'Ì'},
    Composition{U'`', U'O', U'Ò'}, Composition{U'`', U'U', U'Ù'}, Composition{U'`', U'a', U'à'},
    Composition{U'`', U'e', U'è'}, Composition{U'`', U'i', U'ì'}, Composition{U'`', U'o', U'ò'},
    Composition{U'`', U'u', U'ù'}, Composition{U'´', U' ', U'´'}, Composition{U'´', U'A', U'Á'},
    Composition{U'´', U'E', U'É'}, Composition{U'´', U'I', U'Í'}, Composition{U'´', U'O', U'Ó'},
    Composition{U'´', U'U', U'Ú'}, Composition{U'´', U'Y', U'Ý'}, Composition{U'´', U'a', U'á'},
    Composition{U'´', U'e', U'é'}, Composition{U'´', U'i', U'í'}, Composition{U'´', U'o', U'ó'},
    Composition{U'´', U'u', U'ú'}, Composition{U'´', U'y', U'ý'},
};

// pl-PL (KBDPL1, Polish (Programmers)): US at base and Shift; Polish letters on AltGr; '~' is dead.
constexpr std::array kPlPlKeys = {
    key(Code::Backquote, U'`', U'~', 0, 0, kDeadShift),
    key(Code::Digit1, U'1', U'!'),
    key(Code::Digit2, U'2', U'@'),
    key(Code::Digit3, U'3', U'#'),
    key(Code::Digit4, U'4', U'$'),
    key(Code::Digit5, U'5', U'%'),
    key(Code::Digit6, U'6', U'^'),
    key(Code::Digit7, U'7', U'&'),
    key(Code::Digit8, U'8', U'*'),
    key(Code::Digit9, U'9', U'('),
    key(Code::Digit0, U'0', U')'),
    key(Code::Minus, U'-', U'_'),
    key(Code::Equal, U'=', U'+'),
    key(Code::KeyQ, U'q', U'Q'),
    key(Code::KeyW, U'w', U'W'),
    key(Code::KeyE, U'e', U'E', U'ę', U'Ę'),
    key(Code::KeyR, U'r', U'R'),
    key(Code::KeyT, U't', U'T'),
    key(Code::KeyY, U'y', U'Y'),
    key(Code::KeyU, U'u', U'U', U'€'),
    key(Code::KeyI, U'i', U'I'),
    key(Code::KeyO, U'o', U'O', U'ó', U'Ó'),
    key(Code::KeyP, U'p', U'P'),
    key(Code::BracketLeft, U'[', U'{'),
    key(Code::BracketRight, U']', U'}'),
    key(Code::KeyA, U'a', U'A', U'ą', U'Ą'),
    key(Code::KeyS, U's', U'S', U'ś', U'Ś'),
    key(Code::KeyD, U'd', U'D'),
    key(Code::KeyF, U'f', U'F'),
    key(Code::KeyG, U'g', U'G'),
    key(Code::KeyH, U'h', U'H'),
    key(Code::KeyJ, U'j', U'J'),
    key(Code::KeyK, U'k', U'K'),
    key(Code::KeyL, U'l', U'L', U'ł', U'Ł'),
    key(Code::Semicolon, U';', U':'),
    key(Code::Quote, U'\'', U'"'),
    key(Code::Backslash, U'\\', U'|'),
    key(Code::IntlBackslash, U'\\', U'|'),
    key(Code::KeyZ, U'z', U'Z', U'ż', U'Ż'),
    key(Code::KeyX, U'x', U'X', U'ź', U'Ź'),
    key(Code::KeyC, U'c', U'C', U'ć', U'Ć'),
    key(Code::KeyV, U'v', U'V'),
    key(Code::KeyB, U'b', U'B'),
    key(Code::KeyN, U'n', U'N', U'ń', U'Ń'),
    key(Code::KeyM, U'm', U'M'),
    key(Code::Comma, U',', U'<'),
    key(Code::Period, U'.', U'>'),
    key(Code::Slash, U'/', U'?'),
};

constexpr std::array kPlPlCompositions = {
    Composition{U'~', U' ', U'~'}, Composition{U'~', U'A', U'Ą'}, Composition{U'~', U'C', U'Ć'},
    Composition{U'~', U'E', U'Ę'}, Composition{U'~', U'L', U'Ł'}, Composition{U'~', U'N', U'Ń'},
    Composition{U'~', U'O', U'Ó'}, Composition{U'~', U'S', U'Ś'}, Composition{U'~', U'X', U'Ź'},
    Composition{U'~', U'Z', U'Ż'}, Composition{U'~', U'a', U'ą'}, Composition{U'~', U'c', U'ć'},
    Composition{U'~', U'e', U'ę'}, Composition{U'~', U'l', U'ł'}, Composition{U'~', U'n', U'ń'},
    Composition{U'~', U'o', U'ó'}, Composition{U'~', U's', U'ś'}, Composition{U'~', U'x', U'ź'},
    Composition{U'~', U'z', U'ż'},
};

// fr-FR (KBDFR, French (Legacy, AZERTY)). Dead keys: '^' and '¨' (BracketLeft), '~' (AltGr+Digit2),
// '`' (AltGr+Digit7).
constexpr std::array kFrFrKeys = {
    key(Code::Backquote, U'²', 0),
    key(Code::Digit1, U'&', U'1'),
    key(Code::Digit2, U'é', U'2', U'~', 0, kDeadAltGr),
    key(Code::Digit3, U'"', U'3', U'#'),
    key(Code::Digit4, U'\'', U'4', U'{'),
    key(Code::Digit5, U'(', U'5', U'['),
    key(Code::Digit6, U'-', U'6', U'|'),
    key(Code::Digit7, U'è', U'7', U'`', 0, kDeadAltGr),
    key(Code::Digit8, U'_', U'8', U'\\'),
    key(Code::Digit9, U'ç', U'9', U'^'),
    key(Code::Digit0, U'à', U'0', U'@'),
    key(Code::Minus, U')', U'°', U']'),
    key(Code::Equal, U'=', U'+', U'}'),
    key(Code::KeyQ, U'a', U'A'),
    key(Code::KeyW, U'z', U'Z'),
    key(Code::KeyE, U'e', U'E', U'€'),
    key(Code::KeyR, U'r', U'R'),
    key(Code::KeyT, U't', U'T'),
    key(Code::KeyY, U'y', U'Y'),
    key(Code::KeyU, U'u', U'U'),
    key(Code::KeyI, U'i', U'I'),
    key(Code::KeyO, U'o', U'O'),
    key(Code::KeyP, U'p', U'P'),
    key(Code::BracketLeft, U'^', U'¨', 0, 0, kDeadBase | kDeadShift),
    key(Code::BracketRight, U'$', U'£', U'¤'),
    key(Code::KeyA, U'q', U'Q'),
    key(Code::KeyS, U's', U'S'),
    key(Code::KeyD, U'd', U'D'),
    key(Code::KeyF, U'f', U'F'),
    key(Code::KeyG, U'g', U'G'),
    key(Code::KeyH, U'h', U'H'),
    key(Code::KeyJ, U'j', U'J'),
    key(Code::KeyK, U'k', U'K'),
    key(Code::KeyL, U'l', U'L'),
    key(Code::Semicolon, U'm', U'M'),
    key(Code::Quote, U'ù', U'%'),
    key(Code::Backslash, U'*', U'µ'),
    key(Code::IntlBackslash, U'<', U'>'),
    key(Code::KeyZ, U'w', U'W'),
    key(Code::KeyX, U'x', U'X'),
    key(Code::KeyC, U'c', U'C'),
    key(Code::KeyV, U'v', U'V'),
    key(Code::KeyB, U'b', U'B'),
    key(Code::KeyN, U'n', U'N'),
    key(Code::KeyM, U',', U'?'),
    key(Code::Comma, U';', U'.'),
    key(Code::Period, U':', U'/'),
    key(Code::Slash, U'!', U'§'),
};

constexpr std::array kFrFrCompositions = {
    Composition{U'^', U' ', U'^'}, Composition{U'^', U'A', U'Â'}, Composition{U'^', U'E', U'Ê'},
    Composition{U'^', U'I', U'Î'}, Composition{U'^', U'O', U'Ô'}, Composition{U'^', U'U', U'Û'},
    Composition{U'^', U'a', U'â'}, Composition{U'^', U'e', U'ê'}, Composition{U'^', U'i', U'î'},
    Composition{U'^', U'o', U'ô'}, Composition{U'^', U'u', U'û'}, Composition{U'`', U' ', U'`'},
    Composition{U'`', U'A', U'À'}, Composition{U'`', U'E', U'È'}, Composition{U'`', U'I', U'Ì'},
    Composition{U'`', U'O', U'Ò'}, Composition{U'`', U'U', U'Ù'}, Composition{U'`', U'a', U'à'},
    Composition{U'`', U'e', U'è'}, Composition{U'`', U'i', U'ì'}, Composition{U'`', U'o', U'ò'},
    Composition{U'`', U'u', U'ù'}, Composition{U'~', U' ', U'~'}, Composition{U'~', U'A', U'Ã'},
    Composition{U'~', U'N', U'Ñ'}, Composition{U'~', U'O', U'Õ'}, Composition{U'~', U'a', U'ã'},
    Composition{U'~', U'n', U'ñ'}, Composition{U'~', U'o', U'õ'}, Composition{U'¨', U' ', U'¨'},
    Composition{U'¨', U'A', U'Ä'}, Composition{U'¨', U'E', U'Ë'}, Composition{U'¨', U'I', U'Ï'},
    Composition{U'¨', U'O', U'Ö'}, Composition{U'¨', U'U', U'Ü'}, Composition{U'¨', U'a', U'ä'},
    Composition{U'¨', U'e', U'ë'}, Composition{U'¨', U'i', U'ï'}, Composition{U'¨', U'o', U'ö'},
    Composition{U'¨', U'u', U'ü'}, Composition{U'¨', U'y', U'ÿ'},
};

static_assert(kEnUsKeys.size() == 48 && kDeDeKeys.size() == 48 && kPlPlKeys.size() == 48 && kFrFrKeys.size() == 48,
              "each simulated layout holds the 48 ANSI/ISO writing keys");

constinit const LayoutTable kEnUs =
    LayoutTable::build("en-US:00000409", MappingMode::FullTable, kEnUsKeys, std::span<const Composition>());
constinit const LayoutTable kDeDe =
    LayoutTable::build("de-DE:00000407", MappingMode::FullTable, kDeDeKeys, kDeDeCompositions);
constinit const LayoutTable kPlPl =
    LayoutTable::build("pl-PL:00000415", MappingMode::FullTable, kPlPlKeys, kPlPlCompositions);
constinit const LayoutTable kFrFr =
    LayoutTable::build("fr-FR:0000040C", MappingMode::FullTable, kFrFrKeys, kFrFrCompositions);
constinit const LayoutTable kLinuxM1 =
    LayoutTable::build("en-US:LettersByLabel", MappingMode::LettersByLabel, kEnUsKeys, std::span<const Composition>());

} // namespace

const LayoutTable& simulatedLayout(SimulatedLayoutId id) noexcept {
    switch (id) {
    case SimulatedLayoutId::EnUs: return kEnUs;
    case SimulatedLayoutId::DeDe: return kDeDe;
    case SimulatedLayoutId::PlPl: return kPlPl;
    case SimulatedLayoutId::FrFr: return kFrFr;
    }
    return kEnUs;
}

const LayoutTable& linuxM1Layout() noexcept { return kLinuxM1; }

std::string_view simulatedLayoutName(SimulatedLayoutId id) noexcept {
    switch (id) {
    case SimulatedLayoutId::EnUs: return "en-US";
    case SimulatedLayoutId::DeDe: return "de-DE";
    case SimulatedLayoutId::PlPl: return "pl-PL";
    case SimulatedLayoutId::FrFr: return "fr-FR";
    }
    return "?";
}

} // namespace frost::input
