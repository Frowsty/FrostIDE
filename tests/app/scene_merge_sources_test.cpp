// SPDX-License-Identifier: Apache-2.0
// Actual throwaway Git stages; scripted engine output only isolates transport/admission here.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/io/spawn_log.hpp"

#include "scene_merge_work.hpp"
#include "vcs/git_fixture.hpp"
#include <gtest/gtest.h>
namespace frost::app {
namespace {
TEST(SceneMergeSources, ReadsActualImmutableStagesAndRefusesChangedDiskIndexOrTrust) {
    vcs::test::TempRepo repo("scene-merge-sources");
    repo.init();
    const std::string path = "Assets/Scenes/A.fscene";
    const std::string base = R"({"Entities":[],"Name":"base"})", ours = R"({"Entities":[],"Name":"ours"})",
                      theirs = R"({"Entities":[],"Name":"theirs"})";
    repo.write(path, base);
    repo.commitAll("base");
    repo.git({"checkout", "-b", "topic"});
    repo.write(path, theirs);
    repo.commitAll("theirs");
    repo.git({"checkout", "main"});
    repo.write(path, ours);
    repo.commitAll("ours");
    repo.git({"merge", "topic"}, 1);
    using rpc::Json;
    auto result = Json::object()
                      .set("protocol", "frostengine.scene-merge")
                      .set("version", 1)
                      .set("path", path)
                      .set("mode", "semantic")
                      .set("candidateSha256", "")
                      .set("sources", Json::object()
                                          .set("baseExists", true)
                                          .set("base", io::sha256Hex(base))
                                          .set("ours", io::sha256Hex(ours))
                                          .set("theirs", io::sha256Hex(theirs)))
                      .set("result", ours)
                      .set("clean", true)
                      .set("unresolved", 0)
                      .set("conflicts", Json::array())
                      .set("inputIssues", Json::array())
                      .set("validation", Json::object().set("valid", true).set("issues", Json::array()));
    io::ScriptedProcessLauncher engine;
    engine.expect({.stdoutBytes = Json::object().set("ok", true).set("result", result).dump()});
    bool trusted = true;
    EngineUnitTestsSource tool;
    tool.options.program = repo.path("FrostEditor");
    tool.options.project = repo.work();
    tool.options.expectedProjectRoot = repo.fs().physicalPath(repo.work()).value();
    tool.admitted = [&] { return trusted; };
    tool.validatePhysical = []() -> Result<void> { return {}; };
    SceneMergeWork work{repo.fs(), engine, repo.runner(), tool, repo.work(), repo.info().gitDir, repo.path(path), path};
    const auto originalDisk = repo.read(path);
    auto captured = prepareSceneMerge(work, {});
    ASSERT_TRUE(captured) << captured.error().describe();
    EXPECT_EQ(captured->request.images.base, base);
    EXPECT_EQ(captured->request.images.ours, ours);
    EXPECT_EQ(captured->request.images.theirs, theirs);
    EXPECT_EQ(repo.read(path), originalDisk);
    const auto launches = engine.launches();
    ASSERT_EQ(launches.size(), 1U);
    const auto& args = launches[0].spec.arguments;
    ASSERT_GE(args.size(), 3U);
    EXPECT_EQ(args[1], "--params-file");
    EXPECT_FALSE(repo.fs().exists(args[2]).value_or(true));
    repo.write(path, "foreign edit");
    EXPECT_FALSE(validateSceneMergeSources(work, *captured, {}));
    repo.write(path, originalDisk);
    ASSERT_TRUE(validateSceneMergeSources(work, *captured, {}));
    trusted = false;
    EXPECT_FALSE(validateSceneMergeSources(work, *captured, {}));
    trusted = true;
    repo.git({"add", "--", path});
    EXPECT_FALSE(validateSceneMergeSources(work, *captured, {}));
}
} // namespace
} // namespace frost::app
