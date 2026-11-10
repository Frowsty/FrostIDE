// SPDX-License-Identifier: Apache-2.0
//
// CLANG-0 end-to-end tests for frost-clang (PLAN.md Appendix C, ADR 0083):
//   - FeatureModule linked into clangdMain built against the /MT LLVM 23.1.4 SDK answers custom
//     LSP requests ("frost/clangInfo" and "frost/spikePing") over stdio.
//   - LibTooling AST symbol rewriter ("--rewrite-symbol") rewrites declarations, references, types
//     and destructors in C++ fixtures while preserving comments and string literals, and the
//     rewritten C++ source compiles cleanly with the pinned Clang compiler.

#include "frost/core/clock.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/framing.hpp"
#include "frost/rpc/json.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::rpc::FrameDecoder;
using frost::rpc::Json;

constexpr auto kProcessTimeout = 60s;

std::string frostClangPath() {
    auto normalized = frost::io::normalizePath(FROST_CLANG_BINARY);
    EXPECT_TRUE(normalized.has_value());
    return normalized ? *normalized : std::string(FROST_CLANG_BINARY);
}

std::string pinnedCompilerPath() {
    auto normalized = frost::io::normalizePath(FROST_TEST_CXX_COMPILER);
    EXPECT_TRUE(normalized.has_value());
    return normalized ? *normalized : std::string(FROST_TEST_CXX_COMPILER);
}

bool sendLspMessage(frost::io::IProcess& proc, const Json& message) {
    const std::string framed = frost::rpc::encodeFrame(message.dump());
    const auto written = proc.writeStdin(framed);
    EXPECT_TRUE(written.has_value()) << (written ? "" : written.error().describe());
    return written.has_value();
}

std::optional<Json> readResponseWithId(frost::io::IProcess& proc, FrameDecoder& decoder, std::int64_t expectedId) {
    std::array<char, 4096> chunk{};
    while (true) {
        while (auto nextFrame = decoder.next()) {
            if (!nextFrame->has_value()) {
                ADD_FAILURE() << "frame decode error: " << nextFrame->error().describe();
                return std::nullopt;
            }
            auto parsed = frost::rpc::parseJson(**nextFrame);
            if (!parsed) {
                ADD_FAILURE() << "JSON parse error: " << parsed.error().describe();
                return std::nullopt;
            }
            if (parsed->integerAt("id") == expectedId) {
                return std::move(*parsed);
            }
        }
        auto n = proc.readStdout(chunk);
        if (!n) {
            ADD_FAILURE() << "readStdout failed: " << n.error().describe();
            return std::nullopt;
        }
        if (*n == 0) {
            ADD_FAILURE() << "unexpected EOF waiting for JSON-RPC id " << expectedId;
            return std::nullopt;
        }
        decoder.feed(std::string_view(chunk.data(), *n));
    }
}

void verifyCompilesWithPinnedClang(const std::string& sourcePath, const std::string& workingDir) {
    frost::io::ProcessSpec spec;
    spec.executable = pinnedCompilerPath();
    spec.workingDirectory = workingDir;
#ifdef _WIN32
    spec.arguments = {"/Zs", "/std:c++20", "/EHsc", sourcePath};
#else
    spec.arguments = {"-fsyntax-only", "-std=c++20", sourcePath};
#endif
    auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, "", kProcessTimeout);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0) << "stdout:\n" << result->stdoutBytes << "\nstderr:\n" << result->stderrBytes;
}

