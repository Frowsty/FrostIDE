// SPDX-License-Identifier: Apache-2.0
// FER-10 unit contract/refusal tests; real command fidelity is checked in the corpus target.
#include "support/engine_test_fixture.hpp"
#include "support/directory_link.hpp"
#include <gtest/gtest.h>

#include <algorithm>

namespace frost::engine {
namespace {
using test::Json;
TEST(EngineTestParser, SceneListingRefusesInventedGranularityUnsafeDuplicateAndWrongCount) {
    test::EngineTestFixture f;
    auto listing = parseEngineTestListing(f.listingJson());
    ASSERT_TRUE(listing);
    EXPECT_EQ(listing->tests.front().id, "Assets/Tests/A.fscene");
    auto wrong = f.listingJson();
    wrong.set("granularity", "case");
    EXPECT_EQ(parseEngineTestListing(wrong).error().code(), ErrorCode::Unsupported);
    wrong = f.listingJson();
    wrong.set("count", 3);
    EXPECT_FALSE(parseEngineTestListing(wrong));
    wrong = f.listingJson();
    wrong.set("version", 1.0);
    EXPECT_FALSE(parseEngineTestListing(wrong));
    wrong = f.listingJson();
    auto duplicate = wrong.find("tests")->items().front();
    wrong.find("tests")->push(duplicate);
    wrong.set("count", 3);
    EXPECT_FALSE(parseEngineTestListing(wrong));
    wrong = f.listingJson(); // Mutation below replaces the whole scene array.
    wrong.set("count", 1)
        .set("tests",
             Json::array(
                 {Json::object().set("id", "Assets/../Outside.fscene").set("scene", "Assets/../Outside.fscene")}));
    EXPECT_FALSE(parseEngineTestListing(wrong));
}
TEST(EngineTestParser, PreservesActualCasesFailuresLocationsAndUnknownCaseDuration) {
    const std::vector<std::string> ids{"Assets/Tests/A.fscene"};
    auto parsed = parseEngineTestRun(test::engineSceneReport(ids.front(), false), ids);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->size(), 1u);
    const auto& scene = parsed->front();
    EXPECT_EQ(scene.outcome, EngineTestOutcome::Failed);
    EXPECT_EQ(scene.duration, std::chrono::microseconds(12345));
    ASSERT_EQ(scene.cases.size(), 1u);
    EXPECT_EQ(scene.cases.front().name, "actual case");
    EXPECT_FALSE(scene.cases.front().duration);
    ASSERT_EQ(scene.failures.size(), 1u);
    EXPECT_EQ(scene.failures.front().location, (EngineTestLocation{"Assets/Scripts/Test.lua", 5}));
    auto wrong = test::engineSceneReport(ids.front(), false);
    EXPECT_FALSE(parseEngineTestRun(wrong, std::vector<std::string>{"Assets/Tests/Other.fscene"}));
    auto* sceneJson = wrong.find("scenes");
    auto entry = sceneJson->items().front();
    entry.set("durationMicros", -1);
    wrong.set("scenes", Json::array({entry}));
    EXPECT_FALSE(parseEngineTestRun(wrong, ids));
    entry = test::engineSceneReport(ids.front(), false).find("scenes")->items().front();
    auto results = *entry.find("testResults");
    results.set("failedCases", 0);
    entry.set("testResults", results);
    wrong.set("scenes", Json::array({entry}));
    EXPECT_FALSE(parseEngineTestRun(wrong, ids));
}
TEST(EngineTestParser, FailedSceneWithoutAssertionsRetainsActualExitReasonAndSharedLogTail) {
    const std::vector<std::string> ids{"Assets/Tests/A.fscene"};
    auto report = test::engineSceneReport(ids.front(), false);
    auto scene = report.find("scenes")->items().front();
    scene.set("testResults", nullptr)
        .set("exitCode", 11)
        .set("exitName", "EngineFailure")
        .set("exitReason", "actual engine exit reason")
        .set("logTail", Json::array({Json::object()
                                         .set("level", "error")
                                         .set("logger", "engine")
                                         .set("message", "actual shared scene log")}));
    report.set("scenes", Json::array({scene}));
    auto parsed = parseEngineTestRun(report, ids);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->size(), 1u);
    EXPECT_TRUE(parsed->front().cases.empty());
    EXPECT_TRUE(parsed->front().failures.empty());
    EXPECT_EQ(parsed->front().exitReason, "actual engine exit reason");
    EXPECT_EQ(parsed->front().exitName, "EngineFailure");
    EXPECT_EQ(parsed->front().exitCode, 11);
    ASSERT_EQ(parsed->front().logTail.size(), 1u);
    EXPECT_EQ(parsed->front().logTail.front().message, "actual shared scene log");
}
TEST(EngineTestParser, ActualScriptLoadErrorWithUnknownLineRemainsAnErrorWithoutInventedLocation) {
    const std::vector<std::string> ids{"Assets/Tests/A.fscene"};
    auto report = test::engineSceneReport(ids.front(), false);
    auto scene = report.find("scenes")->items().front();
    scene.set("testResults", nullptr)
        .set("scriptErrorCount", 1)
        .set("scriptErrors", Json::array({Json::object()
                                              .set("message", "cannot load script")
                                              .set("path", "Assets/Scripts/Test.lua")
                                              .set("line", 0)}));
    report.set("scenes", Json::array({scene}));
    auto parsed = parseEngineTestRun(report, ids);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed->front().cases.empty());
    ASSERT_EQ(parsed->front().failures.size(), 1u);
    EXPECT_EQ(parsed->front().failures.front().message, "cannot load script");
    EXPECT_FALSE(parsed->front().failures.front().location);
}
TEST(EngineTestParser, PeerReportsPreserveDuplicateCaseNamesWithoutCountingPrimaryTwice) {
    const std::vector<std::string> ids{"Assets/Tests/A.fscene"};
    auto report = test::engineSceneReport(ids.front(), false);
    auto scene = report.find("scenes")->items().front();
    auto peer = Json::object()
                    .set("name", "server")
                    .set("scriptErrorCount", 0)
                    .set("scriptErrors", Json::array())
                    .set("testResults", test::engineRuntimeResults(false));
    auto second = peer;
    second.set("name", "client");
    scene.set("peers", Json::array({peer, second}));
    report.set("scenes", Json::array({scene}));
    auto parsed = parseEngineTestRun(report, ids);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->front().cases.size(), 2u);
    ASSERT_EQ(parsed->front().failures.size(), 2u);
    EXPECT_EQ(parsed->front().cases[0].peer, "server");
    EXPECT_EQ(parsed->front().cases[1].peer, "client");
}
TEST(EngineTestProvider, RunsOnlyExactSelectedScenesAndRerunsOnlyActualFailures) {
    test::EngineTestFixture f;
    io::ScriptedProcessLauncher launcher;
    f.queue(launcher, "test.list", f.listingJson());
    auto listing = discoverEngineSceneTests(io::realFileSystem(), launcher, f.options, [] { return true; });
    ASSERT_TRUE(listing) << listing.error().message();
    f.queue(launcher, "test.run", test::engineSceneReport("Assets/Tests/B.fscene", false), 3);
    const std::vector<std::string> selected{"Assets/Tests/B.fscene"};
    auto run = runEngineSceneTests(io::realFileSystem(), launcher, f.options, *listing, selected, [] { return true; });
    ASSERT_FALSE(run.error);
    ASSERT_EQ(run.results.size(), 1u);
    EXPECT_EQ(failedEngineSceneTests(*listing, run.results), selected);
    auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2u);
    EXPECT_NE(std::ranges::find(launches.back().spec.arguments, "test.run"), launches.back().spec.arguments.end());
    auto p = std::ranges::find(launches.back().spec.arguments, "--params");
    ASSERT_NE(p, launches.back().spec.arguments.end());
    ASSERT_NE(++p, launches.back().spec.arguments.end());
    auto params = rpc::parseJson(*p);
    ASSERT_TRUE(params);
    ASSERT_EQ(params->find("scenes")->size(), 1u);
    EXPECT_EQ(params->find("scenes")->items().front().asString() ? *params->find("scenes")->items().front().asString()
                                                                 : "",
              selected.front());
    auto bad = runEngineSceneTests(io::realFileSystem(), launcher, f.options, *listing,
                                   std::vector<std::string>{"Assets/Tests/Unknown.fscene"}, [] { return true; });
    ASSERT_TRUE(bad.error);
    EXPECT_EQ(launcher.launches().size(), 2u);
}
TEST(EngineTestProvider, AllScenesUseOneNativeBatchAndRefuseIncompleteBatchReports) {
    test::EngineTestFixture f;
    auto listing = parseEngineTestListing(f.listingJson());
    ASSERT_TRUE(listing);
    io::ScriptedProcessLauncher launcher;
    auto report = test::engineSceneReport("Assets/Tests/A.fscene", true);
    const auto failing = test::engineSceneReport("Assets/Tests/B.fscene", false);
    report.set("passed", false)
        .set("scenes", Json::array({report.find("scenes")->items().front(), failing.find("scenes")->items().front()}))
        .set("totals", Json::object().set("scenes", 2).set("passedScenes", 1).set("failedScenes", 1));
    f.queue(launcher, "test.run", report, 3);
    auto run = runEngineSceneTests(io::realFileSystem(), launcher, f.options, *listing, {}, [] { return true; });
    ASSERT_FALSE(run.error);
    ASSERT_EQ(run.results.size(), 2u);
    ASSERT_EQ(launcher.launches().size(), 1u);
    EXPECT_EQ(run.results[0].scene, listing->tests[0].scene);
    EXPECT_EQ(run.results[1].scene, listing->tests[1].scene);
    EXPECT_EQ(failedEngineSceneTests(*listing, run.results), (std::vector<std::string>{"Assets/Tests/B.fscene"}));
    auto args = launcher.launches().front().spec.arguments;
    auto atParams = std::ranges::find(args, "--params");
    ASSERT_NE(atParams, args.end());
    ASSERT_NE(++atParams, args.end());
    auto params = rpc::parseJson(*atParams);
    ASSERT_TRUE(params);
    ASSERT_EQ(params->find("scenes")->size(), listing->tests.size());
    for (std::size_t i = 0; i < listing->tests.size(); ++i)
        EXPECT_EQ(*params->find("scenes")->items()[i].asString(), listing->tests[i].scene);
    f.queue(launcher, "test.run", test::engineSceneReport("Assets/Tests/A.fscene", true));
    auto incomplete = runEngineSceneTests(io::realFileSystem(), launcher, f.options, *listing, {}, [] { return true; });
    ASSERT_TRUE(incomplete.error);
    EXPECT_TRUE(incomplete.results.empty());
}
TEST(EngineTestProvider, CancellationRefusalMissingToolAndMalformedReportsNeverInventResults) {
    test::EngineTestFixture f;
    auto listing = parseEngineTestListing(f.listingJson());
    ASSERT_TRUE(listing);
    io::ScriptedProcessLauncher launcher;
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = runEngineSceneTests(
        io::realFileSystem(), launcher, f.options, *listing, {}, [] { return true; }, stop.get_token());
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_TRUE(cancelled.results.empty());
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_EQ(discoverEngineSceneTests(io::realFileSystem(), launcher, f.options, [] { return false; }).error().code(),
              ErrorCode::Untrusted);
    auto options = f.options;
    options.program = f.dir.join("Missing.exe");
    EXPECT_EQ(discoverEngineSceneTests(io::realFileSystem(), launcher, options, [] { return true; }).error().code(),
              ErrorCode::NotFound);
    io::ScriptedProcess broken;
    broken.stdoutBytes = "malformed";
    broken.exitCode = 1;
    launcher.expect(broken);
    auto run = runEngineSceneTests(io::realFileSystem(), launcher, f.options, *listing, {}, [] { return true; });
    ASSERT_TRUE(run.error);
    EXPECT_TRUE(run.results.empty());
    EXPECT_TRUE(failedEngineSceneTests(*listing, run.results).empty());
}
class ObservedLauncher final : public io::IProcessLauncher {
public:
    io::ScriptedProcessLauncher scripted;
    std::function<void(std::size_t)> afterLaunch;
    std::size_t count = 0;
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        auto p = scripted.launch(spec);
        if (p && afterLaunch)
            afterLaunch(++count);
        return p;
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec&, const io::AsyncContext&) override {
        return fail(ErrorCode::Unsupported, "synchronous test observer");
    }
};
TEST(EngineTestProvider, CancelsOwnedCurrentBatchWithoutInventedPartialReports) {
    test::EngineTestFixture f;
    auto listing = parseEngineTestListing(f.listingJson());
    ASSERT_TRUE(listing);
    ObservedLauncher launcher;
    std::stop_source stop;
    io::ScriptedProcess hung;
    hung.neverExits = true;
    launcher.scripted.expect(hung);
    launcher.afterLaunch = [&](std::size_t) { stop.request_stop(); };
    auto run = runEngineSceneTests(
        io::realFileSystem(), launcher, f.options, *listing, {}, [] { return true; }, stop.get_token());
    ASSERT_TRUE(run.cancelled);
    ASSERT_TRUE(run.error);
    EXPECT_TRUE(run.results.empty());
    auto launches = launcher.scripted.launches();
    ASSERT_EQ(launches.size(), 1u);
    EXPECT_TRUE(launches.back().terminated);
}
TEST(EngineTestProvider, RevokedAdmissionDiscardsJustFinishedReport) {
    test::EngineTestFixture f;
    auto listing = parseEngineTestListing(f.listingJson());
    ASSERT_TRUE(listing);
    ObservedLauncher launcher;
    bool admitted = true;
    launcher.afterLaunch = [&](std::size_t) { admitted = false; };
    f.queue(launcher.scripted, "test.run", test::engineSceneReport("Assets/Tests/A.fscene", true));
    auto run = runEngineSceneTests(io::realFileSystem(), launcher, f.options, *listing, {}, [&] { return admitted; });
    ASSERT_TRUE(run.error);
    EXPECT_EQ(run.error->code(), ErrorCode::Untrusted);
    EXPECT_TRUE(run.results.empty());
    EXPECT_EQ(launcher.scripted.launches().size(), 1u);
}
TEST(EngineTestProvider, ChangedPhysicalProjectAndEscapingLocationsAreRefused) {
    test::EngineTestFixture f;
    io::ScriptedProcessLauncher launcher;
    auto options = f.options;
    options.expectedProjectRoot = f.dir.join("Assets");
    EXPECT_EQ(discoverEngineSceneTests(io::realFileSystem(), launcher, options, [] { return true; }).error().code(),
              ErrorCode::PermissionDenied);
    EXPECT_TRUE(launcher.launches().empty());
    auto loc =
        resolveEngineTestLocation(io::realFileSystem(), f.options.expectedProjectRoot, {"Assets/Scripts/Test.lua", 4});
    ASSERT_TRUE(loc);
    EXPECT_FALSE(resolveEngineTestLocation(io::realFileSystem(), f.options.expectedProjectRoot,
                                           {"Assets/../../Outside.lua", 4}));
    EXPECT_FALSE(
        resolveEngineTestLocation(io::realFileSystem(), f.options.expectedProjectRoot, {"Assets/Scripts/Test.lua", 0}));
}
TEST(EngineTestProvider, DiscoveryProgramPhysicalReceiptRejectsRetargetBeforeAnyChild) {
    test::EngineTestFixture f;
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(f.dir.join("first")));
    ASSERT_TRUE(fs.createDirectories(f.dir.join("second")));
    ASSERT_TRUE(fs.writeFileAtomic(f.dir.join("first/CLI"), "same"));
    ASSERT_TRUE(fs.writeFileAtomic(f.dir.join("second/CLI"), "same"));
    const auto alias = f.dir.join("tools");
    ASSERT_NO_FATAL_FAILURE(test::makeDirectoryLink(alias, f.dir.join("first")));
    f.options.program = io::joinPath(alias, "CLI").value();
    f.options.expectedProgram = fs.physicalPath(f.options.program).value();
    ASSERT_TRUE(fs.removeFile(alias));
    ASSERT_NO_FATAL_FAILURE(test::makeDirectoryLink(alias, f.dir.join("second")));
    io::ScriptedProcessLauncher launcher;
    const auto refused = discoverEngineSceneTests(fs, launcher, f.options, [] { return true; });
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(EngineTestProvider, RealAssetsAliasEscapeRefusesStructuredNavigation) {
    test::EngineTestFixture f;
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(f.dir.join("outside")));
    ASSERT_TRUE(fs.writeFileAtomic(f.dir.join("outside/Test.lua"), "return true"));
    const auto link = f.dir.join("Assets/Escape");
    ASSERT_NO_FATAL_FAILURE(test::makeDirectoryLink(link, f.dir.join("outside")));
    EXPECT_FALSE(resolveEngineTestLocation(fs, f.options.expectedProjectRoot, {"Assets/Escape/Test.lua", 1}));
    EXPECT_TRUE(resolveEngineTestLocation(fs, f.options.expectedProjectRoot, {"Assets/Scripts/Test.lua", 1}));
}

