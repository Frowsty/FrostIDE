// SPDX-License-Identifier: Apache-2.0
//
// Integration tests of frost::term's real pseudo terminals (label "pty"): a TerminalSession over
// RealPtyLauncher runs cmd.exe /d /q on the system ConPTY (Windows) or /bin/sh (Linux). The shell
// echoes text that is read back from the screen model, sees a resize, is interrupted with Ctrl+C,
// and exits; closing the session ends a background grandchild too.

#include "frost/term/pty.hpp"
#include "frost/term/session.hpp"
#include "frost/term/shell.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>

#include <tlhelp32.h>
#else
#include <fstream>
#include <iterator>
#include <regex>
#endif

namespace {

using namespace std::chrono_literals;
using frost::term::KeyMods;
using frost::term::PtySize;
using frost::term::RealPtyLauncher;
using frost::term::TerminalSession;
using frost::term::VtScreen;

constexpr auto kTimeout = 20s;
constexpr KeyMods kCtrl{false, true, false};

frost::io::ProcessSpec shellSpec() {
#ifdef _WIN32
    const auto probe = frost::term::systemShellProbe();
    auto spec = frost::term::ptyProcessSpec(probe.systemDirectory + "\\cmd.exe", {"/d", "/q"});
#else
    auto spec = frost::term::ptyProcessSpec("/bin/sh");
    spec.environment.emplace_back("PS1", "$ ");
#endif
    spec.environment.emplace_back("TERM", "xterm-256color");
    return spec;
}

std::unique_ptr<TerminalSession> startShell(RealPtyLauncher& launcher) {
    auto session = TerminalSession::start(launcher, shellSpec(), PtySize{80, 24}, 1000, {});
    EXPECT_TRUE(session.has_value()) << (session ? "" : session.error().message());
    return session ? std::move(*session) : nullptr;
}

#ifdef _WIN32
// Check that the bundled DLL started the OpenConsole next to this test executable.
bool bundledHostRunning(const std::wstring& directory) {
    const std::wstring expected = directory + L"\\OpenConsole.exe";
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
                if (_wcsicmp(entry.szExeFile, L"OpenConsole.exe") != 0)
                    continue;
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                if (process == nullptr)
                    continue;
                std::array<wchar_t, 32768> path{};
                DWORD length = static_cast<DWORD>(path.size());
                const bool matches = QueryFullProcessImageNameW(process, 0, path.data(), &length) &&
                                     _wcsicmp(path.data(), expected.c_str()) == 0;
                CloseHandle(process);
                if (matches) {
                    CloseHandle(snapshot);
                    return true;
                }
            }
            CloseHandle(snapshot);
        }
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

// The first visible or scrollback line containing `needle`, or "".
std::string lineWith(const TerminalSession& session, std::string_view needle) {
    return session.read([&](const VtScreen& screen) {
        for (int row = -static_cast<int>(screen.scrollbackSize()); row < screen.rows(); ++row) {
            std::string text = screen.lineText(row);
            if (text.find(needle) != std::string::npos) {
                return text;
            }
        }
        return std::string();
    });
}
#endif

// Everything on the screen and in the scrollback.
std::string allText(const TerminalSession& session) {
    return session.read([](const VtScreen& screen) {
        return screen.text(frost::term::ScreenPos{-static_cast<int>(screen.scrollbackSize()), 0},
                           frost::term::ScreenPos{screen.rows() - 1, screen.columns() - 1});
    });
}

#ifdef _WIN32
// The pid of a process named `image` in the shell's Job Object, waiting up to `timeout` for one.
std::optional<std::uint32_t> waitForContained(TerminalSession& session, const char* image,
                                              std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto processes = session.containedProcesses()) {
            for (const auto& process : *processes) {
                if (_stricmp(process.imageName.c_str(), image) == 0) {
                    return static_cast<std::uint32_t>(process.pid);
                }
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    return std::nullopt;
}

// Whether the screen and scrollback hold `needle` at least `count` times within `timeout`.
bool waitForCount(const TerminalSession& session, std::string_view needle, std::size_t count,
                  std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const std::string text = allText(session);
        std::size_t found = 0;
        for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
            ++found;
        }
        if (found >= count) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
}
#endif

