// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the terminal's shell choice and trust rule (frost/term/shell.hpp) over a scripted
// ShellProbe: the default shell order, never a shell inside the project root, the untrusted root's
// application shell with the home directory and NoDefaultCurrentDirectoryInExePath=1, the trusted
// root's working directory, the environment, and the settings.

#include "frost/settings/keys.hpp"
#include "frost/settings/registry.hpp"
#include "frost/term/shell.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace {

using frost::ErrorCode;
using frost::term::defaultShell;
using frost::term::isInsideRoot;
using frost::term::planTerminalLaunch;
using frost::term::ShellProbe;
using frost::term::TerminalLaunchRequest;

struct FakeSystem {
    std::map<std::string, std::string> env;
    std::set<std::string> files;
    std::string system;

    [[nodiscard]] ShellProbe probe() const {
        ShellProbe p;
        p.environment = [this](std::string_view name) -> std::optional<std::string> {
            const auto it = env.find(std::string(name));
            return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
        };
        p.isExecutable = [this](const std::string& path) { return files.contains(path); };
        p.systemDirectory = system;
        return p;
    }
};

// The value `name` gets in `spec`'s environment: nullopt when absent, an empty optional inside when
// it is removed.
std::optional<std::optional<std::string>> envOf(const frost::io::ProcessSpec& spec, std::string_view name) {
    std::optional<std::optional<std::string>> out;
    for (const auto& [key, value] : spec.environment) {
        if (key == name) {
            out = value;
        }
    }
    return out;
}

#ifdef _WIN32

constexpr const char* kSystem = "C:\\Windows\\System32";
constexpr const char* kPwsh = "C:\\Program Files\\PowerShell\\7\\pwsh.exe";
constexpr const char* kWindowsPowerShell = "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
constexpr const char* kCmd = "C:\\Windows\\System32\\cmd.exe";
constexpr const char* kRoot = "D:\\work\\project";
constexpr const char* kHome = "C:\\Users\\dev";

FakeSystem windowsSystem() {
    FakeSystem system;
    system.system = kSystem;
    system.env = {{"ProgramFiles", "C:\\Program Files"}, {"USERPROFILE", kHome}, {"PATH", "C:\\Windows\\System32"}};
    system.files = {kPwsh, kWindowsPowerShell, kCmd};
    return system;
}

TEST(TerminalShell, DefaultPrefersPowerShell7ThenWindowsPowerShellThenCmd) {
    FakeSystem system = windowsSystem();
    EXPECT_EQ(defaultShell(system.probe(), "")->executable, kPwsh);
    system.files.erase(kPwsh);
    EXPECT_EQ(defaultShell(system.probe(), "")->executable, kWindowsPowerShell);
    system.files.erase(kWindowsPowerShell);
    EXPECT_EQ(defaultShell(system.probe(), "")->executable, kCmd);
    system.files.erase(kCmd);
    EXPECT_EQ(defaultShell(system.probe(), "").error().code(), ErrorCode::NotFound);
}

TEST(TerminalShell, Pwsh7IsFoundOnAbsolutePathEntriesOutsideTheRoot) {
    FakeSystem system = windowsSystem();
    system.files.erase(kPwsh);
    system.env["PATH"] = std::string(".;relative\\bin;") + kRoot + "\\tools;\"C:\\scoop\\shims\"";
    system.files.insert(std::string(kRoot) + "\\tools\\pwsh.exe");
    system.files.insert("relative\\bin\\pwsh.exe");
    system.files.insert("C:\\scoop\\shims\\pwsh.exe");
    EXPECT_EQ(defaultShell(system.probe(), kRoot)->executable, "C:\\scoop\\shims\\pwsh.exe");
    system.files.erase("C:\\scoop\\shims\\pwsh.exe");
    EXPECT_EQ(defaultShell(system.probe(), kRoot)->executable, kWindowsPowerShell);
}

