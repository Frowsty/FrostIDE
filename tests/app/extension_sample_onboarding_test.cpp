// SPDX-License-Identifier: Apache-2.0
// Future opt-in acceptance: actual sample binary through normal public onboarding and OS sandbox.
#include "frost/io/spawn_log.hpp"
#include "frost/lang/json.hpp"

#include "workbench_rig.hpp"
#include <gtest/gtest.h>
namespace frost::app::test {
TEST(ExtensionSample, RealSandboxedProviderInitializesCountsActualBufferAndRetires) {
    WorkDir dir("extension-real-sample");
    dir.write("workspace/note.frostnote", "one two\nthree");
    RigOptions options;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    const std::string executable = FROST_EXTENSION_SAMPLE_PATH, license = FROST_EXTENSION_SAMPLE_LICENSE;
    auto bytes = io::realFileSystem().readFileSnapshot(executable, 128 * 1024 * 1024);
    ASSERT_TRUE(bytes);
    auto evidence = io::realFileSystem().readFileSnapshot(license, 1024 * 1024);
    ASSERT_TRUE(evidence);
    auto artifacts =
        lang::Json::array({lang::Json::object().set("path", executable).set("sha256", io::sha256Hex(*bytes))});
    auto files = lang::Json::array({lang::Json::object()
                                        .set("path", executable)
                                        .set("relativePath", io::fileName(executable))
                                        .set("executable", true)});
    std::string dependencies = FROST_EXTENSION_SAMPLE_RUNTIME;
    std::size_t at = 0;
    while (at < dependencies.size()) {
        const auto end = dependencies.find('|', at);
        const auto path = dependencies.substr(at, end == dependencies.npos ? dependencies.size() - at : end - at);
        auto runtime = io::realFileSystem().readFileSnapshot(path, 128 * 1024 * 1024);
        ASSERT_TRUE(runtime) << path;
        artifacts.push(lang::Json::object().set("path", path).set("sha256", io::sha256Hex(*runtime)));
        files.push(
            lang::Json::object().set("path", path).set("relativePath", io::fileName(path)).set("executable", true));
        if (end == dependencies.npos)
            break;
        at = end + 1;
    }
    auto server = lang::Json::object()
                      .set("id", "sample-text-statistics")
                      .set("name", "Actual sample")
                      .set("languageId", "frostnote")
                      .set("version", "1")
                      .set("enabled", true)
                      .set("extensions", lang::Json::array({".frostnote"}))
                      .set("arguments", lang::Json::array())
                      .set("executable", executable)
                      .set("license", "Apache-2.0")
                      .set("licensePath", license)
                      .set("licenseSha256", io::sha256Hex(*evidence))
                      .set("artifacts", artifacts)
                      .set("sandbox", lang::Json::object().set("profile", "read-only-stdio").set("files", files));
    auto extension =
        lang::Json::object()
            .set("id", "text-statistics")
            .set("name", "Sample")
            .set("version", "1")
            .set("enabled", true)
            .set("capabilities", lang::Json::array({"languages", "commands"}))
            .set("contributes", lang::Json::object()
                                    .set("languageServers", lang::Json::array({server}))
                                    .set("commands", lang::Json::array({lang::Json::object()
                                                                            .set("id", "count")
                                                                            .set("title", "Count received text")
                                                                            .set("serverId", "sample-text-statistics")
                                                                            .set("command", "frost.sample.statistics")
                                                                            .set("arguments", lang::Json::array())})));
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openFolder(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->setExtensionCatalog(
        lang::Json::object().set("apiVersion", 1).set("extensions", lang::Json::array({extension})).dump()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/note.frostnote")));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->languageServerRegistryState();
            const auto* rows = state.find("servers");
            return rows && !rows->items().empty() &&
                   rows->items()[0].stringAt("serverName") == "Frost text-statistics sample";
        },
        std::chrono::seconds(15)))
        << rig.workbench->languageServerRegistryState().dump();
    ASSERT_TRUE(rig.workbench->executeExtensionCommand("text-statistics/count"));
    ASSERT_TRUE(waitFor([&] {
        auto state = rig.workbench->extensionCatalogState();
        const auto* last = state.find("lastCommand");
        return last && last->find("result");
    }));
    const auto observed = rig.workbench->extensionCatalogState();
    const auto* result = observed.find("lastCommand")->find("result");
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->find("documents")->size(), 1u);
    const auto& stats = result->find("documents")->items()[0];
    EXPECT_EQ(stats.unsignedAt("whitespaceSeparatedWords"), 3u);
    EXPECT_EQ(stats.unsignedAt("utf8Bytes"), 13u);
    ASSERT_TRUE(rig.workbench->setExtensionCatalog("{\"apiVersion\":1,\"extensions\":[]}"));
    EXPECT_TRUE(rig.workbench->languageServerRegistryState().find("servers")->items().empty());
    EXPECT_TRUE(rig.workbench->extensionCatalogState().find("lastCommand")->isNull());
}
} // namespace frost::app::test
