// SPDX-License-Identifier: Apache-2.0
// Mandatory FER-8/LUA-11 real engine acceptance. No optional executable/environment skip exists.
// Compile this source for the locked Debug, Release and Dist engine presets; never time Debug.
#ifdef FROST_LUA_REAL_ENGINE_PRESET
#undef FROST_CORPUS_FROSTENGINE_PRESET
#define FROST_CORPUS_FROSTENGINE_PRESET FROST_LUA_REAL_ENGINE_PRESET
#endif
#include "support/real_engine_session.hpp"
#include "support/real_lua_player.hpp"
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#if !defined(FROST_LUA_REAL_DRIVER) || !defined(FROST_LUA_REAL_FIXTURES) || !defined(FROST_LUA_REAL_CATALOGUE) ||      \
    !defined(FROST_LUA_REAL_MODE) || !defined(FROST_LUA_REAL_NM) || !defined(FROST_LUA_REAL_RELEASE_PRESET)
#error "R7 real Lua acceptance requires the driver, fixtures, catalogue, mode, symbol tool and Release preset"
#endif
#if (defined(FROST_LUA_REAL_CONFORMANCE) + defined(FROST_LUA_REAL_RELEASE) + defined(FROST_LUA_REAL_DIST)) != 1
#error "Select exactly one real Lua acceptance gate"
#endif

namespace frost::debug {
namespace {
using namespace std::chrono_literals;
using rpc::Json;
constexpr std::array<std::string_view, 4> kDebugClasses{"ScriptDebugger", "ScriptDebugAdapter", "ScriptDebugHost",
                                                        "DapFrameDecoder"};

class RealLuaCorpus : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(io::realFileSystem().exists(FROST_TEST_PYTHON).value_or(false)) << "pinned Python is missing";
        ASSERT_TRUE(io::realFileSystem().exists(FROST_LUA_REAL_DRIVER).value_or(false)) << "real driver is missing";
        auto prepared = build.prepare({"FrostEditor", "FrostRuntime"});
        ASSERT_TRUE(prepared) << prepared.error().describe();
        buildDirectory = build.corpus + "/build/" + FROST_CORPUS_FROSTENGINE_PRESET;
        const auto cache = io::realFileSystem().readFile(buildDirectory + "/CMakeCache.txt");
        ASSERT_TRUE(cache) << "real engine preset is not configured; provision its mandatory corpus first";
        const std::string mode = FROST_LUA_REAL_MODE;
#if defined(FROST_LUA_REAL_CONFORMANCE)
        ASSERT_EQ(mode, "conformance");
#elif defined(FROST_LUA_REAL_RELEASE)
        ASSERT_EQ(mode, "release");
#else
        ASSERT_EQ(mode, "dist");
#endif
        buildType = mode == "release" ? "Release" : mode == "dist" ? "Dist" : "Debug";
        const auto type = "CMAKE_BUILD_TYPE:STRING=" + buildType;
        ASSERT_TRUE(cache->find(type + "\n") != std::string::npos || cache->find(type + "\r\n") != std::string::npos)
            << "actual engine cache does not identify the required build type";
        if (mode == "dist") {
            ASSERT_NE(cache->find("FROST_ENABLE_SCRIPT_DEBUGGER:BOOL=OFF"), std::string::npos)
                << "Dist must forcibly disable debugger activation";
        }
        project = scratch.join("project");
        auto copied = build.copyProject(project);
        ASSERT_TRUE(copied) << copied.error().describe();
    }

    Result<void> install(std::string_view filename) {
        auto source = io::realFileSystem().readFile(std::string(FROST_LUA_REAL_FIXTURES) + "/" + std::string(filename));
        if (!source)
            return std::unexpected(source.error());
        return test::installAcceptanceScript(project, *source);
    }

