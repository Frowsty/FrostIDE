// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the terminal's input encoding (VtScreen::sendKey, sendChar, paste) and the
// "terminal keeps these keys" table (frost/term/keys.hpp).

#include "frost/term/keys.hpp"
#include "frost/term/vt_screen.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using frost::term::KeyMods;
using frost::term::PtySize;
using frost::term::terminalKeepsChar;
using frost::term::terminalKeepsKey;
using frost::term::TermKey;
using frost::term::VtScreen;

constexpr KeyMods kShift{true, false, false};
constexpr KeyMods kCtrl{false, true, false};
constexpr KeyMods kAlt{false, false, true};
constexpr KeyMods kCtrlShift{true, true, false};

std::string key(VtScreen& screen, TermKey k, KeyMods mods = {}) {
    screen.sendKey(k, mods);
    return screen.takeOutput();
}

std::string character(VtScreen& screen, char32_t c, KeyMods mods = {}) {
    screen.sendChar(c, mods);
    return screen.takeOutput();
}

TEST(KeyEncoding, Win32ModeTracksFragmentedRequestsAndReset) {
    VtScreen screen(PtySize{80, 24}, 0);
    const frost::term::Win32KeyEvent event{13, 28, 13, true, 16, 1};
    EXPECT_FALSE(screen.sendWin32Key(event));
    screen.feed("\x1b[?900");
    screen.feed("1;1004h");
    EXPECT_TRUE(screen.sendWin32Key(event));
    EXPECT_EQ(screen.takeOutput(), "\x1b[13;28;13;1;16;1_");
    screen.feed("\x1b[?9001l");
    EXPECT_FALSE(screen.sendWin32Key(event));
    screen.feed("\x1b[?9001$p\x1b[?9001h\x1b[?9001$p\x1b[?9001l\x1b[?9001$p");
    EXPECT_EQ(screen.takeOutput(), "\x1b[?9001;2$y\x1b[?9001;1$y\x1b[?9001;2$y");
    screen.feed("\x1b]0;not a mode: \x1b[?9001h\x07");
    EXPECT_FALSE(screen.sendWin32Key(event));
    screen.feed("\x1b[?9001h\x1b"
                "c");
    EXPECT_FALSE(screen.sendWin32Key(event));
    screen.feed("\x1b[?9001h\x1b[!p");
    EXPECT_FALSE(screen.sendWin32Key(event));
}

TEST(KeyEncoding, KittyNegotiationAndShiftEnter) {
    VtScreen screen(PtySize{80, 24}, 0);
    screen.feed("\x1b[?u\x1b[>5");
    EXPECT_EQ(screen.takeOutput(), "\x1b[?0u");
    screen.feed("u\x1b[?u");
    EXPECT_EQ(screen.takeOutput(), "\x1b[?5u");
    EXPECT_EQ(key(screen, TermKey::Enter, kShift), "\x1b[13;2u");
    EXPECT_EQ(character(screen, U'a'), "a");
    screen.feed("\x1b[?1049h\x1b[?u");
    EXPECT_EQ(screen.takeOutput(), "\x1b[?0u");
    EXPECT_EQ(key(screen, TermKey::Enter, kShift), "\r");
    screen.feed("\x1b[?1049l\x1b[?u");
    EXPECT_EQ(screen.takeOutput(), "\x1b[?5u");
    screen.feed("\x1b[<u\x1b[?u");
    EXPECT_EQ(screen.takeOutput(), "\x1b[?0u");
    EXPECT_EQ(key(screen, TermKey::Enter, kShift), "\r");
    screen.feed("\x1b[=5u\x1b[?u\x1b[=4;3u\x1b[?u");
    EXPECT_EQ(screen.takeOutput(), "\x1b[?5u\x1b[?1u");
    EXPECT_EQ(character(screen, U'c', kCtrl), "\x1b[99;5u");
    screen.feed("\x1b[>5u\x1b[?u");
    EXPECT_EQ(screen.takeOutput(), "\x1b[?5u");
    EXPECT_EQ(key(screen, TermKey::Enter, kShift), "\x1b[13;2u");
    EXPECT_EQ(character(screen, U'A', KeyMods{true, false, true}), "\x1b[97:65;4u");
    screen.feed("\x1b[?7u\x1b[?u\x1b[?9002$p"); // A reply, then two queries.
    EXPECT_EQ(screen.takeOutput(), "\x1b[?5u\x1b[?9002;0$y");
    screen.feed("\x1b[>31u\x1b[?u");
    EXPECT_EQ(screen.takeOutput(), "\x1b[?5u");
}

