// SPDX-License-Identifier: Apache-2.0
// Deferred R19 real guardian/transport oracles. Authored only during the implementation phase.
#include "frost/build/remote_process.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/rpc/json.hpp"

#include "support/test_environment_value.hpp"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <sstream>
namespace frost::build {
namespace {
using namespace std::chrono_literals;
io::ProcessSpec workload(std::string python, std::string code) {
    io::ProcessSpec spec;
    spec.executable = std::move(python);
    spec.arguments = {"-I", "-u", "-c", std::move(code)};
    spec.workingDirectory = "/tmp";
    spec.stdinMode = io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    return spec;
}
#ifndef _WIN32
class LocalGuardianTransport final : public io::IProcessLauncher {
public:
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec&) override {
        // This proves the actual guardian/adapter only, never claims SSH transport acceptance.
        auto source = io::realFileSystem().readFileSnapshot(FROST_REMOTE_GUARD_SOURCE, 65536);
        if (!source)
            return std::unexpected(source.error());
        io::ProcessSpec spec;
        spec.executable = FROST_REMOTE_TEST_PYTHON;
        spec.arguments = {"-I", "-u", "-c", *source};
        spec.stdinMode = io::Stdio::Pipe;
        return io::realProcessLauncher().launch(spec);
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec&, const io::AsyncContext&) override {
        return fail(ErrorCode::Unsupported, "The source fixture drives the owned synchronous guardian path");
    }
};
RemoteTarget localEndpoint() {
    RemoteTarget target;
    target.id = "local-guardian-fixture";
    target.name = "Actual local Linux guardian";
    target.transport = RemoteTransport::Ssh;
    target.transportExecutable = FROST_REMOTE_TEST_PYTHON;
    target.host = "loopback.invalid";
    target.user = "fixture";
    target.executionApproved = true;
    return target;
}
std::string startTicks(const std::string& stat) {
    std::istringstream stream(stat.substr(stat.rfind(')') + 2));
    std::string field;
    for (unsigned i = 0; i <= 19; ++i)
        if (!(stream >> field))
            return {};
    return field;
}
TEST(RemoteGuardianReal, ActualArgvEnvironmentAndChildExitDifferFromTransportExit) {
    LocalGuardianTransport transport;
    RemoteProcessLauncher launcher(transport, localEndpoint(), {.admit = []() -> Result<void> { return {}; }});
    auto spec = workload(
        FROST_REMOTE_TEST_PYTHON,
        "import json,os,sys; print(json.dumps({'argv':sys.argv[1:],'value':os.environ['FROST_REMOTE_LITERAL']})); "
        "print('actual stderr',file=sys.stderr); sys.exit(37)");
    spec.arguments.insert(spec.arguments.end(), {"two words", "'; $() && literal", "line one\nline two"});
    spec.environment.emplace_back("FROST_REMOTE_LITERAL", "value with ; $() and spaces");
    auto result = io::runToCompletion(launcher, spec, "", 15s);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->exitCode, 37) << "Guardian/transport exits zero; only acknowledged child code is returned";
    auto actual = rpc::parseJson(result->stdoutBytes);
    ASSERT_TRUE(actual);
    ASSERT_NE(actual->find("argv"), nullptr);
    ASSERT_EQ(actual->find("argv")->size(), 3U);
    EXPECT_EQ(*actual->find("argv")->items()[1].asString(), "'; $() && literal");
    EXPECT_EQ(actual->stringAt("value"), "value with ; $() and spaces");
    EXPECT_EQ(result->stderrBytes, "actual stderr\n");
}
TEST(RemoteGuardianReal, StopAcknowledgesCleanupAndIndependentProcOracleSeesDescendantGone) {
    LocalGuardianTransport transport;
    std::atomic<bool> admitted{true};
    RemoteProcessLauncher launcher(transport, localEndpoint(), {.admit = [&]() -> Result<void> {
                                       return admitted ? Result<void>{}
                                                       : Result<void>(fail(ErrorCode::Untrusted, "revoked"));
                                   }});
    auto spec = workload(FROST_REMOTE_TEST_PYTHON,
                         "import json,subprocess,sys,time; child=subprocess.Popen([sys.executable,'-I','-c','import "
                         "time; time.sleep(60)']); print(json.dumps({'child':child.pid}),flush=True); time.sleep(60)");
    auto process = launcher.launch(spec);
    ASSERT_TRUE(process) << process.error().describe();
    std::array<char, 4096> buffer{};
    std::string line;
    while (line.find('\n') == line.npos) {
        auto count = (*process)->readStdout(buffer);
        ASSERT_TRUE(count);
        ASSERT_GT(*count, 0U);
        line.append(buffer.data(), *count);
        ASSERT_LE(line.size(), 4096U);
    }
    auto actual = rpc::parseJson(line);
    ASSERT_TRUE(actual);
    const auto child = actual->integerAt("child");
    ASSERT_TRUE(child);
    const auto stat = "/proc/" + std::to_string(*child) + "/stat";
    auto before = io::realFileSystem().readFile(stat);
    ASSERT_TRUE(before);
    const auto identity = startTicks(*before);
    ASSERT_FALSE(identity.empty());
    ASSERT_TRUE((*process)->terminate());
    auto exit = (*process)->wait(15s);
    ASSERT_TRUE(exit) << exit.error().describe();
    EXPECT_EQ(exit->exitCode, 137);
    auto after = io::realFileSystem().readFile(stat);
    EXPECT_TRUE(!after || startTicks(*after) != identity) << "Actual workload descendant identity survived Stop";
}
#endif
TEST(RemoteTransportReal, ExplicitlySelectedWslOrSshEndpointReportsActualWorkloadArgv) {
    RemoteTarget target;
    target.id = "explicit-acceptance";
    target.name = "Explicit test endpoint";
    target.executionApproved = true;
#ifdef _WIN32
    target.transport = RemoteTransport::Wsl;
    target.transportExecutable = test::environmentValue("FROST_REMOTE_TEST_WSL").value_or("");
    target.distribution = test::environmentValue("FROST_REMOTE_TEST_DISTRIBUTION").value_or("");
    if (target.transportExecutable.empty() || target.distribution.empty())
        GTEST_SKIP() << "No explicitly selected WSL acceptance endpoint; not counted as transport acceptance";
#else
    target.transport = RemoteTransport::Ssh;
    target.transportExecutable = test::environmentValue("FROST_REMOTE_TEST_SSH").value_or("");
    target.host = test::environmentValue("FROST_REMOTE_TEST_HOST").value_or("");
    target.user = test::environmentValue("FROST_REMOTE_TEST_USER").value_or("");
    if (target.transportExecutable.empty() || target.host.empty() || target.user.empty())
        GTEST_SKIP()
            << "No explicitly selected strict-known-host SSH acceptance endpoint; not counted as transport acceptance";
#endif
    if (auto python = test::environmentValue("FROST_REMOTE_TEST_LINUX_PYTHON"); python && !python->empty())
        target.remotePython = *python;
    RemoteProcessLauncher launcher(io::realProcessLauncher(), target, {.admit = []() -> Result<void> { return {}; }});
    auto spec = workload(target.remotePython, "import json,sys; print(json.dumps(sys.argv[1:])); sys.exit(23)");
    spec.arguments.emplace_back("literal space ; $() argument");
    auto actual = io::runToCompletion(launcher, spec, "", 30s);
    ASSERT_TRUE(actual) << actual.error().describe();
    EXPECT_EQ(actual->exitCode, 23);
    auto value = rpc::parseJson(actual->stdoutBytes);
    ASSERT_TRUE(value);
    ASSERT_EQ(value->size(), 1U);
    EXPECT_EQ(*value->items()[0].asString(), "literal space ; $() argument");
}
} // namespace
} // namespace frost::build
