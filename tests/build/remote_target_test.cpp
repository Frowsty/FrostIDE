// SPDX-License-Identifier: Apache-2.0
// Pure R19 endpoint/mapping/bootstrap contracts; no SSH/WSL feature is executed by these tests.
#include "frost/build/remote_target.hpp"

#include <gtest/gtest.h>

#include <algorithm>
namespace frost::build {
namespace {
RemoteTarget target() {
    RemoteTarget result;
    result.id = "named-linux";
    result.name = "Explicit Linux";
    result.transport = RemoteTransport::Ssh;
    result.transportExecutable =
        io::kNativePathStyle == io::PathStyle::Windows ? "C:/Windows/System32/OpenSSH/ssh.exe" : "/usr/bin/ssh";
    result.host = "example.invalid";
    result.user = "builder";
    result.executionApproved = true;
    result.mappings.push_back(
        {io::kNativePathStyle == io::PathStyle::Windows ? "C:/project" : "/local/project", "/home/builder/source"});
    return result;
}
TEST(RemoteTargets, ApplicationRegistryRoundTripsWithoutDroppingEndpointApproval) {
    const auto configured = target();
    auto parsed = parseRemoteTargets(remoteTargetsJson({configured}));
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->size(), 1U);
    EXPECT_EQ(parsed->front(), configured);
    auto denied = configured;
    denied.executionApproved = false;
    EXPECT_FALSE(remoteTransportSpec(denied, "print('guard')"));
}
TEST(RemoteTargets, MapsExplicitBoundariesAndRefusesTraversalOrAmbiguousInverse) {
    auto configured = target();
    const auto local = io::joinPath(configured.mappings[0].localRoot, "src/With Case.cpp").value();
    auto remote = mapLocalToRemote(configured, local);
    ASSERT_TRUE(remote);
    EXPECT_EQ(*remote, "/home/builder/source/src/With Case.cpp");
    auto restored = mapRemoteToLocal(configured, *remote);
    ASSERT_TRUE(restored);
    EXPECT_EQ(*restored, local);
    EXPECT_FALSE(mapRemoteToLocal(configured, "/home/builder/source/../outside.cpp"));
    EXPECT_FALSE(mapLocalToRemote(configured, configured.mappings[0].localRoot + "/../outside.cpp"));
    if (io::kNativePathStyle == io::PathStyle::Windows) {
        EXPECT_FALSE(mapRemoteToLocal(configured, "/home/builder/source/..\\outside.cpp"));
        EXPECT_FALSE(mapRemoteToLocal(configured, "/home/builder/source/C:/outside.cpp"));
        EXPECT_FALSE(mapRemoteToLocal(configured, "/home/builder/source/directory./file.cpp"));
    }
    EXPECT_FALSE(mapRemoteToLocal(configured, "/home/builder/source-sibling/file.cpp"));
    configured.mappings.push_back({io::joinPath(configured.mappings[0].localRoot, "nested").value(), "/other"});
    EXPECT_FALSE(validateRemoteTarget(configured));
}
TEST(RemoteTargets, SshBootstrapContainsNoWorkloadShellInterpolationOrConfigurationHooks) {
    auto configured = target();
    auto spec = remoteTransportSpec(configured, "print('static guard')\n");
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->executable, configured.transportExecutable);
    EXPECT_NE(std::find(spec->arguments.begin(), spec->arguments.end(), "StrictHostKeyChecking=yes"),
              spec->arguments.end());
    EXPECT_NE(std::find(spec->arguments.begin(), spec->arguments.end(), "BatchMode=yes"), spec->arguments.end());
    EXPECT_EQ(spec->arguments[1], "-F");
    EXPECT_EQ(spec->arguments[2], "none");
    configured.host = "host; touch /tmp/canary";
    EXPECT_FALSE(remoteTransportSpec(configured, "pass"));
}
TEST(RemoteTargets, WslAlwaysUsesNamedDistributionAndStructuredExecArgv) {
    auto configured = target();
    configured.transport = RemoteTransport::Wsl;
    configured.host.clear();
    configured.user.clear();
    configured.distribution = "Ubuntu-24.04";
    auto spec = remoteTransportSpec(configured, "pass");
    ASSERT_TRUE(spec);
    ASSERT_GE(spec->arguments.size(), 4U);
    EXPECT_EQ(spec->arguments[0], "--distribution");
    EXPECT_EQ(spec->arguments[1], configured.distribution);
    EXPECT_EQ(spec->arguments[2], "--exec");
    configured.distribution.clear();
    EXPECT_FALSE(remoteTransportSpec(configured, "pass"));
}
} // namespace
} // namespace frost::build