TEST(KeyEncoding, CursorKeysFollowTheApplicationCursorMode) {
    VtScreen screen(PtySize{80, 24}, 0);
    EXPECT_EQ(key(screen, TermKey::Up), "\x1b[A");
    EXPECT_EQ(key(screen, TermKey::Down), "\x1b[B");
    EXPECT_EQ(key(screen, TermKey::Right), "\x1b[C");
    EXPECT_EQ(key(screen, TermKey::Left), "\x1b[D");
    EXPECT_EQ(key(screen, TermKey::Home), "\x1b[H");
    EXPECT_EQ(key(screen, TermKey::End), "\x1b[F");
    screen.feed("\x1b[?1h"); // DECCKM, as vim and less set it.
    EXPECT_EQ(key(screen, TermKey::Up), "\x1bOA");
    EXPECT_EQ(key(screen, TermKey::Home), "\x1bOH");
    EXPECT_EQ(key(screen, TermKey::Right, kCtrl), "\x1b[1;5C");
}

TEST(KeyEncoding, ModifiedNavigationKeys) {
    VtScreen screen(PtySize{80, 24}, 0);
    EXPECT_EQ(key(screen, TermKey::Right, kCtrl), "\x1b[1;5C");
    EXPECT_EQ(key(screen, TermKey::Left, kAlt), "\x1b[1;3D");
    EXPECT_EQ(key(screen, TermKey::Up, kShift), "\x1b[1;2A");
    EXPECT_EQ(key(screen, TermKey::Delete), "\x1b[3~");
    EXPECT_EQ(key(screen, TermKey::Insert), "\x1b[2~");
    EXPECT_EQ(key(screen, TermKey::PageUp), "\x1b[5~");
    EXPECT_EQ(key(screen, TermKey::PageDown, kCtrl), "\x1b[6;5~");
}

TEST(KeyEncoding, FunctionKeys) {
    VtScreen screen(PtySize{80, 24}, 0);
    EXPECT_EQ(key(screen, TermKey::F1), "\x1bOP");
    EXPECT_EQ(key(screen, TermKey::F4), "\x1bOS");
    EXPECT_EQ(key(screen, TermKey::F5), "\x1b[15~");
    EXPECT_EQ(key(screen, TermKey::F10), "\x1b[21~");
    EXPECT_EQ(key(screen, TermKey::F12), "\x1b[24~");
    EXPECT_EQ(key(screen, TermKey::F1, kShift), "\x1b[1;2P");
    EXPECT_EQ(key(screen, TermKey::F5, kCtrl), "\x1b[15;5~");
}

TEST(KeyEncoding, EditingKeysUseTheClassicCodes) {
    VtScreen screen(PtySize{80, 24}, 0);
    EXPECT_EQ(key(screen, TermKey::Enter), "\r");
    EXPECT_EQ(key(screen, TermKey::Enter, kShift), "\r");
    EXPECT_EQ(key(screen, TermKey::Enter, kAlt), "\x1b\r");
    EXPECT_EQ(key(screen, TermKey::Tab), "\t");
    EXPECT_EQ(key(screen, TermKey::Tab, kShift), "\x1b[Z");
    EXPECT_EQ(key(screen, TermKey::Backspace), "\x7f");
    EXPECT_EQ(key(screen, TermKey::Backspace, kCtrl), "\x08");
    EXPECT_EQ(key(screen, TermKey::Backspace, kAlt), "\x1b\x7f");
    EXPECT_EQ(key(screen, TermKey::Escape), "\x1b");
}

TEST(KeyEncoding, ControlAndAltCharacters) {
    VtScreen screen(PtySize{80, 24}, 0);
    EXPECT_EQ(character(screen, U'c', kCtrl), "\x03");
    EXPECT_EQ(character(screen, U'C', kCtrl), "\x03");
    EXPECT_EQ(character(screen, U'd', kCtrl), "\x04");
    EXPECT_EQ(character(screen, U'i', kCtrl), "\t");
    EXPECT_EQ(character(screen, U'm', kCtrl), "\r");
    EXPECT_EQ(character(screen, U'[', kCtrl), "\x1b");
    EXPECT_EQ(character(screen, U'/', kCtrl), "\x1f");
    EXPECT_EQ(character(screen, U' ', kCtrl), std::string(1, '\0'));
    EXPECT_EQ(character(screen, U'b', kAlt), "\x1b"
                                             "b");
    EXPECT_EQ(character(screen, U'B', KeyMods{true, false, true}), "\x1b"
                                                                   "B");
    EXPECT_EQ(character(screen, U'r', KeyMods{false, true, true}), "\x1b\x12");
}