    Result<Json> run(std::string suite, Json extra = Json::object()) {
        test::RealEditor editor;
        test::RealLuaPlayer player;
        const bool initialPlayer = suite == "player";
        auto endpoint = initialPlayer ? player.start(build, project)
                                      : editor.start(build, project, scratch.join("config-" + suite));
        if (!endpoint)
            return std::unexpected(endpoint.error());
        auto image = io::toStdPath(initialPlayer ? player.image : build.program);
        if (!image)
            return std::unexpected(image.error());
#ifdef _WIN32
        // The console .com forwards to this native editor image. The authenticated OS pid must
        // resolve to this exact built .exe, not merely a program with an engine-looking name.
        if (image->extension() == ".com")
            image->replace_extension(".exe");
#endif
        extra.set("endpoint", Json::object()
                                  .set("port", endpoint->port)
                                  .set("projectPath", endpoint->projectPath)
                                  .set("instanceId", endpoint->instanceId)
                                  .set("generation", endpoint->generation)
                                  .set("secret", endpoint->secret));
        extra.set("engineImage", io::fromStdPath(*image).value_or(""));
        extra.set("pinnedEngineSha", FROST_CORPUS_FROSTENGINE_SHA);
        extra.set("catalogue", FROST_LUA_REAL_CATALOGUE);
        extra.set("buildType", buildType);
        io::ProcessSpec process;
        process.executable = io::normalizePath(FROST_TEST_PYTHON).value_or("");
        process.arguments = {"-B", FROST_LUA_REAL_DRIVER, "--suite", std::move(suite)};
        process.workingDirectory = scratch.path();
        process.stdinMode = io::Stdio::Pipe;
        // Credential-bearing inputs are confined to the pipe. The driver redacts all lease
        // credentials/proofs; never print raw startup, private input or arbitrary stderr here.
        auto completed = io::runToCompletion(io::realProcessLauncher(), process, extra.dump(), 30min);
        auto playerCleanup = player.stop();
        if (!playerCleanup)
            return fail(playerCleanup.error().code(),
                        "real player fixture failed contained cleanup or private-output check");
        if (!completed)
            return fail(completed.error().code(), "real Lua acceptance driver did not complete within its bound");
        auto result = rpc::parseJson(completed->stdoutBytes);
        if (!result || !result->isObject())
            return fail(ErrorCode::Corrupt, "real Lua acceptance driver emitted no bounded JSON verdict");
        if (completed->exitCode != 0 || result->boolAt("ok") != true)
            return fail(ErrorCode::IoError, "real Lua acceptance failed: " + result->dump());
        RecordProperty("realLuaEvidence", result->dump());
        std::cout << "R7_REAL_LUA_EVIDENCE " << result->dump() << '\n';
        return *result;
        // editor destructor terminates and reaps the entire owned process container on every exit.
    }

    Result<void> symbols(const std::string& directory, bool expected) {
        if (!io::realFileSystem().exists(FROST_LUA_REAL_NM).value_or(false))
            return fail(ErrorCode::NotFound, "mandatory pinned llvm-nm symbol oracle is missing");
        std::array<bool, kDebugClasses.size()> seen{};
        for (const std::string_view name : {"FrostSimulation", "FrostAutomation", "FrostFoundation"}) {
#ifdef _WIN32
            const std::string archive = directory + "/lib/" + std::string(name) + ".lib";
#else
            const std::string archive = directory + "/lib/lib" + std::string(name) + ".a";
#endif
            if (!io::realFileSystem().exists(archive).value_or(false))
                return fail(ErrorCode::NotFound, "mandatory actual engine static archive is missing: " + archive);
            io::ProcessSpec spec;
            spec.executable = FROST_LUA_REAL_NM;
            spec.arguments = {"--defined-only", "--demangle", archive};
            auto output = io::runToCompletion(io::realProcessLauncher(), spec, {}, 2min);
            if (!output || output->exitCode != 0)
                return fail(ErrorCode::IoError, "mandatory llvm-nm archive inspection failed");
            for (std::size_t i = 0; i < kDebugClasses.size(); ++i) {
                // Match actual demangled or decorated Frost class symbols, never archive/member
                // filenames (which may remain visible even when no implementation was linked).
                const std::string klass(kDebugClasses[i]);
                seen[i] = seen[i] || output->stdoutBytes.find("Frost::" + klass + "::") != std::string::npos ||
                          output->stdoutBytes.find("@" + klass + "@Frost@@") != std::string::npos;
            }
        }
        for (std::size_t i = 0; i < seen.size(); ++i)
            if (seen[i] != expected)
                return fail(ErrorCode::Corrupt, std::string(expected ? "Release positive symbol missing: "
                                                                     : "Dist contains forbidden debugger symbol: ") +
                                                    std::string(kDebugClasses[i]));
        return {};
    }

    Result<void> distCompilationExcludesDebugger() {
        const auto bytes = io::realFileSystem().readFile(buildDirectory + "/compile_commands.json");
        if (!bytes)
            return std::unexpected(bytes.error());
        const auto commands = rpc::parseJson(*bytes);
        if (!commands || !commands->isArray())
            return fail(ErrorCode::Corrupt, "Dist compile database is missing or malformed");
        bool inspectedEngine = false;
        for (const auto& command : commands->items()) {
            const auto path = io::toStdPath(command.stringAt("file"));
            if (!path)
                return std::unexpected(path.error());
            const auto filename = path->filename().string();
            if (filename.starts_with("ScriptDebug") || filename == "DapFrameDecoder.cpp")
                return fail(ErrorCode::Corrupt, "Dist compiled forbidden debugger implementation: " + filename);
            auto normalized = io::fromStdPath(*path).value_or("");
            if (normalized.find("/Frost/src/") == std::string::npos)
                continue;
            inspectedEngine = true;
            std::string arguments(command.stringAt("command"));
            if (const auto* array = command.find("arguments"); array && array->isArray())
                for (const auto& item : array->items()) {
                    if (!item.isString())
                        return fail(ErrorCode::Corrupt, "Dist compile argument is not a string");
                    arguments += " " + *item.asString();
                }
            if (arguments.find("FROST_ENABLE_SCRIPT_DEBUGGER=0") == std::string::npos ||
                arguments.find("FROST_ENABLE_SCRIPT_DEBUGGER=1") != std::string::npos)
                return fail(ErrorCode::Corrupt, "Dist engine translation unit did not compile with debugger disabled");
        }
        if (!inspectedEngine)
            return fail(ErrorCode::Corrupt, "Dist compile database contained no actual engine translation units");
        return {};
    }

