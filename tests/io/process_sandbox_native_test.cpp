// SPDX-License-Identifier: Apache-2.0
// Future execution is opt-in: Windows creates only a scratch private LPAC generation; Linux
// needs an explicit runtime grant inventory so the test never silently grants /usr or /lib.
#include "frost/io/process_sandbox.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/async_process.hpp"
#include "process_env.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
#include <chrono>
#ifdef _WIN32
#include "frost/io/utf.hpp"
#include <windows.h>
#include <userenv.h>
#else
#include <cstdlib>
#endif
using namespace frost;
using namespace frost::io;
namespace {
#ifdef _WIN32
struct OwnProfile {
    std::string name;
    ~OwnProfile() { auto text = utf8ToUtf16(name); if (text && !text->empty()) DeleteAppContainerProfile(reinterpret_cast<const wchar_t*>(text->c_str())); }
};
#endif
TEST(NativeProcessSandbox, ProductionSyncAndAsyncRetainNativePolicy) {
#ifdef _WIN32
    const bool probeEnabled = GetEnvironmentVariableW(L"FROST_RUN_NATIVE_SANDBOX_PROBE", nullptr, 0) != 0;
#else
    const bool probeEnabled = std::getenv("FROST_RUN_NATIVE_SANDBOX_PROBE") != nullptr;
#endif
    if (!probeEnabled) GTEST_SKIP() << "Explicit native sandbox acceptance is disabled";
    test::ScratchDir scratch("sandbox-probe"); auto& fs = realFileSystem();
    const auto publicFile = *joinPath(scratch.path(), "public.txt"), secretFile = *joinPath(scratch.path(), "secret.txt");
    ASSERT_TRUE(fs.writeFileAtomic(publicFile, "public-fixture\n")); ASSERT_TRUE(fs.writeFileAtomic(secretFile, "private-fixture\n"));
    ProcessSpec spec; spec.executable = FROST_SANDBOX_TEST_CHILD; spec.arguments = {publicFile, secretFile}; spec.cleanEnvironment = true;
    spec.sandbox = ProcessSandboxProfile{};
    spec.sandbox->paths = {{spec.executable, SandboxAccess::Read | SandboxAccess::Execute, false}, {publicFile, SandboxAccess::Read, false}};
#ifdef _WIN32
    OwnProfile profile;
    auto image = fs.readFileSnapshot(spec.executable, 64 * 1024 * 1024); ASSERT_TRUE(image);
    const std::vector<SandboxStagingFile> files{{spec.executable, "probe.exe", *image, SandboxAccess::Read | SandboxAccess::Execute}, {publicFile, "public.txt", "public-fixture\n", SandboxAccess::Read}};
    auto staged = stageWindowsSandboxProcess(spec, files, scratch.path()); ASSERT_TRUE(staged) << staged.error().describe();
    spec = std::move(*staged); profile.name = spec.sandbox->windowsAppContainerName;
#else
    const auto* configured = std::getenv("FROST_SANDBOX_TEST_RUNTIME_FILES");
    if (!configured) GTEST_SKIP() << "Provide explicit semicolon-separated canonical runtime files (loader/libc/libstdc++)";
    std::string paths(configured); std::size_t begin = 0;
    while (begin < paths.size()) {
        const auto end = paths.find(';', begin); const auto path = paths.substr(begin, end == paths.npos ? end : end - begin);
        spec.sandbox->paths.push_back({path, SandboxAccess::Read | SandboxAccess::Execute, false});
        if (end == paths.npos) break;
        begin = end + 1;
    }
#endif
    const auto originalProfile = spec.sandbox->paths.size();
    auto sync = runToCompletion(realProcessLauncher(), spec, {}, std::chrono::seconds(10));
    ASSERT_TRUE(sync) << sync.error().describe(); EXPECT_EQ(sync->exitCode, 0) << sync->stderrBytes;
    EXPECT_NE(sync->stdoutBytes.find("secret=denied write=denied network=denied"), std::string::npos);
    test::AsyncEnv env; auto async = env.launch(spec); ASSERT_TRUE(async) << async.error().describe();
    auto result = env.run((*async)->communicate({}, std::chrono::seconds(10))); ASSERT_TRUE(result);
    EXPECT_EQ(result->exitCode, 0) << result->stderrBytes; EXPECT_EQ(spec.sandbox->paths.size(), originalProfile);
    EXPECT_EQ(*fs.readFile(publicFile), "public-fixture\n"); EXPECT_EQ(*fs.readFile(secretFile), "private-fixture\n");
}
TEST(NativeProcessSandbox, BothLaunchPathsRefuseInvalidExplicitProfileBeforeAnyChild) {
    ProcessSpec spec; spec.executable = FROST_SANDBOX_TEST_CHILD; spec.cleanEnvironment = true;
    spec.sandbox = ProcessSandboxProfile{}; // No executable grant; never launch as an ordinary process.
    EXPECT_FALSE(realProcessLauncher().launch(spec)); test::AsyncEnv env; EXPECT_FALSE(env.launch(spec));
}
}
