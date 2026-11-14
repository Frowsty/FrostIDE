// SPDX-License-Identifier: Apache-2.0
// FER-10 mandatory real-engine oracle. Missing pinned engine/tool/command fails; no skip/fake case list.
#include "frost/engine/engine_test_provider.hpp"
#include "frost/io/async_process.hpp"

#include "support/real_engine_session.hpp"
#include <gtest/gtest.h>

#include <algorithm>

namespace frost::engine {
namespace {
using rpc::Json;
class CancelAfterLaunch final : public io::IProcessLauncher {
public:
    std::stop_source stop;
    std::size_t launches = 0;
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        auto process = io::realProcessLauncher().launch(spec);
        if (process) {
            ++launches;
            stop.request_stop();
        }
        return process;
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec&, const io::AsyncContext&) override {
        return fail(ErrorCode::Unsupported, "corpus cancellation probe is synchronous");
    }
};
TEST(EngineTestProviderCorpus, RealListingSelectedAllFailedLocationsDurationsAndOwnedCancellation) {
    test::RealEngineBuild build;
    auto ready = build.prepare();
    ASSERT_TRUE(ready) << ready.error().message();
    test::ScratchDir project("engine-scene-tests-corpus");
    auto copied = build.copyProject(project.join("Game"));
    ASSERT_TRUE(copied) << copied.error().message();
    auto& fs = io::realFileSystem();
    const auto root = fs.canonicalPath(project.join("Game"));
    ASSERT_TRUE(root);
    const std::string source = "local Tests = {}\nfunction Tests:OnCreate()\n  Test.Case('registered at runtime', "
                               "function()\n    Test.Equal(2 + 2, 5, 'actual failure')\n  end)\nend\nreturn Tests\n";
    auto installed = test::installAcceptanceScript(*root, source);
    ASSERT_TRUE(installed) << installed.error().message();
    auto refreshed = build.cli("assets.refresh", *root);
    ASSERT_TRUE(refreshed) << refreshed.error().message();
    auto raw = build.cli("test.list", *root);
    ASSERT_TRUE(raw) << raw.error().message();
    auto oracleListing = parseEngineTestListing(*raw);
    ASSERT_TRUE(oracleListing) << oracleListing.error().message();
    EngineTestProviderOptions options;
    options.program = build.program;
    options.expectedProgram = fs.physicalPath(build.program).value();
    options.project = *root;
    options.expectedProjectRoot = *root;
    auto file = fs.canonicalPath(oracleListing->projectFile);
    ASSERT_TRUE(file);
    options.expectedProjectFile = *file;
    options.frames = 2;
    auto listed = discoverEngineSceneTests(fs, io::realProcessLauncher(), options, [] { return true; });
    ASSERT_TRUE(listed) << listed.error().message();
    EXPECT_EQ(*listed, *oracleListing);
    ASSERT_FALSE(listed->tests.empty());
    const auto found =
        std::ranges::find_if(listed->tests, [](const auto& t) { return t.scene == "Assets/Tests/FeatureTest.fscene"; });
    ASSERT_NE(found, listed->tests.end());
    const std::vector<std::string> ids{found->id};
    auto actualReport =
        build.cli("test.run", *root,
                  Json::object().set("scenes", Json::array({found->scene})).set("frames", 2).set("mode", "headless"));
    ASSERT_TRUE(actualReport) << actualReport.error().message();
    auto oracle = parseEngineTestRun(*actualReport, ids);
    ASSERT_TRUE(oracle) << oracle.error().message();
    ASSERT_EQ(oracle->size(), 1u);
    ASSERT_EQ(oracle->front().cases.size(), 1u);
    EXPECT_EQ(oracle->front().cases.front().name, "registered at runtime");
    EXPECT_FALSE(oracle->front().cases.front().duration);
    const auto& rawScene = actualReport->find("scenes")->items().front();
    ASSERT_TRUE(rawScene.unsignedAt("durationMicros"));
    EXPECT_EQ(oracle->front().duration, std::chrono::microseconds(*rawScene.unsignedAt("durationMicros")));
    auto selected = runEngineSceneTests(fs, io::realProcessLauncher(), options, *listed, ids, [] { return true; });
    ASSERT_FALSE(selected.error) << selected.error->message();
    ASSERT_EQ(selected.results.size(), 1u);
    EXPECT_EQ(selected.results.front().scene, oracle->front().scene);
    EXPECT_EQ(selected.results.front().outcome, oracle->front().outcome);
    EXPECT_EQ(selected.results.front().cases, oracle->front().cases);
    EXPECT_EQ(selected.results.front().failures, oracle->front().failures);
    ASSERT_TRUE(selected.results.front().duration);
    ASSERT_FALSE(selected.results.front().failures.empty());
    const auto& failure = selected.results.front().failures.front();
    ASSERT_TRUE(failure.location);
    EXPECT_EQ(failure.location->line, 4u);
    auto location = resolveEngineTestLocation(fs, *root, *failure.location);
    ASSERT_TRUE(location) << location.error().message();
    EXPECT_TRUE(location->ends_with("Hero.lua"));
    const auto failed = failedEngineSceneTests(*listed, selected.results);
    EXPECT_EQ(failed, ids);
    auto rerun = runEngineSceneTests(fs, io::realProcessLauncher(), options, *listed, failed, [] { return true; });
    ASSERT_FALSE(rerun.error);
    ASSERT_EQ(rerun.results.size(), 1u);
    EXPECT_EQ(rerun.results.front().scene, found->scene);
    auto all = runEngineSceneTests(fs, io::realProcessLauncher(), options, *listed, {}, [] { return true; });
    ASSERT_FALSE(all.error) << all.error->message();
    ASSERT_EQ(all.results.size(), listed->tests.size());
    for (std::size_t i = 0; i < all.results.size(); ++i)
        EXPECT_EQ(all.results[i].scene, listed->tests[i].scene);
    auto refusal = runEngineSceneTests(fs, io::realProcessLauncher(), options, *listed,
                                       std::vector<std::string>{"Assets/Tests/NotListed.fscene"}, [] { return true; });
    ASSERT_TRUE(refusal.error);
    EXPECT_TRUE(refusal.results.empty());
    CancelAfterLaunch cancel;
    auto cancelled =
        runEngineSceneTests(fs, cancel, options, *listed, ids, [] { return true; }, cancel.stop.get_token());
    EXPECT_EQ(cancel.launches, 1u);
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_TRUE(cancelled.results.empty());
    ASSERT_TRUE(cancelled.error);
    EXPECT_EQ(cancelled.error->code(), ErrorCode::Cancelled);
}
} // namespace
} // namespace frost::engine
