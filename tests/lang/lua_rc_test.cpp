// SPDX-License-Identifier: Apache-2.0
//
// Tests for the .luarc.json model of the Lua settings page (frost/lang/lua_rc.hpp, LUA-4) on text
// and an in-memory file system: the three key forms LuaLS accepts, edits that keep every other
// key, defaults that remove keys, refused files (not JSON, wrong types), and the checked write
// (no write without a change, Busy when the file changed on disk). That LuaLS reads what is
// written is the real-LuaLS test in lua_workspace_e2e_test.cpp.

#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lua_rc.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using frost::lang::LuaRcSettings;

#ifdef _WIN32
const std::string& kRoot() {
    static const std::string value = "C:\\proj";
    return value;
}
#else
const std::string& kRoot() {
    static const std::string value = "/proj";
    return value;
}
#endif

TEST(LuaRc, ReadsEveryKeyFormLuaLsAccepts) {
    const auto nested =
        frost::lang::parseLuaRc(R"({"runtime":{"version":"Lua 5.1"},"workspace":{"library":["lib"]},)"
                                R"("diagnostics":{"globals":["G"],"disable":["lowercase-global"],"enable":false}})");
    ASSERT_TRUE(nested.has_value()) << nested.error().describe();
    EXPECT_EQ(nested->runtimeVersion, "Lua 5.1");
    EXPECT_EQ(nested->libraries, std::vector<std::string>{"lib"});
    EXPECT_EQ(nested->globals, std::vector<std::string>{"G"});
    EXPECT_EQ(nested->disabledDiagnostics, std::vector<std::string>{"lowercase-global"});
    EXPECT_FALSE(nested->diagnosticsEnabled);
    const auto dotted = frost::lang::parseLuaRc(R"({"runtime.version":"LuaJIT","diagnostics.globals":["A","B"]})");
    ASSERT_TRUE(dotted.has_value());
    EXPECT_EQ(dotted->runtimeVersion, "LuaJIT");
    EXPECT_EQ(dotted->globals, (std::vector<std::string>{"A", "B"}));
    const auto prefixed =
        frost::lang::parseLuaRc(R"({"Lua.runtime.version":"Lua 5.3","runtime":{"version":"Lua 5.1"}})");
    ASSERT_TRUE(prefixed.has_value());
    EXPECT_EQ(prefixed->runtimeVersion, "Lua 5.3") << "the prefixed form is read first";
    const auto empty = frost::lang::parseLuaRc("  \n");
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(*empty, LuaRcSettings{});
}

TEST(LuaRc, RefusesWhatItCannotEditFaithfully) {
    for (const std::string_view bad :
         {R"({"runtime":{"version":5.4}})", R"({"workspace":{"library":"lib"}})", R"({"diagnostics":{"globals":[1]}})",
          R"({"diagnostics":{"enable":"no"}})", "[1]", "// a comment\n{}", "{"}) {
        const auto parsed = frost::lang::parseLuaRc(bad);
        ASSERT_FALSE(parsed.has_value()) << bad;
        EXPECT_EQ(parsed.error().code(), frost::ErrorCode::Corrupt) << bad;
    }
}