bool waitForExit(const TerminalSession& session, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (session.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    return !session.running();
}

// Whether the process `pid` has gone (or is only a zombie) within `timeout`.
bool processGone(std::uint32_t pid, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        return GetLastError() == ERROR_INVALID_PARAMETER; // No such process.
    }
    const DWORD result = WaitForSingleObject(process, static_cast<DWORD>(timeout.count()));
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        if (!stat) {
            return true;
        }
        std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
        const auto close = content.rfind(')');
        if (close != std::string::npos && close + 2 < content.size() && content[close + 2] == 'Z') {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
#endif
}

#ifdef _WIN32
// Explicitly invoked local spike. No input other than VT query replies is sent.
TEST(RealPty, DISABLED_ClaudeStartupProbe) {
    const auto probe = frost::term::systemShellProbe();
    const auto executable = probe.environment("FROST_CLAUDE_PROBE_EXE");
    const auto config = probe.environment("FROST_CLAUDE_PROBE_CONFIG");
    ASSERT_TRUE(executable.has_value());
    ASSERT_TRUE(config.has_value());
    auto spec = frost::term::ptyProcessSpec(*executable);
    spec.environment.emplace_back("CLAUDE_CONFIG_DIR", *config);
    spec.environment.emplace_back("CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", "1");
    spec.environment.emplace_back("DISABLE_AUTOUPDATER", "1");
    spec.environment.emplace_back("TERM", "xterm-256color");
    spec.environment.emplace_back("TERM_PROGRAM", "FrostIDE");
    RealPtyLauncher launcher;
    auto opened = launcher.open(spec, PtySize{100, 30});
    ASSERT_TRUE(opened.has_value()) << (opened ? "" : opened.error().message());
    auto pty = std::move(*opened);
    VtScreen screen(PtySize{100, 30}, 0);
    std::string raw;
    std::thread reader([&] {
        std::array<char, 4096> bytes{};
        for (;;) {
            auto count = pty->read(bytes);
            if (!count || *count == 0)
                break;
            const std::string_view chunk(bytes.data(), *count);
            raw.append(chunk);
            screen.feed(chunk);
            const auto reply = screen.takeOutput();
            if (!reply.empty())
                static_cast<void>(pty->write(reply));
        }
    });
    std::this_thread::sleep_for(10s);
    pty->close();
    reader.join();
    std::cout << "CLAUDE-SCREEN-BEGIN\n" << screen.screenText() << "\nCLAUDE-SCREEN-END\n";
    for (std::size_t i = 0; i + 2 < raw.size(); ++i) {
        if (raw[i] != '\x1b' || raw[i + 1] != '[')
            continue;
        std::size_t end = i + 2;
        while (end < raw.size() &&
               (static_cast<unsigned char>(raw[end]) < 0x40 || static_cast<unsigned char>(raw[end]) > 0x7e))
            ++end;
        if (end < raw.size() && std::string_view("pcqnu").find(raw[end]) != std::string_view::npos) {
            std::cout << "CLAUDE-CSI: " << raw.substr(i + 2, end - i - 1) << "\n";
        }
    }
}

TEST(RealPty, Win32InputCarriesShiftEnterToConsoleInputRecords) {
    RealPtyLauncher launcher;
    auto spec = frost::term::ptyProcessSpec(FROST_TERM_KEY_CHILD);
    auto started = TerminalSession::start(launcher, spec, PtySize{80, 24}, 100, {});
    ASSERT_TRUE(started.has_value());
    auto session = std::move(*started);
    ASSERT_TRUE(session->waitForText("READY", kTimeout));
    // The integration executable is deployed with the pinned SDK DLL.
    const HMODULE bundled = GetModuleHandleW(L"conpty.dll");
    ASSERT_NE(bundled, nullptr);
    std::array<wchar_t, 32768> modulePath{};
    std::array<wchar_t, 32768> exePath{};
    const DWORD moduleLength = GetModuleFileNameW(bundled, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    const DWORD exeLength = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    ASSERT_GT(moduleLength, 0U);
    ASSERT_GT(exeLength, 0U);
    const std::wstring module(modulePath.data(), moduleLength);
    const std::wstring exe(exePath.data(), exeLength);
    const std::wstring directory = exe.substr(0, exe.find_last_of(L"\\/"));
    EXPECT_EQ(module.substr(0, module.find_last_of(L"\\/")), directory);
    EXPECT_TRUE(bundledHostRunning(directory));
    ASSERT_TRUE(session->sendWin32Key({65, 30, 0xd83d, true, 0, 1}));
    ASSERT_TRUE(session->sendWin32Key({65, 30, 0xde00, true, 0, 1}));
    ASSERT_TRUE(session->sendWin32Key({VK_RETURN, 28, 13, true, SHIFT_PRESSED, 1}));
    ASSERT_TRUE(session->waitForText("SURROGATE-1 ENTER-SHIFT-1", kTimeout)) << allText(*session);
}
#endif

#ifndef _WIN32
TEST(RealPty, KittyModeCarriesShiftEnterOverThePty) {
    RealPtyLauncher launcher;
    auto spec = frost::term::ptyProcessSpec(FROST_TERM_KEY_CHILD);
    auto started = TerminalSession::start(launcher, spec, PtySize{80, 24}, 100, {});
    ASSERT_TRUE(started.has_value());
    auto session = std::move(*started);
    ASSERT_TRUE(session->waitForText("READY", kTimeout));
    session->sendKey(frost::term::TermKey::Enter, KeyMods{true, false, false});
    ASSERT_TRUE(session->waitForText("KEY-SHIFT-ENTER", kTimeout)) << allText(*session);
}
#endif

TEST(RealPty, EchoResizeAndCtrlC) {
    RealPtyLauncher launcher;
    auto session = startShell(launcher);
    ASSERT_NE(session, nullptr);

    // The expansion proves the shell ran the command (the typed text never contains its result).
#ifdef _WIN32
    session->sendText("echo frost-%OS%\r");
    ASSERT_TRUE(session->waitForText("frost-Windows_NT", kTimeout)) << allText(*session);
#else
    session->sendText("echo frost-$((40+2))\r");
    ASSERT_TRUE(session->waitForText("frost-42", kTimeout)) << allText(*session);
#endif

    ASSERT_TRUE(session->resize(PtySize{100, 30}).has_value());
#ifdef _WIN32
    /*
     * ConPTY wraps what it renders at its own width, so an 81-column line (COMSPEC three times; the
     * typed command never contains it) arriving on one row proves the console is 100 columns wide.
     * Locale-independent, unlike `mode con`.
     */
    const std::string comspec = frost::term::systemShellProbe().environment("COMSPEC").value_or("");
    ASSERT_FALSE(comspec.empty());
    const std::size_t copies = (80 / comspec.size()) + 1;
    std::string command = "echo ";
    std::string expected;
    for (std::size_t i = 0; i < copies; ++i) {
        command += "%COMSPEC%";
        expected += comspec;
    }
    ASSERT_LE(expected.size(), 100U);
    session->sendText(command + "\r");
    ASSERT_TRUE(session->waitForText(expected, kTimeout)) << allText(*session);
    EXPECT_EQ(lineWith(*session, expected), expected);
#else
    session->sendText("stty size\r");
    ASSERT_TRUE(session->waitForText("30 100", kTimeout)) << allText(*session);
#endif

    // Ctrl+C interrupts a long command; the shell then runs the next one at once.
    const auto started = std::chrono::steady_clock::now();
#ifdef _WIN32
    session->sendText("ping -n 60 127.0.0.1\r");
    const std::optional<std::uint32_t> ping = waitForContained(*session, "ping.exe", kTimeout);
    ASSERT_TRUE(ping.has_value());
    /*
     * ping is in the Job Object before it has attached to the console, and a Ctrl+C sent then reaches
     * only cmd (seen under ctest's parallel load). Its banner and first reply (each with the address in
     * every locale; the typed command is the first occurrence) show it running. A Ctrl+C that is still
     * lost is sent again, a few times, while ping lives.
     */
    ASSERT_TRUE(waitForCount(*session, "127.0.0.1", 3, kTimeout)) << allText(*session);
    bool interrupted = false;
    for (int attempt = 0; attempt < 3 && !interrupted; ++attempt) {
        session->sendChar(U'c', kCtrl);
        interrupted = processGone(*ping, 3s);
    }
    ASSERT_TRUE(interrupted) << allText(*session);
    session->sendText("echo after-%OS%\r");
    ASSERT_TRUE(session->waitForText("after-Windows_NT", kTimeout)) << allText(*session);
#else
    session->sendText("sleep 60\r");
    std::this_thread::sleep_for(500ms);
    session->sendChar(U'c', kCtrl);
    session->sendText("echo after-$((1+1))\r");
    ASSERT_TRUE(session->waitForText("after-2", kTimeout)) << allText(*session);
#endif
    EXPECT_LT(std::chrono::steady_clock::now() - started, 50s);

    // An explicit code: a bare `exit` would return the interrupted command's status.
    session->sendText("exit 3\r");
    ASSERT_TRUE(waitForExit(*session, kTimeout));
    EXPECT_EQ(session->exitStatus()->exitCode, 3);
    session->close();
}

TEST(RealPty, CloseEndsTheProcessTree) {
    RealPtyLauncher launcher;
    auto session = startShell(launcher);
    ASSERT_NE(session, nullptr);
    std::optional<std::uint32_t> grandchild;
#ifdef _WIN32
    session->sendText("start /b ping -n 120 127.0.0.1 >nul\r");
    grandchild = waitForContained(*session, "ping.exe", kTimeout);
#else
    // A background job has its own process group; the session kill still reaches it.
    session->sendText("sleep 120 & echo bg-$((7*6))-$!\r");
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    const std::regex pidPattern("bg-42-([0-9]+)");
    while (!grandchild && std::chrono::steady_clock::now() < deadline) {
        std::smatch match;
        const std::string text = allText(*session);
        if (std::regex_search(text, match, pidPattern)) {
            grandchild = static_cast<std::uint32_t>(std::stoul(match[1].str()));
        }
        std::this_thread::sleep_for(20ms);
    }
#endif
    ASSERT_TRUE(grandchild.has_value());
    EXPECT_FALSE(processGone(*grandchild, 0ms));
    session->close();
    EXPECT_TRUE(processGone(*grandchild, 10s)) << *grandchild;
}

TEST(RealPty, LauncherRejectsAZeroSize) {
    RealPtyLauncher launcher;
    auto pty = launcher.open(shellSpec(), PtySize{0, 24});
    ASSERT_FALSE(pty.has_value());
    EXPECT_EQ(pty.error().code(), frost::ErrorCode::InvalidArgument);
}

} // namespace