TEST(TerminalShell, UntrustedRootRunsTheApplicationShellInTheHomeDirectory) {
    const FakeSystem system = windowsSystem();
    TerminalLaunchRequest request;
    request.projectRoot = kRoot;
    request.trusted = false;
    request.workingDirectory = std::string(kRoot) + "\\src"; // "Open in Terminal" does not override it.
    auto launch = planTerminalLaunch(request, system.probe());
    ASSERT_TRUE(launch.has_value()) << launch.error().message();
    EXPECT_TRUE(launch->restricted);
    EXPECT_EQ(launch->spec.executable, kPwsh);
    EXPECT_EQ(launch->spec.workingDirectory, kHome);
    EXPECT_EQ(envOf(launch->spec, "NoDefaultCurrentDirectoryInExePath"), std::optional<std::string>("1"));
}

TEST(TerminalShell, TrustedRootStartsInTheRootWithTheUsualSearch) {
    const FakeSystem system = windowsSystem();
    TerminalLaunchRequest request;
    request.projectRoot = kRoot;
    request.trusted = true;
    auto launch = planTerminalLaunch(request, system.probe());
    ASSERT_TRUE(launch.has_value()) << launch.error().message();
    EXPECT_FALSE(launch->restricted);
    EXPECT_EQ(launch->spec.workingDirectory, kRoot);
    // Removed (an entry without a value), not left at the parent's value.
    const auto removed = envOf(launch->spec, "NoDefaultCurrentDirectoryInExePath");
    ASSERT_TRUE(removed.has_value());
    EXPECT_FALSE(removed->has_value());
    request.workingDirectory = std::string(kRoot) + "\\src";
    EXPECT_EQ(planTerminalLaunch(request, system.probe())->spec.workingDirectory, request.workingDirectory);
}

TEST(TerminalShell, ConfiguredShellMustBeAbsolute) {
    const FakeSystem system = windowsSystem();
    TerminalLaunchRequest request;
    request.configuredShell = "C:\\msys64\\usr\\bin\\bash.exe";
    EXPECT_EQ(planTerminalLaunch(request, system.probe())->spec.executable, request.configuredShell);
    request.configuredShell = "bash.exe";
    EXPECT_EQ(planTerminalLaunch(request, system.probe()).error().code(), ErrorCode::InvalidArgument);
}

TEST(TerminalShell, InsideRootIgnoresCaseOnWindows) {
    EXPECT_TRUE(isInsideRoot("d:\\WORK\\project\\bin\\x.exe", kRoot));
    EXPECT_TRUE(isInsideRoot("D:/work/project", kRoot));
    EXPECT_FALSE(isInsideRoot("D:\\work\\project2\\x.exe", kRoot));
    EXPECT_FALSE(isInsideRoot("D:\\work\\x.exe", kRoot));
}

#else

constexpr const char* kRoot = "/work/project";
constexpr const char* kHome = "/home/dev";

FakeSystem linuxSystem() {
    FakeSystem system;
    system.env = {{"HOME", kHome}, {"SHELL", "/usr/bin/zsh"}};
    system.files = {"/usr/bin/zsh", "/bin/bash", "/bin/sh"};
    return system;
}

TEST(TerminalShell, DefaultIsShellThenBashThenSh) {
    FakeSystem system = linuxSystem();
    EXPECT_EQ(defaultShell(system.probe(), "")->executable, "/usr/bin/zsh");
    system.env["SHELL"] = "zsh"; // Not absolute.
    EXPECT_EQ(defaultShell(system.probe(), "")->executable, "/bin/bash");
    system.files.erase("/bin/bash");
    EXPECT_EQ(defaultShell(system.probe(), "")->executable, "/bin/sh");
    system.files.erase("/bin/sh");
    EXPECT_EQ(defaultShell(system.probe(), "").error().code(), ErrorCode::NotFound);
}

TEST(TerminalShell, ShellInsideTheRootIsNeverPicked) {
    FakeSystem system = linuxSystem();
    system.env["SHELL"] = std::string(kRoot) + "/bin/evil";
    system.files.insert(system.env["SHELL"]);
    EXPECT_EQ(defaultShell(system.probe(), kRoot)->executable, "/bin/bash");
}

