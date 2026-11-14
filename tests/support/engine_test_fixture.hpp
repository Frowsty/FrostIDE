// SPDX-License-Identifier: Apache-2.0
// FER-10 validator inputs, not a substitute for the mandatory real-engine corpus acceptance.
#pragma once
#include "frost/engine/engine_test_provider.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include "support/scratch_dir.hpp"

namespace frost::test {
using rpc::Json;
inline Json engineCase(std::string name = "actual case", std::string status = "Failed") {
    return Json::object()
        .set("name", std::move(name))
        .set("status", std::move(status))
        .set("location", "Assets/Scripts/Test.lua:4")
        .set("path", "Assets/Scripts/Test.lua")
        .set("line", 4);
}
inline Json engineRuntimeResults(bool passed) {
    auto failure = Json::object()
                       .set("message", "actual assertion message")
                       .set("testCase", "actual case")
                       .set("location", "Assets/Scripts/Test.lua:5")
                       .set("path", "Assets/Scripts/Test.lua")
                       .set("line", 5);
    return Json::object()
        .set("passed", passed)
        .set("cases", 1)
        .set("passedCases", passed ? 1 : 0)
        .set("failedCases", passed ? 0 : 1)
        .set("skippedCases", 0)
        .set("failures", passed ? Json::array() : Json::array({failure}))
        .set("caseResults", Json::array({engineCase("actual case", passed ? "Passed" : "Failed")}));
}
inline Json engineSceneReport(std::string scene, bool passed) {
    auto entry = Json::object()
                     .set("scene", std::move(scene))
                     .set("passed", passed)
                     .set("durationMicros", 12345)
                     .set("scriptErrorCount", 0)
                     .set("scriptErrors", Json::array())
                     .set("testResults", engineRuntimeResults(passed));
    return Json::object()
        .set("passed", passed)
        .set("scenes", Json::array({entry}))
        .set("report", Json::array({"actual report line"}))
        .set("totals",
             Json::object().set("scenes", 1).set("passedScenes", passed ? 1 : 0).set("failedScenes", passed ? 0 : 1));
}
struct EngineTestFixture {
    ScratchDir dir{"engine-test-provider"};
    engine::EngineTestProviderOptions options;
    EngineTestFixture() {
        auto& fs = io::realFileSystem();
        EXPECT_TRUE(fs.createDirectories(dir.join("Assets/Scripts")));
        EXPECT_TRUE(fs.writeFileAtomic(dir.join("Assets/Scripts/Test.lua"),
                                       "-- line 1\n-- line 2\n-- line 3\n-- line 4\n-- line 5\n"));
        EXPECT_TRUE(fs.writeFileAtomic(dir.join("Game.frostproj"), "{}"));
        EXPECT_TRUE(fs.writeFileAtomic(dir.join("FrostEditor.exe"), "validator input, never executed"));
        options.program = dir.join("FrostEditor.exe");
        options.expectedProgram = fs.physicalPath(options.program).value_or("");
        options.project = dir.path();
        options.expectedProjectRoot = fs.physicalPath(dir.path()).value_or("");
        options.expectedProjectFile = fs.physicalPath(dir.join("Game.frostproj")).value_or("");
    }
    Json listingJson() const {
        return Json::object()
            .set("version", 1)
            .set("granularity", "scene")
            .set("projectPath", options.expectedProjectRoot)
            .set("projectFile", *options.expectedProjectFile)
            .set("count", 2)
            .set(
                "tests",
                Json::array({Json::object().set("id", "Assets/Tests/A.fscene").set("scene", "Assets/Tests/A.fscene"),
                             Json::object().set("id", "Assets/Tests/B.fscene").set("scene", "Assets/Tests/B.fscene")}));
    }
    void queue(io::ScriptedProcessLauncher& launcher, std::string command, Json result, int exit = 0) const {
        io::ScriptedProcess script;
        script.exitCode = exit;
        script.stdoutBytes =
            Json::object().set("command", std::move(command)).set("ok", true).set("result", std::move(result)).dump();
        launcher.expect(std::move(script));
    }
};
} // namespace frost::test