TEST(ClangSpike, FeatureModuleAnswersCustomLspRequests) {
    frost::test::ScratchDir scratch("clang-lsp-spike");
    ASSERT_FALSE(scratch.path().empty());

    frost::io::ProcessSpec spec;
    spec.executable = frostClangPath();
    spec.workingDirectory = scratch.path();
    spec.arguments = {"--log=error"};
    spec.stdinMode = frost::io::Stdio::Pipe;
    spec.stdoutMode = frost::io::Stdio::Pipe;
    spec.stderrMode = frost::io::Stdio::Null;

    auto launched = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(launched.has_value()) << launched.error().describe();
    frost::io::IProcess& proc = **launched;
    struct ProcessGuard {
        frost::io::IProcess& process;
        bool exited = false;
        ~ProcessGuard() {
            if (!exited) {
                process.closeStdin();
                (void)process.terminate();
                (void)process.wait(std::chrono::seconds(5));
            }
        }
    } guard{proc};

    FrameDecoder decoder;

    // 1. Send LSP initialize request.
    Json initReq =
        Json::object()
            .set("jsonrpc", "2.0")
            .set("id", 1)
            .set("method", "initialize")
            .set("params",
                 Json::object().set("processId", Json()).set("rootUri", Json()).set("capabilities", Json::object()));
    ASSERT_TRUE(sendLspMessage(proc, initReq));

    auto initResp = readResponseWithId(proc, decoder, 1);
    ASSERT_TRUE(initResp.has_value());
    const Json* initResult = initResp->find("result");
    ASSERT_NE(initResult, nullptr);
    const Json* serverCaps = initResult->find("capabilities");
    ASSERT_NE(serverCaps, nullptr);
    const Json* featureCap = serverCaps->find("frostClangFeatureModule");
    ASSERT_NE(featureCap, nullptr);
    EXPECT_EQ(featureCap->stringAt("engine"), "frost-clang");
    EXPECT_EQ(featureCap->stringAt("llvmVersion"), "23.1.4");

    // 2. Send initialized notification.
    Json initializedNotif =
        Json::object().set("jsonrpc", "2.0").set("method", "initialized").set("params", Json::object());
    ASSERT_TRUE(sendLspMessage(proc, initializedNotif));

    // 3. Send custom frost/clangInfo request handled by FrostFeatureModule.
    Json infoReq = Json::object()
                       .set("jsonrpc", "2.0")
                       .set("id", 2)
                       .set("method", "frost/clangInfo")
                       .set("params", Json::object().set("ping", "hello-from-frost-ide"));
    ASSERT_TRUE(sendLspMessage(proc, infoReq));

    auto infoResp = readResponseWithId(proc, decoder, 2);
    ASSERT_TRUE(infoResp.has_value());
    const Json* infoResult = infoResp->find("result");
    ASSERT_NE(infoResult, nullptr);
    EXPECT_EQ(infoResult->stringAt("engine"), "frost-clang");
    EXPECT_EQ(infoResult->stringAt("llvmVersion"), "23.1.4");
    EXPECT_EQ(infoResult->boolAt("featureModule"), true);
    EXPECT_EQ(infoResult->stringAt("moduleName"), "frost-clang");
    EXPECT_EQ(infoResult->stringAt("echo"), "hello-from-frost-ide");
#ifdef _WIN32
    EXPECT_EQ(infoResult->stringAt("crtLinkage"), "MT");
#else
    EXPECT_EQ(infoResult->stringAt("crtLinkage"), "static-llvm");
#endif

    // 4. Send custom frost/spikePing request to verify the alias method and query echo.
    Json pingReq = Json::object()
                       .set("jsonrpc", "2.0")
                       .set("id", 3)
                       .set("method", "frost/spikePing")
                       .set("params", Json::object().set("query", "mt-static-sdk-ok"));
    ASSERT_TRUE(sendLspMessage(proc, pingReq));

    auto pingResp = readResponseWithId(proc, decoder, 3);
    ASSERT_TRUE(pingResp.has_value());
    const Json* pingResult = pingResp->find("result");
    ASSERT_NE(pingResult, nullptr);
    EXPECT_EQ(pingResult->boolAt("featureModule"), true);
    EXPECT_EQ(pingResult->stringAt("echo"), "mt-static-sdk-ok");

    // 5. Clean LSP shutdown + exit.
    Json shutdownReq = Json::object().set("jsonrpc", "2.0").set("id", 4).set("method", "shutdown");
    ASSERT_TRUE(sendLspMessage(proc, shutdownReq));
    auto shutdownResp = readResponseWithId(proc, decoder, 4);
    ASSERT_TRUE(shutdownResp.has_value());

    Json exitNotif = Json::object().set("jsonrpc", "2.0").set("method", "exit");
    ASSERT_TRUE(sendLspMessage(proc, exitNotif));
    proc.closeStdin();

    auto exitStatus = proc.wait(kProcessTimeout);
    guard.exited = true;
    ASSERT_TRUE(exitStatus.has_value()) << exitStatus.error().describe();
    EXPECT_EQ(exitStatus->exitCode, 0);
}

