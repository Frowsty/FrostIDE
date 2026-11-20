// SPDX-License-Identifier: Apache-2.0
// Explicit reload consent requires fresh authenticated domain identity and the same held OS process.
#include "../../src/app/src/unity_mono_rebind.hpp"
#include <gtest/gtest.h>
namespace frost::app {
namespace {
TEST(UnityMonoRebind, RequiresNewAuthenticatedDomainAndOriginalUnrevokedConsent) {
    UnityMonoDebugBinding binding;
    binding.root = "/owned";
    binding.instance = "editor-instance";
    binding.session = "old-domain";
    binding.selection.pid = 42;
    binding.trustRevision = 7;
    binding.processToken = 99;
    binding.image = "/unity/Editor";
    auto epoch = std::make_shared<std::atomic_uint64_t>(3);
    UnityEditorIdentity next;
    next.root = binding.root;
    next.instance = binding.instance;
    next.session = "new-domain";
    next.reportedPid = 42;
    next.epoch = epoch;
    next.revision = 3;
    next.authenticated = true;
    EXPECT_TRUE(canRebindUnityMono(binding, next, "/owned", true, 7));
    EXPECT_FALSE(canRebindUnityMono(binding, next, "/owned", true, 9)); // revoked then regranted
    EXPECT_FALSE(canRebindUnityMono(binding, next, "/other", true, 7));
    next.authenticated = false;
    EXPECT_FALSE(canRebindUnityMono(binding, next, "/owned", true, 7));
    next.authenticated = true;
    next.compiling = true;
    EXPECT_FALSE(canRebindUnityMono(binding, next, "/owned", true, 7));
    next.compiling = false;
    next.instance = "another-editor";
    EXPECT_FALSE(canRebindUnityMono(binding, next, "/owned", true, 7));
    next.instance = binding.instance;
    next.session = binding.session;
    EXPECT_FALSE(canRebindUnityMono(binding, next, "/owned", true, 7));
    next.session = "new-domain";
    epoch->fetch_add(1);
    EXPECT_FALSE(canRebindUnityMono(binding, next, "/owned", true, 7));
    EXPECT_TRUE(sameUnityMonoProcess(binding, 42, 99, "/unity/Editor"));
    EXPECT_FALSE(sameUnityMonoProcess(binding, 42, 100, "/unity/Editor")); // recycled PID
    EXPECT_FALSE(sameUnityMonoProcess(binding, 42, 99, "/other/image"));
}
} // namespace
} // namespace frost::app
