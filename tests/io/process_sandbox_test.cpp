// SPDX-License-Identifier: Apache-2.0
// Checks process sandbox profile validation and path access boundaries.
#include "frost/io/process_sandbox.hpp"
#include <gtest/gtest.h>
using namespace frost;
using namespace frost::io;
namespace {
ProcessSpec profile(bool windows) {
    ProcessSpec spec; spec.executable = windows ? "C:/payload/tool.exe" : "/payload/tool";
    spec.cleanEnvironment = true; spec.stdinMode = Stdio::Pipe;
    spec.sandbox = ProcessSandboxProfile{};
    spec.sandbox->paths.push_back({spec.executable, SandboxAccess::Read | SandboxAccess::Execute, false});
    if (windows) { spec.sandbox->windowsStagingRoot = "C:/payload"; spec.sandbox->windowsAppContainerName = "frost.extension.fixture"; }
    return spec;
}
TEST(ProcessSandbox, DefaultProcessRemainsUnrestrictedAndExplicitProfilesRejectAmbientLaunchFeatures) {
    ProcessSpec ordinary; ordinary.executable = "/tool"; EXPECT_TRUE(validateProcessSandbox(ordinary, false));
    for (bool windows : {false, true}) {
        auto spec = profile(windows); ASSERT_TRUE(validateProcessSandbox(spec, windows));
        spec.killOnClose = false; EXPECT_FALSE(validateProcessSandbox(spec, windows)); spec.killOnClose = true;
        spec.cleanEnvironment = false; EXPECT_FALSE(validateProcessSandbox(spec, windows)); spec.cleanEnvironment = true;
        spec.stdinMode = Stdio::Inherit; EXPECT_FALSE(validateProcessSandbox(spec, windows)); spec.stdinMode = Stdio::Pipe;
        spec.posixInheritedFds.push_back(3); EXPECT_FALSE(validateProcessSandbox(spec, windows)); spec.posixInheritedFds.clear();
        spec.rawArguments = "anything"; EXPECT_FALSE(validateProcessSandbox(spec, windows));
    }
}
TEST(ProcessSandbox, ExactRightsAndComponentBoundariesNeverGrantSiblingOrParentContent) {
    ProcessSandboxProfile p; p.paths = {{"/payload", SandboxAccess::Read, true}, {"/tool", SandboxAccess::Read | SandboxAccess::Execute, false}};
    EXPECT_TRUE(sandboxAllowsPath(p, "/payload/a", SandboxAccess::Read, PathStyle::Posix));
    EXPECT_FALSE(sandboxAllowsPath(p, "/payload-other/a", SandboxAccess::Read, PathStyle::Posix));
    EXPECT_FALSE(sandboxAllowsPath(p, "/payload/../secret", SandboxAccess::Read, PathStyle::Posix));
    EXPECT_FALSE(sandboxAllowsPath(p, "/payload/a", SandboxAccess::Write, PathStyle::Posix));
    EXPECT_FALSE(sandboxAllowsPath(p, "/tool/child", SandboxAccess::Execute, PathStyle::Posix));
    p.paths = {{"C:/payload", SandboxAccess::Read, true}};
    EXPECT_TRUE(sandboxAllowsPath(p, "c:\\payload\\a", SandboxAccess::Read, PathStyle::Windows));
    EXPECT_FALSE(sandboxAllowsPath(p, "C:/payload-other/a", SandboxAccess::Read, PathStyle::Windows));
}
TEST(ProcessSandbox, UnpreparedWindowsAndUnknownRightsCannotSilentlyBecomeOrdinaryProcesses) {
    auto spec = profile(true); spec.sandbox->windowsAppContainerName.clear(); EXPECT_FALSE(validateProcessSandbox(spec, true));
    spec = profile(true); spec.sandbox->paths.push_back({"C:/project/secret", SandboxAccess::Read, false});
    EXPECT_FALSE(validateProcessSandbox(spec, true));
    spec = profile(false); spec.sandbox->paths.front().access = SandboxAccess::Read;
    EXPECT_FALSE(validateProcessSandbox(spec, false));
    spec.sandbox->paths.front().access = static_cast<SandboxAccess>(128); EXPECT_FALSE(validateProcessSandbox(spec, false));
}
}