TEST(ClangSpike, LibToolingRewritesAstSymbolLeavingCommentsAndStringsIntact) {
    frost::test::ScratchDir scratch("clang-rewrite-fn");
    ASSERT_FALSE(scratch.path().empty());

    const std::string sourcePath = scratch.join("fixture.cpp");
    ASSERT_FALSE(sourcePath.empty());

    constexpr std::string_view kFixtureSource =
        R"cpp(// Comment: computeLegacyMetric must stay computeLegacyMetric in comments.
namespace demo {

int computeLegacyMetricHelper(int v) {
    return v + 1;
}

int computeLegacyMetric(int seed) {
    return computeLegacyMetricHelper(seed) * 2;
}

int runComputation() {
    const char* label = "computeLegacyMetric in string literal";
    (void)label;
    const int first = computeLegacyMetric(10);
    const int second = demo::computeLegacyMetric(20);
    return first + second;
}

} // namespace demo
)cpp";

    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(sourcePath, kFixtureSource).has_value());

    frost::io::ProcessSpec rewriteSpec;
    rewriteSpec.executable = frostClangPath();
    rewriteSpec.workingDirectory = scratch.path();
    rewriteSpec.arguments = {
        "--rewrite-symbol", "computeLegacyMetric", "calculateModernMetric", sourcePath, "--", "-std=c++20",
    };

    auto rewriteResult = frost::io::runToCompletion(frost::io::realProcessLauncher(), rewriteSpec, "", kProcessTimeout);
    ASSERT_TRUE(rewriteResult.has_value()) << rewriteResult.error().describe();
    ASSERT_EQ(rewriteResult->exitCode, 0) << "stderr:\n" << rewriteResult->stderrBytes;

    auto diskBytes = frost::io::realFileSystem().readFile(sourcePath);
    ASSERT_TRUE(diskBytes.has_value()) << diskBytes.error().describe();
    EXPECT_EQ(*diskBytes, rewriteResult->stdoutBytes);

    // Comments, strings and longer identifiers must remain untouched.
    EXPECT_NE(diskBytes->find("// Comment: computeLegacyMetric must stay computeLegacyMetric in comments."),
              std::string::npos);
    EXPECT_NE(diskBytes->find("\"computeLegacyMetric in string literal\""), std::string::npos);
    EXPECT_NE(diskBytes->find("int computeLegacyMetricHelper(int v)"), std::string::npos);

    // The declaration and both call sites must be rewritten.
    EXPECT_NE(diskBytes->find("int calculateModernMetric(int seed)"), std::string::npos);
    EXPECT_NE(diskBytes->find("const int first = calculateModernMetric(10);"), std::string::npos);
    EXPECT_NE(diskBytes->find("const int second = demo::calculateModernMetric(20);"), std::string::npos);
    EXPECT_EQ(diskBytes->find("int computeLegacyMetric("), std::string::npos);

    verifyCompilesWithPinnedClang(sourcePath, scratch.path());
}