// Deterministic IFileSystem interleaving oracle, supplementary to real Windows/POSIX alias tests.
class ListingCheckpointFs final : public io::IFileSystem {
public:
    io::IFileSystem& real = io::realFileSystem();
    std::string alias, selected, root;
    unsigned rootResolutions = 0;
    std::function<void()> beforeListingIdentity;
    Result<std::string> readFile(std::string_view p) override { return real.readFile(p); }
    Result<void> writeFileAtomic(std::string_view p, std::string_view b) override { return real.writeFileAtomic(p, b); }
    Result<io::FileStatus> status(std::string_view p) override { return real.status(p); }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view p) override { return real.listDirectory(p); }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view p) override { return real.listDirectoryWithDiagnostics(p); }
    Result<void> createDirectories(std::string_view p) override { return real.createDirectories(p); }
    Result<void> removeFile(std::string_view p) override { return real.removeFile(p); }
    Result<void> removeDirectory(std::string_view p) override { return real.removeDirectory(p); }
    Result<bool> exists(std::string_view p) override { return real.exists(p); }
    Result<std::string> canonicalPath(std::string_view p) override { return real.canonicalPath(p); }
    Result<std::string> physicalPath(std::string_view p) override {
        if (lang::documentKey(p) == lang::documentKey(root) && ++rootResolutions == 3 && beforeListingIdentity)
            beforeListingIdentity();
        return real.physicalPath(p == alias ? selected : p);
    }
};

