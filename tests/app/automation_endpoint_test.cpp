// SPDX-License-Identifier: Apache-2.0
//
// The automation endpoint (src/app/src/automation_endpoint.hpp; FND-7a, ADR 0080) over a WorkbenchRig
// in this process, with the real cmake, Ninja and C compiler of this build:
//   - the session token: a first request other than hello, or a wrong token, is refused and the
//     connection closed; the connection file names the protocol, server, token and pid;
//   - the R0 trust criterion through the endpoint (the R0 deviation closed here): opening an
//     untrusted CMake folder, asking for its presets and a configure starts no process and trusts
//     nothing; the endpoint's trust (the trust dialog's button) starts the configure;
//   - a small j1: the listed presets equal `cmake --list-presets` (the oracle, run here), a build of
//     one target with a seeded compile error shows it in Problems at the line the compiler reported
//     (oracle: the line of the seeded error in the source this test wrote), Problems' activation
//     opens it there, the fix rebuilds clean, and clean succeeds.
// In a build without the endpoint (Release) the test checks that it is absent instead.

#include "frost/build/cmake_service.hpp"
#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/json.hpp"

#include "automation_endpoint.hpp"
#include "support/endpoint_client.hpp"
#include "workbench_rig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using frost::app::AutomationEndpoint;
using frost::app::test::RigOptions;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
using frost::rpc::Json;
using frost::test::EndpointClient;

#ifdef FROST_AUTOMATION_ENDPOINT

// The real launcher, recording the file names of what it starts.
class RecordingLauncher final : public frost::io::IProcessLauncher {
public:
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        record(spec);
        return frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        record(spec);
        return frost::io::realProcessLauncher().launchAsync(spec, context);
    }
    [[nodiscard]] std::vector<std::string> names() const {
        const std::scoped_lock lock(m_mutex);
        return m_names;
    }

private:
    void record(const frost::io::ProcessSpec& spec) {
        const std::scoped_lock lock(m_mutex);
        m_names.emplace_back(frost::io::fileName(spec.executable));
    }
    mutable std::mutex m_mutex;
    std::vector<std::string> m_names;
};

// The connection file the endpoint wrote, parsed.
Json connection(const WorkDir& dir) {
    auto text = frost::io::realFileSystem().readFile(dir.join("automation.json"));
    EXPECT_TRUE(text.has_value());
    auto parsed = frost::rpc::parseJson(text.value_or("{}"));
    EXPECT_TRUE(parsed.has_value());
    return parsed.value_or(Json::object());
}

// The result of `reply`, failing the test with its error when it has none.
Json result(const Json& reply, std::string_view what) {
    if (const Json* value = reply.find("result"); value != nullptr)
        return *value;
    const Json* error = reply.find("error");
    ADD_FAILURE() << what << " failed: " << (error != nullptr ? error->dump() : std::string("no reply"));
    return Json::object();
}

// A C project with two configure presets and two targets; main.c's line 3 is where the seeded
// error goes. The compiler is this build's own.
void writeProject(WorkDir& dir) {
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nset(CMAKE_C_COMPILER \"" + compiler +
                                    "\" CACHE FILEPATH \"\" FORCE)\nproject(Endpoint C)\n"
                                    "add_executable(hello main.c)\nadd_executable(other other.c)\n");
    dir.write("main.c", "int main(void) {\n    int answer = 42;\n    return answer;\n}\n");
    dir.write("other.c", "int main(void) { return 0; }\n");
    dir.write("CMakePresets.json", R"({"version":3,"configurePresets":[
        {"name":"dev-debug","generator":"Ninja","binaryDir":"${sourceDir}/build/dev-debug",
         "cacheVariables":{"CMAKE_BUILD_TYPE":"Debug"}},
        {"name":"dev-release","generator":"Ninja","binaryDir":"${sourceDir}/build/dev-release",
         "cacheVariables":{"CMAKE_BUILD_TYPE":"Release"}}],
      "buildPresets":[{"name":"dev-debug","configurePreset":"dev-debug"}]})");
}

// The configure presets `cmake --list-presets` prints for `dir` (the oracle): the quoted names.
std::vector<std::string> cmakeListedPresets(const std::string& dir) {
    frost::io::ProcessSpec spec;
    spec.executable = frost::io::normalizePath(FROST_TEST_CMAKE).value_or(FROST_TEST_CMAKE);
    spec.arguments = {"--list-presets=configure"};
    spec.workingDirectory = dir;
    auto run = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 120s);
    EXPECT_TRUE(run.has_value() && run->exitCode == 0) << (run ? run->stderrBytes : run.error().describe());
    std::vector<std::string> names;
    std::istringstream lines(run ? run->stdoutBytes : std::string());
    for (std::string line; std::getline(lines, line);) {
        const auto open = line.find('"');
        const auto close = open == std::string::npos ? open : line.find('"', open + 1);
        if (close != std::string::npos)
            names.push_back(line.substr(open + 1, close - open - 1));
    }
    return names;
}