TEST(ClangSpike, LibToolingRewritesClassTypeAndMembersAndCompiles) {
    frost::test::ScratchDir scratch("clang-rewrite-type");
    ASSERT_FALSE(scratch.path().empty());

    const std::string sourcePath = scratch.join("widget.cpp");
    ASSERT_FALSE(sourcePath.empty());

    constexpr std::string_view kWidgetFixture = R"cpp(namespace ui {

struct LegacyCounter {
    int legacyValue = 0;

    explicit LegacyCounter(int initial) : legacyValue(initial) {}
    ~LegacyCounter() = default;

    int readValue() const {
        return this->legacyValue;
    }
};

int useCounter(const LegacyCounter& counter) {
    LegacyCounter localCopy(counter.legacyValue + 5);
    return localCopy.readValue();
}

} // namespace ui
)cpp";

    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(sourcePath, kWidgetFixture).has_value());

    // First rewrite the struct type name LegacyCounter -> ModernCounter.
    {
        frost::io::ProcessSpec spec;
        spec.executable = frostClangPath();
        spec.workingDirectory = scratch.path();
        spec.arguments = {
            "--rewrite-symbol", "LegacyCounter", "ModernCounter", sourcePath, "--", "-std=c++20",
        };
        auto res = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, "", kProcessTimeout);
        ASSERT_TRUE(res.has_value()) << res.error().describe();
        ASSERT_EQ(res->exitCode, 0) << "stderr:\n" << res->stderrBytes;
    }

    // Next rewrite the field name legacyValue -> modernValue.
    {
        frost::io::ProcessSpec spec;
        spec.executable = frostClangPath();
        spec.workingDirectory = scratch.path();
        spec.arguments = {
            "--rewrite-symbol", "legacyValue", "modernValue", sourcePath, "--", "-std=c++20",
        };
        auto res = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, "", kProcessTimeout);
        ASSERT_TRUE(res.has_value()) << res.error().describe();
        ASSERT_EQ(res->exitCode, 0) << "stderr:\n" << res->stderrBytes;
    }

    auto rewritten = frost::io::realFileSystem().readFile(sourcePath);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().describe();
    EXPECT_NE(rewritten->find("struct ModernCounter"), std::string::npos);
    EXPECT_NE(rewritten->find("explicit ModernCounter(int initial) : modernValue(initial) {}"), std::string::npos);
    EXPECT_NE(rewritten->find("~ModernCounter() = default;"), std::string::npos);
    EXPECT_NE(rewritten->find("return this->modernValue;"), std::string::npos);
    EXPECT_NE(rewritten->find("int useCounter(const ModernCounter& counter)"), std::string::npos);
    EXPECT_NE(rewritten->find("ModernCounter localCopy(counter.modernValue + 5);"), std::string::npos);
    EXPECT_EQ(rewritten->find("LegacyCounter"), std::string::npos);
    EXPECT_EQ(rewritten->find("legacyValue"), std::string::npos);

    verifyCompilesWithPinnedClang(sourcePath, scratch.path());
}

TEST(ClangSpike, LibToolingFailsCleanlyOnMissingSymbol) {
    frost::test::ScratchDir scratch("clang-rewrite-missing");
    ASSERT_FALSE(scratch.path().empty());

    const std::string sourcePath = scratch.join("unchanged.cpp");
    ASSERT_FALSE(sourcePath.empty());
    constexpr std::string_view kSource = "int presentSymbol() { return 42; }\n";
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(sourcePath, kSource).has_value());

    frost::io::ProcessSpec spec;
    spec.executable = frostClangPath();
    spec.workingDirectory = scratch.path();
    spec.arguments = {
        "--rewrite-symbol", "nonExistentSymbol", "replacementSymbol", sourcePath, "--", "-std=c++20",
    };

    auto res = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, "", kProcessTimeout);
    ASSERT_TRUE(res.has_value()) << res.error().describe();
    EXPECT_NE(res->exitCode, 0);

    auto after = frost::io::realFileSystem().readFile(sourcePath);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*after, kSource);
}

} // namespace