    test::RealEngineBuild build;
    test::ScratchDir scratch{"lua-real"};
    std::string project, buildDirectory, buildType;
};

#if defined(FROST_LUA_REAL_CONFORMANCE)
TEST_F(RealLuaCorpus, AllThirtyCatalogueScenariosUseActualPinnedEditorFramesAndValues) {
    ASSERT_TRUE(install("Conformance.lua"));
    auto result = run("conformance");
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->unsignedAt("totalCount"), 30U);
    EXPECT_EQ(result->unsignedAt("passedCount"), 30U);
    EXPECT_EQ(result->unsignedAt("skippedCount"), 0U);
}

TEST_F(RealLuaCorpus, ActualPlayerInitialOnCreateWaitsForPrivateAuthenticatedPendingConfiguration) {
    ASSERT_TRUE(install("Conformance.lua"));
    auto result = run("player");
    ASSERT_TRUE(result) << result.error().describe();
}

TEST_F(RealLuaCorpus, AttachedInfiniteLuaLoopRetainsActualWatchdogDiagnostic) {
    ASSERT_TRUE(install("Watchdog.lua"));
    const auto oracleProject = scratch.join("watchdog-oracle");
    ASSERT_TRUE(build.copyProject(oracleProject));
    auto source = io::realFileSystem().readFile(std::string(FROST_LUA_REAL_FIXTURES) + "/Watchdog.lua");
    ASSERT_TRUE(source);
    const auto gate = source->find("self.armed = false");
    ASSERT_NE(gate, std::string::npos);
    source->replace(gate, std::string_view("self.armed = false").size(), "self.armed = true");
    ASSERT_TRUE(test::installAcceptanceScript(oracleProject, *source));
    auto oracle = build.cli("run", oracleProject, Json::object().set("frames", 1).set("headless", true));
    ASSERT_TRUE(oracle) << oracle.error().describe();
    auto result = run("watchdog", Json::object().set("watchdogOracle", *oracle));
    ASSERT_TRUE(result) << result.error().describe();
}

TEST_F(RealLuaCorpus, RealMinutePauseExcludesWatchdogAndNormalImGuiFrameReentry) {
    ASSERT_TRUE(install("Conformance.lua"));
    auto result = run("pause");
    ASSERT_TRUE(result) << result.error().describe();
}

TEST_F(RealLuaCorpus, PendingOnCreateMinutePauseAcknowledgesReceiptDetachAndExactOperationStop) {
    ASSERT_TRUE(install("Conformance.lua"));
    auto result = run("pending");
    ASSERT_TRUE(result) << result.error().describe();
}
#elif defined(FROST_LUA_REAL_RELEASE)
TEST_F(RealLuaCorpus, FrozenSameReleaseWorkloadWithFiveExecutedBreakpointsAddsAtMostTwentyFivePercent) {
    ASSERT_TRUE(install("FrozenWorkload.lua"));
    auto positive = symbols(buildDirectory, true);
    ASSERT_TRUE(positive) << positive.error().describe();
    auto result = run("overhead");
    ASSERT_TRUE(result) << result.error().describe();
}
#elif defined(FROST_LUA_REAL_DIST)
TEST_F(RealLuaCorpus, DistExcludesDebuggerCompilationSymbolsPortsAndActivationWithReleasePositiveControl) {
    auto positive = symbols(build.corpus + "/build/" + FROST_LUA_REAL_RELEASE_PRESET, true);
    ASSERT_TRUE(positive) << positive.error().describe();
    auto excluded = distCompilationExcludesDebugger();
    ASSERT_TRUE(excluded) << excluded.error().describe();
    auto absent = symbols(buildDirectory, false);
    ASSERT_TRUE(absent) << absent.error().describe();
    auto result = run("dist");
    ASSERT_TRUE(result) << result.error().describe();
}
#else
#error "Select exactly one FROST_LUA_REAL_CONFORMANCE, FROST_LUA_REAL_RELEASE or FROST_LUA_REAL_DIST gate"
#endif
} // namespace
} // namespace frost::debug
