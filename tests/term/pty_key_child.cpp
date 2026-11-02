// SPDX-License-Identifier: Apache-2.0
// Child for the real ConPTY keyboard test: report the first Enter key record.
#ifdef _WIN32
#include <cstdio>

#include <windows.h>
int main() {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode) || !SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return 2;
    DWORD written = 0;
    constexpr char request[] = "\x1b[?9001hREADY\r\n";
    if (!WriteFile(out, request, sizeof(request) - 1, &written, nullptr))
        return 3;
    bool highSurrogate = false;
    bool pairSeen = false;
    for (;;) {
        INPUT_RECORD record{};
        DWORD read = 0;
        if (!ReadConsoleInputW(in, &record, 1, &read))
            return 4;
        if (read && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
            const auto unit = record.Event.KeyEvent.uChar.UnicodeChar;
            if (highSurrogate && unit == 0xde00)
                pairSeen = true;
            highSurrogate = unit == 0xd83d;
        }
        if (read && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
            record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) {
            std::printf("SURROGATE-%u ENTER-SHIFT-%u\n", pairSeen ? 1U : 0U,
                        (record.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED) != 0 ? 1U : 0U);
            std::fflush(stdout);
            return 0;
        }
    }
}
#else
#include <cstdio>
#include <string>

#include <termios.h>
#include <unistd.h>
int main() {
    termios settings{};
    if (tcgetattr(STDIN_FILENO, &settings) != 0)
        return 2;
    cfmakeraw(&settings);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0)
        return 3;
    std::printf("\x1b[>5uREADY\r\n");
    std::fflush(stdout);
    std::string input;
    for (;;) {
        char byte = 0;
        if (read(STDIN_FILENO, &byte, 1) != 1)
            return 4;
        input += byte;
        if (byte == 'u') {
            std::printf("\r\nKEY-%s\r\n", input == "\x1b[13;2u" ? "SHIFT-ENTER" : "OTHER");
            std::fflush(stdout);
            return 0;
        }
    }
}
#endif