TEST(AutomationEndpoint, RefusesAFirstRequestOtherThanHelloAndAWrongToken) {
    ASSERT_TRUE(AutomationEndpoint::available());
    WorkDir dir("endpoint-token");
    WorkbenchRig rig(dir);
    auto endpoint = AutomationEndpoint::start(*rig.workbench, dir.join("automation.json"));
    ASSERT_TRUE(endpoint.has_value()) << endpoint.error().describe();
    const Json file = connection(dir);
    EXPECT_EQ(file.stringAt("protocol"), "frost-automation-endpoint/1");
    EXPECT_EQ(file.stringAt("server"), (*endpoint)->serverName());
    EXPECT_EQ(file.stringAt("token"), (*endpoint)->token());
    EXPECT_EQ((*endpoint)->token().size(), 64U);
    EXPECT_EQ(file.integerAt("pid"), QCoreApplication::applicationPid());

    {
        EndpointClient client((*endpoint)->serverName());
        ASSERT_TRUE(client.connected());
        const Json reply = client.request("state");
        ASSERT_NE(reply.find("error"), nullptr) << reply.dump();
        EXPECT_EQ(reply.find("error")->stringAt("code"), "PermissionDenied");
        EXPECT_TRUE(frost::app::test::waitFor([&] { return !client.connected(); }, 10s)) << "still connected";
    }
    {
        EndpointClient client((*endpoint)->serverName());
        std::string wrong = (*endpoint)->token();
        wrong.back() = wrong.back() == '0' ? '1' : '0';
        const Json reply = client.request("hello", Json::object().set("token", wrong));
        ASSERT_NE(reply.find("error"), nullptr) << reply.dump();
        EXPECT_TRUE(frost::app::test::waitFor([&] { return !client.connected(); }, 10s));
    }
    EndpointClient client((*endpoint)->serverName());
    const Json hello = result(client.request("hello", Json::object().set("token", (*endpoint)->token())), "hello");
    EXPECT_EQ(hello.stringAt("protocol"), "frost-automation-endpoint/1");
    const Json state = result(client.request("state"), "state");
    EXPECT_EQ(state.stringAt("project"), "");
    EXPECT_EQ(state.find("cmake")->boolAt("available"), false);
    const Json unknown = client.request("no.such.method");
    ASSERT_NE(unknown.find("error"), nullptr);
    EXPECT_EQ(unknown.find("error")->stringAt("code"), "NotFound");
}

/*
 * The R0 trust criterion through the endpoint, then a small j1 on a two-target C project with the
 * real cmake, Ninja and compiler.
 */