TEST(LuaRc, EditsKeepOtherKeysUpdateEveryFormAndDropDefaults) {
    const std::string original = R"({"$schema":"x","runtime":{"version":"Lua 5.1","path":["?.lua"]},)"
                                 R"("diagnostics.globals":["Old"],"format":{"enable":true}})";
    LuaRcSettings settings;
    settings.runtimeVersion = "Lua 5.4";
    settings.globals = {"New"};
    settings.libraries = {"types"};
    auto text = frost::lang::applyLuaRc(original, settings);
    ASSERT_TRUE(text.has_value()) << text.error().describe();
    EXPECT_EQ(*text, "{\n"
                     "    \"$schema\": \"x\",\n"
                     "    \"runtime\": {\n"
                     "        \"version\": \"Lua 5.4\",\n"
                     "        \"path\": [\n"
                     "            \"?.lua\"\n"
                     "        ]\n"
                     "    },\n"
                     "    \"diagnostics.globals\": [\n"
                     "        \"New\"\n"
                     "    ],\n"
                     "    \"format\": {\n"
                     "        \"enable\": true\n"
                     "    },\n"
                     "    \"workspace\": {\n"
                     "        \"library\": [\n"
                     "            \"types\"\n"
                     "        ]\n"
                     "    }\n"
                     "}\n");
    EXPECT_EQ(frost::lang::parseLuaRc(*text).value(), settings) << "what is written reads back";

    // Back to the defaults: the keys go, and so does a group left empty; other keys stay.
    auto reset = frost::lang::applyLuaRc(*text, LuaRcSettings{});
    ASSERT_TRUE(reset.has_value());
    const auto json = frost::lang::parseJson(*reset);
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ(json->find("workspace"), nullptr);
    EXPECT_EQ(json->find("diagnostics.globals"), nullptr);
    ASSERT_NE(json->find("runtime"), nullptr);
    EXPECT_EQ(json->find("runtime")->find("version"), nullptr);
    EXPECT_NE(json->find("runtime")->find("path"), nullptr);
    EXPECT_NE(json->find("format"), nullptr);

    LuaRcSettings off;
    off.diagnosticsEnabled = false;
    off.disabledDiagnostics = {"undefined-global"};
    const auto disabled = frost::lang::parseJson(frost::lang::applyLuaRc("", off).value());
    ASSERT_TRUE(disabled.has_value());
    EXPECT_EQ(disabled->find("diagnostics")->boolAt("enable"), false);
    EXPECT_EQ(*disabled->find("diagnostics")->find("disable")->items()[0].asString(), "undefined-global");
}

TEST(LuaRc, WritesOnlyChangesAndNeverOverAnEditOnDisk) {
    frost::ManualClock clock;
    frost::io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories(kRoot()).has_value());
    const std::string path = frost::io::joinPath(kRoot(), ".luarc.json").value();

    auto missing = frost::lang::readLuaRc(fs, kRoot());
    ASSERT_TRUE(missing.has_value()) << missing.error().describe();
    EXPECT_FALSE(missing->exists);
    EXPECT_EQ(missing->path, path);
    ASSERT_TRUE(frost::lang::writeLuaRc(fs, *missing, LuaRcSettings{}).has_value());
    EXPECT_FALSE(fs.exists(path).value_or(true)) << "the defaults create no file";

    LuaRcSettings settings;
    settings.runtimeVersion = "Lua 5.1";
    ASSERT_TRUE(frost::lang::writeLuaRc(fs, *missing, settings).has_value());
    auto loaded = frost::lang::readLuaRc(fs, kRoot());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->exists);
    EXPECT_EQ(loaded->settings, settings);
    const auto stamp = fs.status(path).value().mtime;
    clock.advance(std::chrono::seconds(5));
    ASSERT_TRUE(frost::lang::writeLuaRc(fs, *loaded, settings).has_value());
    EXPECT_EQ(fs.status(path).value().mtime, stamp) << "no change, no write";

    // Someone edits the file after it was read: the page's write is refused and the edit kept.
    ASSERT_TRUE(fs.writeFileAtomic(path, R"({"runtime":{"version":"LuaJIT"}})").has_value());
    settings.runtimeVersion = "Lua 5.3";
    const auto refused = frost::lang::writeLuaRc(fs, *loaded, settings);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Busy);
    EXPECT_EQ(fs.readFile(path).value(), R"({"runtime":{"version":"LuaJIT"}})");
    // Created by someone else after it was read as missing: refused too.
    const auto created = frost::lang::writeLuaRc(fs, *missing, settings);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().code(), frost::ErrorCode::Busy);

    EXPECT_EQ(frost::lang::readLuaRc(fs, "relative").error().code(), frost::ErrorCode::InvalidArgument);
    ASSERT_TRUE(fs.writeFileAtomic(path, "not json").has_value());
    EXPECT_EQ(frost::lang::readLuaRc(fs, kRoot()).error().code(), frost::ErrorCode::Corrupt);
}

} // namespace
