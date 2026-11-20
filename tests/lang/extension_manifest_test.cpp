// SPDX-License-Identifier: Apache-2.0
// Authored public contribution contract regressions; no external tool dependency.
#include "frost/io/spawn_log.hpp"
#include "frost/lang/extension_manifest.hpp"

#include <gtest/gtest.h>
namespace frost::lang {
namespace {
Json descriptor() {
#ifdef _WIN32
    const std::string root = "C:/extensions";
#else
    const std::string root = "/extensions";
#endif
    return Json::object()
        .set("id", "python-local")
        .set("name", "Local Python provider")
        .set("languageId", "python")
        .set("version", "user-reviewed-version")
        .set("enabled", true)
        .set("extensions", Json::array({".py"}))
        .set("executable", root + "/adapter")
        .set("arguments", Json::array({"--stdio"}))
        .set("license", "MIT")
        .set("licensePath", root + "/LICENSE")
        .set("licenseSha256", io::sha256Hex("license"))
        .set("artifacts",
             Json::array({Json::object().set("path", root + "/adapter").set("sha256", io::sha256Hex("tool"))}));
}
Json extension() {
    return Json::object()
        .set("id", "python-tools")
        .set("name", "Personal Python tools")
        .set("version", "1")
        .set("enabled", true)
        .set("capabilities", Json::array({"languages", "commands", "debugAdapters"}))
        .set("contributes",
             Json::object()
                 .set("languageServers", Json::array({descriptor()}))
                 .set("commands", Json::array({Json::object()
                                                   .set("id", "organize")
                                                   .set("title", "Organize imports")
                                                   .set("serverId", "python-local")
                                                   .set("command", "provider.organize")
                                                   .set("arguments", Json::array({"${file}"}))}))
                 .set("debugAdapters", Json::array({Json::object()
                                                        .set("id", "debug")
                                                        .set("title", "Python DAP")
                                                        .set("adapterId", "python")
                                                        .set("tool", descriptor())
                                                        .set("launch", Json::object().set("program", "${file}"))})));
}
std::string catalog(Json row) {
    return Json::object().set("apiVersion", 1).set("extensions", Json::array({std::move(row)})).dump();
}
TEST(ExtensionManifest, NegotiatesExactApiAndExplicitCapabilityFamilies) {
    auto parsed = ExtensionCatalog::parse(catalog(extension()));
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->contributions().size(), 1u);
    EXPECT_EQ(parsed->contributions()[0].commands[0].command, "provider.organize");
    EXPECT_FALSE(ExtensionCatalog::parse(Json::object().set("apiVersion", 2).set("extensions", Json::array()).dump()));
    EXPECT_FALSE(ExtensionCatalog::parse(R"({"apiVersion":1.0,"extensions":[]})"));
    auto row = extension();
    row.set("capabilities", Json::array({"languages"}));
    EXPECT_FALSE(ExtensionCatalog::parse(catalog(row)));
    row = extension();
    row.set("capabilities", Json::array({"languages", "commands", "nativeCode"}));
    EXPECT_FALSE(ExtensionCatalog::parse(catalog(row)));
    row = extension();
    auto contributes = *row.find("contributes");
    contributes.set("installScript", "run-me");
    row.set("contributes", contributes);
    EXPECT_FALSE(ExtensionCatalog::parse(catalog(row)));
}
TEST(ExtensionManifest, ComposesEnabledServersAtomicallyAndRefusesSelectorCollisions) {
    const auto base = Json::object().set("version", 1).set("servers", Json::array()).dump();
    auto parsed = ExtensionCatalog::parse(catalog(extension()));
    ASSERT_TRUE(parsed);
    auto combined = parsed->composeLanguageRegistry(base);
    ASSERT_TRUE(combined);
    auto registry = LanguageServerRegistry::parse(*combined);
    ASSERT_TRUE(registry);
    ASSERT_NE(registry->forPath("example.py"), nullptr);
    EXPECT_FALSE(parsed->composeLanguageRegistry(*combined));
    auto row = extension();
    row.set("enabled", false);
    auto disabled = ExtensionCatalog::parse(catalog(row));
    ASSERT_TRUE(disabled);
    EXPECT_EQ(*disabled->composeLanguageRegistry(base), base);
}
TEST(ExtensionManifest, RestrictsTemplatesToLiteralFileAndWorkspaceValues) {
    auto args =
        Json::object().set("program", "${file}").set("args", Json::array({"${workspaceFolder}/data", "literal"}));
    auto expanded = expandExtensionValues(args, "/work/main.py", "/work");
    ASSERT_TRUE(expanded);
    EXPECT_EQ(expanded->stringAt("program"), "/work/main.py");
    EXPECT_EQ(*expanded->find("args")->items()[0].asString(), "/work/data");
    EXPECT_FALSE(expandExtensionValues(Json("${env:SECRET}"), "file", "root"));
    EXPECT_FALSE(expandExtensionValues(Json("${command:install}"), "file", "root"));
    EXPECT_FALSE(expandExtensionValues(Json("${file"), "file", "root"));
    EXPECT_FALSE(expandExtensionValues(Json("literal"), "file", "root", 33));
    // Inserted paths are data, never recursively interpreted as another template.
    auto literal = expandExtensionValues(Json("${file}"), "/work/${env:SECRET}", "root");
    ASSERT_TRUE(literal);
    EXPECT_EQ(*literal->asString(), "/work/${env:SECRET}");
}
TEST(ExtensionManifest, CommandsRequireActualInitializeCapabilityEvidence) {
    ServerCapabilities capabilities;
    EXPECT_FALSE(extensionCommandAdvertised(capabilities, "provider.organize"));
    capabilities.raw = Json::object().set("executeCommandProvider",
                                          Json::object().set("commands", Json::array({"provider.organize"})));
    EXPECT_TRUE(extensionCommandAdvertised(capabilities, "provider.organize"));
    EXPECT_FALSE(extensionCommandAdvertised(capabilities, "provider.install"));
}
TEST(ExtensionManifest, SandboxRequiresExactPinnedRuntimeInventoryAndNoAmbientGrants) {
    auto server = descriptor();
    auto sandbox = Json::object()
                       .set("profile", "read-only-stdio")
                       .set("files", Json::array({Json::object()
                                                      .set("path", server.stringAt("executable"))
                                                      .set("relativePath", "sample-provider")
                                                      .set("executable", true)}));
    server.set("sandbox", sandbox);
    const auto wrap = [](Json value) {
        return Json::object().set("version", 1).set("servers", Json::array({std::move(value)})).dump();
    };
    auto parsed = LanguageServerRegistry::parse(wrap(server));
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->definitions()[0].sandboxFiles.size(), 1u);
    sandbox.set("network", true);
    server.set("sandbox", sandbox);
    EXPECT_FALSE(LanguageServerRegistry::parse(wrap(server)));
    sandbox = Json::object()
                  .set("profile", "read-only-stdio")
                  .set("files", Json::array({Json::object()
                                                 .set("path", server.stringAt("executable"))
                                                 .set("relativePath", "../escape")
                                                 .set("executable", true)}));
    server.set("sandbox", sandbox);
    EXPECT_FALSE(LanguageServerRegistry::parse(wrap(server)));
    sandbox.set("files", Json::array({Json::object()
                                          .set("path", server.stringAt("executable"))
                                          .set("relativePath", "provider")
                                          .set("executable", false)}));
    server.set("sandbox", sandbox);
    EXPECT_FALSE(LanguageServerRegistry::parse(wrap(server)));
    sandbox.set("profile", "ambient-permissions");
    server.set("sandbox", sandbox);
    EXPECT_FALSE(LanguageServerRegistry::parse(wrap(server)));
}
TEST(ExtensionManifest, DebugToolsKeepRegistryLicenseAndPinGovernanceAndRejectAttach) {
    auto row = extension();
    auto contributions = *row.find("contributes");
    auto adapter = contributions.find("debugAdapters")->items()[0];
    adapter.set("launch", Json::object().set("request", "attach"));
    contributions.set("debugAdapters", Json::array({adapter}));
    row.set("contributes", contributions);
    EXPECT_FALSE(ExtensionCatalog::parse(catalog(row)));
    row = extension();
    contributions = *row.find("contributes");
    adapter = contributions.find("debugAdapters")->items()[0];
    auto tool = descriptor();
    tool.set("license", "GPL-3.0-only");
    adapter.set("tool", tool);
    contributions.set("debugAdapters", Json::array({adapter}));
    row.set("contributes", contributions);
    EXPECT_FALSE(ExtensionCatalog::parse(catalog(row)));
}
} // namespace
} // namespace frost::lang
