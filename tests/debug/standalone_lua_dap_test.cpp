// SPDX-License-Identifier: Apache-2.0
// Real protocol/argv construction, not simulated conformance acceptance.
#include "frost/debug/standalone_lua_dap.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/rpc/framing.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <latch>
namespace frost::debug {
namespace {
// The scripted adapter can reply only after the writer has transmitted the launch.
class LaunchGatedAdapter final : public io::IProcess {
public:
    explicit LaunchGatedAdapter(std::unique_ptr<io::IProcess> process) : m_process(std::move(process)) {}
    static constexpr io::ProcessId kPid = 4180;
    io::ProcessId pid() const noexcept override { return kPid; }
    Result<void> writeStdin(std::string_view bytes) override {
        auto result = m_process->writeStdin(bytes);
        if (result && bytes.find("\"command\":\"launch\"") != std::string_view::npos)
            release();
        return result;
    }
    void closeStdin() noexcept override {
        m_process->closeStdin();
        release();
    }
    Result<std::size_t> readStdout(std::span<char> buffer) override {
        m_launch.wait();
        return m_process->readStdout(buffer);
    }
    Result<std::size_t> readStderr(std::span<char> buffer) override { return m_process->readStderr(buffer); }
    Result<io::ExitStatus> wait(std::optional<IClock::Duration> timeout) override { return m_process->wait(timeout); }
    Result<void> terminate() override { return m_process->terminate(); }
    Result<io::ProcessResult> communicate(std::string_view input, IClock::Duration timeout) override {
        return m_process->communicate(input, timeout);
    }
    void abandonOutput() noexcept override { m_process->abandonOutput(); }

private:
    void release() {
        if (!m_released.exchange(true))
            m_launch.count_down();
    }
    std::unique_ptr<io::IProcess> m_process;
    std::atomic<bool> m_released = false;
    std::latch m_launch{1};
};

class LaunchGatedLauncher final : public io::IProcessLauncher {
public:
    io::ScriptedProcessLauncher scripted;
    std::string adapter;

    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        auto launched = scripted.launch(spec);
        if (launched && spec.executable == adapter)
            return std::make_unique<LaunchGatedAdapter>(std::move(*launched));
        return launched;
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                           const io::AsyncContext& context) override {
        return scripted.launchAsync(spec, context);
    }
};

TEST(StandaloneLuaDap, ExplicitLuaExeAndArg0NeverFallBackToAdapterBundledInterpreter) {
    StandaloneLuaTarget target;
    target.interpreter = test::workPath("sdk/selected-lua");
    target.script = test::workPath("project/main.lua");
    target.workingDirectory = test::workPath("project");
    target.interpreterArguments = {"-E", "-e", "assert(_VERSION == 'Lua 5.4')"};
    target.arguments = {"two words", ""};
    target.environment = {{"USER_VALUE", "a=b"}};
    auto launch = standaloneLuaLaunchArguments(target);
    ASSERT_TRUE(launch);
    EXPECT_EQ(launch->stringAt("luaexe"), target.interpreter);
    EXPECT_EQ(launch->stringAt("luaVersion"), "lua54");
    EXPECT_EQ(launch->stringAt("program"), target.script);
    EXPECT_EQ(launch->stringAt("cwd"), target.workingDirectory);
    ASSERT_NE(launch->find("arg0"), nullptr);
    ASSERT_EQ(launch->find("arg0")->size(), 3U);
    EXPECT_EQ(*launch->find("arg0")->items()[2].asString(), "assert(_VERSION == 'Lua 5.4')");
    ASSERT_NE(launch->find("arg"), nullptr);
    ASSERT_EQ(launch->find("arg")->size(), 2U);
    EXPECT_EQ(*launch->find("arg")->items()[0].asString(), "two words");
    EXPECT_EQ(launch->stringAt("console"), "integratedTerminal");
    EXPECT_EQ(launch->boolAt("useWSL"), false);
}
TEST(StandaloneLuaDap, RejectsMissingRuntimeAndRelativeScriptBeforeStartingAdapter) {
    StandaloneLuaTarget target;
    target.script = test::workPath("project/main.lua");
    target.workingDirectory = test::workPath("project");
    EXPECT_FALSE(standaloneLuaLaunchArguments(target));
    target.interpreter = test::workPath("sdk/lua");
    target.script = "relative.lua";
    EXPECT_FALSE(standaloneLuaLaunchArguments(target));
    io::ScriptedProcessLauncher launcher;
    auto factory = standaloneLuaAdapterFactory(launcher, "lua-debug", target);
    EXPECT_FALSE(factory({}));
}

TEST(StandaloneLuaDap, InstalledAdapterUsesNoArgumentsAndOwnedStdioWithNoInterpreterSubstitution) {
    io::ScriptedProcessLauncher launcher;
    const auto adapter = test::workPath("installed-lua-debug/bin/lua-debug");
    launcher.expect({.expectExecutable = adapter, .launchError = Error(ErrorCode::IoError, "authored start refusal")});
    auto started = standaloneLuaAdapterFactory(launcher, adapter, {})({});
    EXPECT_FALSE(started);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_EQ(launches.front().spec.executable, adapter);
    EXPECT_TRUE(launches.front().spec.arguments.empty());
    EXPECT_EQ(launches.front().spec.stdinMode, io::Stdio::Pipe);
    EXPECT_EQ(launches.front().spec.stdoutMode, io::Stdio::Pipe);
    EXPECT_EQ(launches.front().spec.stderrMode, io::Stdio::Pipe);
    EXPECT_TRUE(launches.front().spec.killOnClose);
}

TEST(StandaloneLuaDap, UnsolicitedTerminalRequestClosesTheAdapterWithoutLaunchingLua) {
    io::ScriptedProcessLauncher launcher;
    const auto adapter = test::workPath("installed-lua-debug/bin/lua-debug");
    const auto request = rpc::encodeFrame(R"({"seq":1,"type":"request","command":"runInTerminal","arguments":{}})");
    launcher.expect({.expectExecutable = adapter, .stdoutBytes = request, .outputHeld = true});
    StandaloneLuaTarget target;
    target.interpreter = test::workPath("sdk/lua");
    target.script = test::workPath("project/main.lua");
    target.workingDirectory = test::workPath("project");
    std::atomic<bool> closed = false;
    auto client = standaloneLuaAdapterFactory(launcher, adapter,
                                              std::move(target))({.onClosed = [&](const Error&) { closed = true; }});
    ASSERT_TRUE(client);
    ASSERT_TRUE(test::waitUntil([&] { return closed.load(); }));
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_TRUE(launches.front().terminated);
}

TEST(StandaloneLuaDap, AdmittedLaunchRequiresAnObjectForAnEmptyTerminalEnvironment) {
    for (const auto& environment :
         {rpc::Json(), rpc::Json(false), rpc::Json(""), rpc::Json::array(), rpc::Json::object()}) {
        SCOPED_TRACE(environment.dump());
        const bool valid = environment.isObject();
        LaunchGatedLauncher launcher;
        launcher.adapter = test::workPath("installed-lua-debug/bin/lua-debug");
        StandaloneLuaTarget target;
        target.interpreter = test::workPath("sdk/lua");
        target.script = test::workPath("project/main.lua");
        target.workingDirectory = test::workPath("project");
        const auto bootstrap = test::workPath("installed-lua-debug/script/launch.lua");
        const auto pid = std::to_string(LaunchGatedAdapter::kPid);
#ifdef _WIN32
        const auto code = "dofile[[" + bootstrap + "]] DBG[[" + pid + "]]";
#else
        const auto code = "dofile\"" + bootstrap + "\" DBG\"" + pid + "\"";
#endif
        auto arguments = rpc::Json::object()
                             .set("kind", "integrated")
                             .set("cwd", target.workingDirectory)
                             .set("args", rpc::Json::array({target.interpreter, "-e", code, target.script}))
                             .set("env", environment);
        const auto request = rpc::Json::object()
                                 .set("seq", 1)
                                 .set("type", "request")
                                 .set("command", "runInTerminal")
                                 .set("arguments", std::move(arguments));
        launcher.scripted.expect({.expectExecutable = launcher.adapter,
                                  .stdoutBytes = rpc::encodeFrame(request.dump()),
                                  .outputHeld = !valid});
        launcher.scripted.expect({.expectExecutable = target.interpreter});
        auto launch = standaloneLuaLaunchArguments(target);
        ASSERT_TRUE(launch);
        std::atomic<bool> closed = false;
        auto client = standaloneLuaAdapterFactory(launcher, launcher.adapter,
                                                  target)({.onClosed = [&](const Error&) { closed = true; }});
        ASSERT_TRUE(client);
        ASSERT_GT((*client)->request("launch", std::move(*launch), [](Result<rpc::Json>) {}), 0);
        ASSERT_TRUE(test::waitUntil([&] { return closed.load(); }));
        const auto launches = launcher.scripted.launches();
        ASSERT_EQ(launches.size(), valid ? 2U : 1U);
        EXPECT_NE(launches.front().stdinBytes.find("\"command\":\"launch\""), std::string::npos);
        if (valid) {
            EXPECT_TRUE(launches[1].launched);
            EXPECT_TRUE(launches[1].spec.environment.empty());
        } else {
            EXPECT_TRUE(launches.front().terminated);
            EXPECT_EQ(launcher.scripted.pendingScripts(), 1U);
        }
    }
}

} // namespace
} // namespace frost::debug