TEST(EngineTestProvider, ListingIdentityUsesImmutableCommandTargetAfterPostCommandCheckpoint) {
    test::EngineTestFixture f;
    auto& real = io::realFileSystem();
    ASSERT_TRUE(real.writeFileAtomic(f.dir.join("Other.frostproj"), "{}"));
    ListingCheckpointFs fs;
    fs.root = f.options.expectedProjectRoot;
    fs.alias = f.dir.join("Selected.frostproj");
    fs.selected = *f.options.expectedProjectFile;
    const auto first = real.physicalPath(fs.selected).value();
    const auto second = real.physicalPath(f.dir.join("Other.frostproj")).value();
    ASSERT_NE(lang::documentKey(first), lang::documentKey(second));
    ASSERT_EQ(lang::documentKey(io::parentPath(first)), lang::documentKey(io::parentPath(second)));
    f.options.project = fs.alias;
    f.options.expectedProjectFile = fs.alias;
    auto reply = f.listingJson();
    reply.set("projectFile", fs.alias); // A legal structured lexical response spelling.
    bool retargeted = false;
    fs.beforeListingIdentity = [&] { fs.selected = second; retargeted = true; };
    io::ScriptedProcessLauncher launcher;
    f.queue(launcher, "test.list", reply);
    auto result = discoverEngineSceneTests(fs, launcher, f.options, [] { return true; });
    EXPECT_TRUE(retargeted);
    EXPECT_EQ(fs.rootResolutions, 3U); // AFTER command pre/post admission, BEFORE response file resolution.
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    ASSERT_EQ(launcher.launches().size(), 1U);
    const auto arguments = launcher.launches().front().spec.arguments;
    EXPECT_NE(std::ranges::find(arguments, first), arguments.end()); // Child ran with the first admitted file.
    EXPECT_TRUE(real.exists(first).value());
    EXPECT_TRUE(real.exists(second).value());
    // Selecting a DIRECTORY legitimately lets the engine report its contained project file.
    f.options.project = fs.root;
    f.options.expectedProjectFile.reset();
    reply.set("projectFile", second);
    f.queue(launcher, "test.list", reply);
    EXPECT_TRUE(discoverEngineSceneTests(real, launcher, f.options, [] { return true; }));
}

} // namespace
} // namespace frost::engine