TEST(TerminalShell, UntrustedRootRunsTheApplicationShellInTheHomeDirectory) {
    const FakeSystem system = linuxSystem();
    TerminalLaunchRequest request;
    request.projectRoot = kRoot;
    request.trusted = false;
    request.workingDirectory = std::string(kRoot) + "/src";
    auto launch = planTerminalLaunch(request, system.probe());
    ASSERT_TRUE(launch.has_value()) << launch.error().message();
    EXPECT_TRUE(launch->restricted);
    EXPECT_EQ(launch->spec.executable, "/usr/bin/zsh");
    EXPECT_EQ(launch->spec.workingDirectory, kHome);
}

TEST(TerminalShell, TrustedRootStartsInTheRoot) {
    const FakeSystem system = linuxSystem();
    TerminalLaunchRequest request;
    request.projectRoot = kRoot;
    request.trusted = true;
    auto launch = planTerminalLaunch(request, system.probe());
    ASSERT_TRUE(launch.has_value()) << launch.error().message();
    EXPECT_FALSE(launch->restricted);
    EXPECT_EQ(launch->spec.workingDirectory, kRoot);
}

TEST(TerminalShell, ConfiguredShellMustBeAbsolute) {
    const FakeSystem system = linuxSystem();
    TerminalLaunchRequest request;
    request.configuredShell = "/usr/bin/fish";
    EXPECT_EQ(planTerminalLaunch(request, system.probe())->spec.executable, "/usr/bin/fish");
    request.configuredShell = "fish";
    EXPECT_EQ(planTerminalLaunch(request, system.probe()).error().code(), ErrorCode::InvalidArgument);
}

#endif

TEST(TerminalShell, NoProjectStartsInTheHomeDirectory) {
#ifdef _WIN32
    const FakeSystem system = windowsSystem();
#else
    const FakeSystem system = linuxSystem();
#endif
    auto launch = planTerminalLaunch(TerminalLaunchRequest{}, system.probe());
    ASSERT_TRUE(launch.has_value()) << launch.error().message();
    EXPECT_FALSE(launch->restricted);
    EXPECT_EQ(launch->spec.workingDirectory, kHome);
}

TEST(TerminalShell, EnvironmentAdvertisesTheTerminal) {
#ifdef _WIN32
    const FakeSystem system = windowsSystem();
#else
    const FakeSystem system = linuxSystem();
#endif
    auto launch = planTerminalLaunch(TerminalLaunchRequest{}, system.probe());
    ASSERT_TRUE(launch.has_value());
    EXPECT_EQ(envOf(launch->spec, "TERM"), std::optional<std::string>("xterm-256color"));
    EXPECT_EQ(envOf(launch->spec, "COLORTERM"), std::optional<std::string>("truecolor"));
    EXPECT_EQ(envOf(launch->spec, "TERM_PROGRAM"), std::optional<std::string>("FrostIDE"));
    EXPECT_EQ(launch->spec.stdinMode, frost::io::Stdio::Inherit);
    EXPECT_TRUE(launch->spec.killOnClose);
    EXPECT_FALSE(launch->spec.pseudoTerminal.has_value());
}

TEST(TerminalShell, UntrustedRootNeedsAHomeDirectory) {
#ifdef _WIN32
    FakeSystem system = windowsSystem();
    system.env.erase("USERPROFILE");
#else
    FakeSystem system = linuxSystem();
    system.env.erase("HOME");
#endif
    TerminalLaunchRequest request;
    request.projectRoot = kRoot;
    EXPECT_EQ(planTerminalLaunch(request, system.probe()).error().code(), ErrorCode::NotFound);
}

TEST(TerminalShell, SettingsDefaults) {
    const auto& registry = frost::settings::builtinRegistry();
    const auto* shell = registry.find(frost::settings::key::terminalShell.name);
    ASSERT_NE(shell, nullptr);
    EXPECT_TRUE(shell->executes);
    const auto* scrollback = registry.find(frost::settings::key::terminalScrollbackLines.name);
    ASSERT_NE(scrollback, nullptr);
    const auto* lines = std::get_if<std::int64_t>(&scrollback->defaultValue);
    ASSERT_NE(lines, nullptr);
    EXPECT_EQ(*lines, 10000);
}

} // namespace