TEST(KeyEncoding, TextIsUtf8) {
    VtScreen screen(PtySize{80, 24}, 0);
    EXPECT_EQ(character(screen, U'a'), "a");
    EXPECT_EQ(character(screen, U'é'), "\xc3\xa9");
    EXPECT_EQ(character(screen, U'\U0001F600'), "\xf0\x9f\x98\x80");
    screen.sendText("\xe4\xb8\xad\xe6\x96\x87"); // An IME commit.
    EXPECT_EQ(screen.takeOutput(), "\xe4\xb8\xad\xe6\x96\x87");
}

TEST(KeyEncoding, PasteIsBracketedWhenTheProgramAsks) {
    VtScreen screen(PtySize{80, 24}, 0);
    screen.paste("echo a\r\necho b\n");
    EXPECT_EQ(screen.takeOutput(), "echo a\recho b\r");
    screen.feed("\x1b[?2004h");
    screen.paste("ls\nrm -rf x");
    EXPECT_EQ(screen.takeOutput(), "\x1b[200~ls\rrm -rf x\x1b[201~");
    // A clipboard cannot end the bracket early.
    screen.paste("a\x1b[201~b");
    EXPECT_EQ(screen.takeOutput(), "\x1b[200~ab\x1b[201~");
}

TEST(TerminalKeepsKeys, NavigationAndEditingKeys) {
    for (TermKey k : {TermKey::Escape, TermKey::Tab, TermKey::Enter, TermKey::Backspace, TermKey::Up, TermKey::Down,
                      TermKey::Left, TermKey::Right, TermKey::Home, TermKey::End, TermKey::PageUp, TermKey::PageDown,
                      TermKey::Insert, TermKey::Delete}) {
        EXPECT_TRUE(terminalKeepsKey(k, {})) << static_cast<int>(k);
    }
    EXPECT_TRUE(terminalKeepsKey(TermKey::Left, kCtrl));
    EXPECT_TRUE(terminalKeepsKey(TermKey::Backspace, kCtrl));
    EXPECT_TRUE(terminalKeepsKey(TermKey::Tab, kShift));
    // The IDE's: hide the tool window, the switcher.
    EXPECT_FALSE(terminalKeepsKey(TermKey::Escape, kShift));
    EXPECT_FALSE(terminalKeepsKey(TermKey::Tab, kCtrl));
    EXPECT_FALSE(terminalKeepsKey(TermKey::Tab, kCtrlShift));
}

TEST(TerminalKeepsKeys, FunctionKeys) {
    EXPECT_TRUE(terminalKeepsKey(TermKey::F1, {}));
    EXPECT_TRUE(terminalKeepsKey(TermKey::F10, {}));
    EXPECT_FALSE(terminalKeepsKey(TermKey::F11, {}));
    EXPECT_FALSE(terminalKeepsKey(TermKey::F12, {}));
    EXPECT_FALSE(terminalKeepsKey(TermKey::F5, kShift));
    EXPECT_FALSE(terminalKeepsKey(TermKey::F12, kAlt)); // Alt+F12 toggles the terminal itself.
}

TEST(TerminalKeepsKeys, CharacterKeys) {
    EXPECT_TRUE(terminalKeepsChar(U'c', kCtrl));
    EXPECT_TRUE(terminalKeepsChar(U'r', kCtrl));
    EXPECT_TRUE(terminalKeepsChar(U'e', kCtrl)); // The IDE's Recent Files.
    EXPECT_TRUE(terminalKeepsChar(U'[', kCtrl));
    EXPECT_TRUE(terminalKeepsChar(U'b', kAlt));
    EXPECT_TRUE(terminalKeepsChar(U'c', kCtrlShift)); // Copy.
    EXPECT_TRUE(terminalKeepsChar(U'v', kCtrlShift)); // Paste.
    EXPECT_TRUE(terminalKeepsChar(U'x', {}));
    EXPECT_TRUE(terminalKeepsChar(U'x', kShift));
    EXPECT_FALSE(terminalKeepsChar(U'1', kAlt));       // Tool windows.
    EXPECT_FALSE(terminalKeepsChar(U'a', kCtrlShift)); // Find Action.
    EXPECT_FALSE(terminalKeepsChar(U'f', kCtrlShift)); // Find in Files.
    EXPECT_FALSE(terminalKeepsChar(U's', KeyMods{false, true, true}));
    EXPECT_FALSE(terminalKeepsChar(U'1', kCtrl));
}

} // namespace
