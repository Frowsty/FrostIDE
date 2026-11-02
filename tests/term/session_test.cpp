// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for frost::term::TerminalSession over the FakePtyLauncher: output reaches the screen,
// input reaches the terminal, query answers go back to the child, the observer's notifications, the
// exit status, resize and close.

#include "frost/term/fake_pty.hpp"
#include "frost/term/session.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using frost::term::FakePty;
using frost::term::FakePtyLauncher;
using frost::term::KeyMods;
using frost::term::PtySize;
using frost::term::SessionObserver;
using frost::term::TerminalSession;
using frost::term::TermKey;
using frost::term::VtScreen;

constexpr auto kTimeout = 10s;

std::unique_ptr<TerminalSession> startSession(FakePtyLauncher& launcher, SessionObserver observer = {}) {
    auto session = TerminalSession::start(launcher, frost::term::ptyProcessSpec("/bin/fake"), PtySize{40, 10}, 100,
                                          std::move(observer));
    EXPECT_TRUE(session.has_value());
    return session ? std::move(*session) : nullptr;
}

TEST(TerminalSession, OutputReachesTheScreenAndInputTheTerminal) {
    FakePtyLauncher launcher;
    auto session = startSession(launcher);
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(launcher.count(), 1U);
    const auto pty = launcher.pty(0);
    EXPECT_EQ(launcher.specs()[0].executable, "/bin/fake");
    EXPECT_EQ(pty->size(), (PtySize{40, 10}));

    pty->print("$ prompt");
    EXPECT_TRUE(session->waitForText("$ prompt", kTimeout));
    EXPECT_EQ(session->read([](const VtScreen& screen) { return screen.cursor().column; }), 8);

    session->sendChar(U'l', {});
    session->sendChar(U's', {});
    session->sendKey(TermKey::Enter, {});
    session->sendChar(U'c', KeyMods{false, true, false});
    EXPECT_TRUE(pty->waitForInput("ls\r\x03", kTimeout));
}

TEST(TerminalSession, QueryAnswersGoBackToTheChild) {
    FakePtyLauncher launcher;
    auto session = startSession(launcher);
    ASSERT_NE(session, nullptr);
    launcher.pty(0)->print("\x1b[2;3H\x1b[6n");
    EXPECT_TRUE(launcher.pty(0)->waitForInput("\x1b[2;3R", kTimeout));
}

TEST(TerminalSession, ObserverSeesChangesTitlesBellsAndTheExit) {
    FakePtyLauncher launcher;
    std::atomic<int> changes{0};
    std::atomic<int> bells{0};
    std::atomic<int> exitCode{-1};
    std::string title;
    std::atomic<bool> titled{false};
    SessionObserver observer;
    observer.changed = [&] { ++changes; };
    observer.bell = [&] { ++bells; };
    observer.titleChanged = [&](const std::string& value) {
        title = value;
        titled = true;
    };
    observer.exited = [&](frost::io::ExitStatus status) { exitCode = status.exitCode; };
    auto session = startSession(launcher, std::move(observer));
    ASSERT_NE(session, nullptr);
    const auto pty = launcher.pty(0);

    pty->print("one\x1b]0;build\x07\a");
    ASSERT_TRUE(session->waitForText("one", kTimeout));
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while ((!titled || bells == 0) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(titled);
    EXPECT_EQ(title, "build");
    EXPECT_EQ(bells, 1);
    EXPECT_EQ(changes, 1);
    // Coalesced until acknowledged.
    pty->print("two");
    ASSERT_TRUE(session->waitForText("two", kTimeout));
    EXPECT_EQ(changes, 1);
    session->acknowledge();
    pty->print("three");
    ASSERT_TRUE(session->waitForText("three", kTimeout));
    while (changes < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(changes, 2);

    EXPECT_TRUE(session->running());
    pty->exit(3);
    while (exitCode == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(exitCode, 3);
    EXPECT_FALSE(session->running());
    EXPECT_EQ(session->exitStatus()->exitCode, 3);
}

TEST(TerminalSession, ResizeReachesTheScreenAndTheTerminal) {
    FakePtyLauncher launcher;
    auto session = startSession(launcher);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->resize(PtySize{100, 30}).has_value());
    EXPECT_EQ(launcher.pty(0)->size(), (PtySize{100, 30}));
    EXPECT_EQ(launcher.pty(0)->resizeCount(), 1);
    EXPECT_EQ(session->read([](const VtScreen& screen) { return screen.columns(); }), 100);
}

TEST(TerminalSession, CloseEndsTheTerminal) {
    FakePtyLauncher launcher;
    auto session = startSession(launcher);
    ASSERT_NE(session, nullptr);
    const auto pty = launcher.pty(0);
    session->close();
    EXPECT_TRUE(pty->closed());
    EXPECT_EQ(session->exitStatus()->exitCode, frost::io::kTerminatedExitCode);
    session->close();            // Idempotent.
    session->sendChar(U'x', {}); // Dropped, not crashed.
    EXPECT_EQ(pty->input(), "");
}

TEST(TerminalSession, StartFailsWhenTheLauncherDoes) {
    FakePtyLauncher launcher;
    launcher.failNext();
    auto session = TerminalSession::start(launcher, frost::term::ptyProcessSpec("/bin/fake"), PtySize{40, 10}, 100, {});
    EXPECT_FALSE(session.has_value());
}

} // namespace