TEST(AutomationEndpoint, DrivesTrustPresetsConfigureBuildProblemsAndCleanOfARealCMakeProject) {
    WorkDir profile("endpoint-profile");
    WorkDir dir("endpoint-cmake");
    writeProject(dir);
    RecordingLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(profile, std::move(options));
    rig.show();
    auto endpoint = AutomationEndpoint::start(*rig.workbench, profile.join("automation.json"));
    ASSERT_TRUE(endpoint.has_value()) << endpoint.error().describe();
    EndpointClient client((*endpoint)->serverName());
    result(client.request("hello", Json::object().set("token", (*endpoint)->token())), "hello");

    // Untrusted: open, list presets, pick one, ask for a configure. Nothing starts, nothing is trusted.
    result(client.request("open", Json::object().set("path", dir.path())), "open");
    const Json profiles = result(client.request("cmake.profiles"), "cmake.profiles");
    std::vector<std::string> listed;
    for (const Json& item : profiles.find("profiles")->items())
        listed.emplace_back(item.stringAt("configurePreset"));
    // Picking the profile needs no process either (the trust grant below configures this one).
    result(client.request("cmake.select", Json::object().set("name", "dev-debug")), "cmake.select");
    const Json refused = client.request("cmake.configure");
    const Json waited =
        result(client.request("wait", Json::object().set("for", "cmake").set("timeoutMs", 60000)), "wait");
    EXPECT_EQ(waited.boolAt("timedOut"), false);
    EXPECT_EQ(waited.find("cmake")->find("lastConfigure"), nullptr) << "a configure ran before trust";
    EXPECT_TRUE(launcher.names().empty()) << "a process started before trust: " << launcher.names().front();
    EXPECT_FALSE(rig.trust->isTrusted(dir.path())) << "trusted silently";
    EXPECT_FALSE(frost::io::realFileSystem().exists(dir.join("build")).value_or(true));
    (void)refused; // Refused or queued behind trust: either way nothing ran (checked above).

    // The presets the endpoint lists are cmake's own (oracle run only now: it is a process too).
    const std::vector<std::string> oracle = cmakeListedPresets(dir.path());
    ASSERT_FALSE(oracle.empty());
    EXPECT_EQ(std::set<std::string>(listed.begin(), listed.end()), std::set<std::string>(oracle.begin(), oracle.end()));

    // The trust dialog's grant, through the endpoint: the configure runs.
    result(client.request("trust", Json::object().set("root", dir.path())), "trust");
    EXPECT_TRUE(rig.trust->isTrusted(dir.path()));
    Json state;
    ASSERT_TRUE(frost::app::test::waitFor(
        [&] {
            state = result(client.request("wait", Json::object().set("for", "cmake").set("timeoutMs", 1000)), "wait");
            const Json* configured = state.find("cmake")->find("lastConfigure");
            return configured != nullptr && configured->boolAt("succeeded") == true &&
                   state.find("cmake")->boolAt("modelLoaded") == true;
        },
        240s))
        << state.dump();
    EXPECT_EQ(state.find("cmake")->stringAt("buildDirectory"), dir.join("build/dev-debug"));
    const auto names = launcher.names();
    EXPECT_TRUE(std::ranges::any_of(names, [](const std::string& name) { return name.starts_with("cmake"); }));

    const Json targets = result(client.request("cmake.targets"), "cmake.targets");
    std::set<std::string> targetNames;
    for (const Json& item : targets.find("targets")->items())
        targetNames.emplace(item.stringAt("name"));
    EXPECT_TRUE(targetNames.contains("hello") && targetNames.contains("other")) << targets.dump();

    // A seeded compile error on line 3 of main.c: Problems shows it there and opens it there.
    const std::string main = dir.join("main.c");
    dir.write("main.c", "int main(void) {\n    int answer = 42;\n    return answer + undefined_symbol;\n}\n");
    result(client.request("cmake.build", Json::object().set("target", "hello")), "cmake.build");
    state = result(client.request("wait", Json::object().set("for", "cmake").set("timeoutMs", 240000)), "wait");
    ASSERT_NE(state.find("cmake")->find("lastBuild"), nullptr) << state.dump();
    EXPECT_EQ(state.find("cmake")->find("lastBuild")->boolAt("succeeded"), false);
    const Json problems = result(client.request("problems"), "problems");
    std::int64_t index = -1;
    for (std::size_t i = 0; i < problems.find("problems")->items().size(); ++i) {
        const Json& item = problems.find("problems")->items()[i];
        if (item.stringAt("severity") == "error" && frost::io::normalizePath(item.stringAt("file")) == main &&
            index < 0)
            index = static_cast<std::int64_t>(i);
    }
    ASSERT_GE(index, 0) << problems.dump();
    const Json& problem = problems.find("problems")->items()[static_cast<std::size_t>(index)];
    EXPECT_EQ(problem.integerAt("line"), 3) << problem.dump();
    EXPECT_EQ(problem.boolAt("jumpable"), true);
    result(client.request("problems.activate", Json::object().set("index", index)), "problems.activate");
    Json editor;
    ASSERT_TRUE(frost::app::test::waitFor(
        [&] {
            editor = result(client.request("editor"), "editor");
            return editor.boolAt("available") == true && frost::io::normalizePath(editor.stringAt("path")) == main;
        },
        30s))
        << editor.dump();
    EXPECT_EQ(editor.integerAt("line"), 3);

    // Fix, rebuild: success and no build error left; then clean.
    dir.write("main.c", "int main(void) {\n    int answer = 42;\n    return answer;\n}\n");
    result(client.request("cmake.build", Json::object().set("target", "hello")), "cmake.build");
    state = result(client.request("wait", Json::object().set("for", "cmake").set("timeoutMs", 240000)), "wait");
    EXPECT_EQ(state.find("cmake")->find("lastBuild")->boolAt("succeeded"), true) << state.dump();
    EXPECT_EQ(state.find("cmake")->integerAt("errors"), 0);
    result(client.request("cmake.build", Json::object().set("action", "clean")), "clean");
    state = result(client.request("wait", Json::object().set("for", "cmake").set("timeoutMs", 240000)), "wait");
    EXPECT_EQ(state.find("cmake")->find("lastBuild")->stringAt("action"), "Clean") << state.dump();
    EXPECT_EQ(state.find("cmake")->find("lastBuild")->boolAt("succeeded"), true) << state.dump();
    ASSERT_TRUE(rig.workbench->cmakeService()->waitForIdle(120s));
}

#else

// Builds without the endpoint (Release; FROST_ENABLE_AUTOMATION_ENDPOINT off) have none of it.
TEST(AutomationEndpoint, IsAbsentFromThisBuild) {
    EXPECT_FALSE(AutomationEndpoint::available());
    WorkDir dir("endpoint-absent");
    WorkbenchRig rig(dir);
    auto endpoint = AutomationEndpoint::start(*rig.workbench, dir.join("automation.json"));
    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().code(), frost::ErrorCode::Unsupported);
    EXPECT_FALSE(frost::io::realFileSystem().exists(dir.join("automation.json")).value_or(true));
}

#endif

} // namespace
