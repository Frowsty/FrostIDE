// SPDX-License-Identifier: Apache-2.0
// REF14 authored real-tool goldens: multi-TU class/global/template/macro/PCH identity and conflicts.
// Execution is deferred by user; all fixture writes belong to ScratchDir, never product files.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/rpc/json.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
namespace {
using frost::rpc::Json;
using namespace std::chrono_literals;
class ProjectRename : public ::testing::Test {
protected:
    frost::test::ScratchDir scratch{"project-rename"};
    std::string compiler = *frost::io::normalizePath(FROST_TEST_CXX_COMPILER);
    std::string helper = *frost::io::normalizePath(FROST_CLANG_BINARY);
    std::map<std::string, std::string> before;
    int exit = -1;
    Json plan(std::map<std::string, std::string> files, std::vector<std::string> units, std::string anchor,
              std::size_t offset, std::string name, std::vector<std::string> flags = {},
              Json overlays = Json::array()) {
        auto& fs = frost::io::realFileSystem();
        std::vector<Json> commands;
        for (const auto& [path, bytes] : files) {
            auto written = fs.writeFileAtomic(scratch.join(path), bytes);
            EXPECT_TRUE(written);
            if (!written)
                return {};
            before[scratch.join(path)] = bytes;
        }
        for (const auto& unit : units) {
            const auto path = scratch.join(unit);
#ifdef _WIN32
            std::vector<std::string> arguments{compiler, "/std:c++20", "/c", path,
                                               "/Fo" + scratch.join("unexpected.obj")};
#else
            std::vector<std::string> arguments{compiler, "-std=c++20", "-c", path, "-o", scratch.join("unexpected.o")};
#endif
            arguments.insert(arguments.end(), flags.begin(), flags.end());
            std::vector<Json> args;
            for (const auto& item : arguments)
                args.emplace_back(item);
            commands.push_back(Json::object()
                                   .set("directory", scratch.path())
                                   .set("file", path)
                                   .set("arguments", Json::array(std::move(args))));
        }
        const auto request = Json::object()
                                 .set("projectRoot", scratch.path())
                                 .set("seedTranslationUnit", "")
                                 .set("path", scratch.join(anchor))
                                 .set("offset", offset)
                                 .set("newName", name)
                                 .set("compilationDatabase", Json::array(std::move(commands)).dump())
                                 .set("overlays", std::move(overlays));
        frost::io::ProcessSpec spec;
        spec.executable = helper;
        spec.arguments = {"--plan-project-rename"};
        spec.workingDirectory = scratch.path();
        spec.stdinMode = frost::io::Stdio::Pipe;
        spec.environment.emplace_back("FROST_LLVM_ROOT",
                                      std::filesystem::path(compiler).parent_path().parent_path().generic_string());
        auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, request.dump(), 5min);
        EXPECT_TRUE(result);
        if (!result)
            return {};
        exit = result->exitCode;
        auto value = frost::rpc::parseJson(result->stdoutBytes);
        EXPECT_TRUE(value) << result->stdoutBytes << result->stderrBytes;
        for (const auto& [path, bytes] : before) {
            auto disk = fs.readFile(path);
            EXPECT_TRUE(disk);
            if (disk)
                EXPECT_EQ(*disk, bytes);
        }
        EXPECT_FALSE(std::filesystem::exists(scratch.join("unexpected.obj")));
        EXPECT_FALSE(std::filesystem::exists(scratch.join("unexpected.o")));
        return value ? std::move(*value) : Json{};
    }
    const Json* image(const Json& value, std::string_view leaf) {
        const auto* files = value.find("files");
        if (!files)
            return nullptr;
        for (const auto& file : files->items())
            if (file.stringAt("path") == scratch.join(leaf))
                return &file;
        return nullptr;
    }
};
TEST_F(ProjectRename, ClassAcrossFiftyFilesIncludesHeaderAndPreservesOtherNamespace) {
    const std::string header =
        "#pragma once\nnamespace other { struct Widget {}; }\nstruct Widget { int value() { return 7; } };\n";
    std::map<std::string, std::string> files{{"type.h", header}};
    std::vector<std::string> units;
    for (int i = 0; i < 50; ++i) {
        auto name = "unit" + std::to_string(i) + ".cpp";
        units.push_back(name);
        files[name] = "#include \"type.h\"\nint use" + std::to_string(i) +
                      "(){Widget value; other::Widget untouched; return value.value();}\n";
    }
    auto result = plan(files, units, "type.h", header.find("struct Widget { int") + 7, "Gadget");
    ASSERT_EQ(result.stringAt("status"), "ready");
    ASSERT_EQ(exit, 0);
    ASSERT_NE(result.find("files"), nullptr);
    EXPECT_EQ(result.find("files")->items().size(), 51U);
    const auto* changed = image(result, "type.h");
    ASSERT_NE(changed, nullptr);
    EXPECT_NE(changed->stringAt("after").find("namespace other { struct Widget {}; }"), std::string_view::npos);
    EXPECT_NE(changed->stringAt("after").find("struct Gadget { int"), std::string_view::npos);
    EXPECT_EQ(result.find("translationUnitConfigurations")->asUnsigned(), 50U);
}
TEST_F(ProjectRename, DetectsCaptureOfAnExistingOverloadWithoutSyntaxErrors) {
    const std::string code =
        "int target(int x){return x;} int pick(double x){return (int)x;} int use(){return target(1)+pick(1);}";
    auto result = plan({{"one.cpp", code}}, {"one.cpp"}, "one.cpp", code.find("target"), "pick");
    EXPECT_EQ(result.stringAt("status"), "conflict");
    EXPECT_NE(exit, 0);
    ASSERT_NE(result.find("files"), nullptr);
    EXPECT_TRUE(result.find("files")->items().empty());
}
TEST_F(ProjectRename, TemplatesConstructorsDestructorsAndHeaderSelectionUseCanonicalClass) {
    const std::string header =
        "#pragma once\ntemplate<class T> struct Widget { T value; Widget()=default; ~Widget()=default; };\n";
    auto result = plan({{"type.h", header},
                        {"one.cpp", "#include \"type.h\"\nWidget<int> make(){Widget<int> value; return value;}\n"}},
                       {"one.cpp"}, "type.h", header.find("Widget"), "Gadget");
    ASSERT_EQ(result.stringAt("status"), "ready");
    const auto* changed = image(result, "type.h");
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->stringAt("after").find("Widget"), std::string_view::npos);
}
TEST_F(ProjectRename, VirtualOverrideFamilyRenamesTogether) {
    const std::string code = "struct Base{virtual int tick(){return 1;}}; struct Child:Base{int tick() override{return "
                             "2;}}; int use(Base& value){return value.tick();}";
    auto result = plan({{"one.cpp", code}}, {"one.cpp"}, "one.cpp", code.find("tick"), "next");
    ASSERT_EQ(result.stringAt("status"), "ready");
    const auto* changed = image(result, "one.cpp");
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->stringAt("after").find("tick"), std::string_view::npos);
}
TEST_F(ProjectRename, MacroBodyAndTokenSynthesisAreExplicitRefusals) {
    const std::string code = "struct Widget {};\n#define TYPE Widget\nTYPE value;\n";
    auto result = plan({{"one.cpp", code}}, {"one.cpp"}, "one.cpp", code.find("Widget"), "Gadget");
    EXPECT_EQ(result.stringAt("status"), "unsupported");
    EXPECT_NE(exit, 0);
    ASSERT_NE(result.find("files"), nullptr);
    EXPECT_TRUE(result.find("files")->items().empty());
}
TEST_F(ProjectRename, MacroArgumentSpellingCanRemainAProvenDirectToken) {
    const std::string code = "struct Widget {};\n#define DECLARE(type,name) type name\nDECLARE(Widget,value);\n";
    auto result = plan({{"one.cpp", code}}, {"one.cpp"}, "one.cpp", code.find("Widget"), "Gadget");
    ASSERT_EQ(result.stringAt("status"), "ready");
    const auto* changed = image(result, "one.cpp");
    ASSERT_NE(changed, nullptr);
    EXPECT_NE(changed->stringAt("after").find("DECLARE(Gadget,value)"), std::string_view::npos);
}
TEST_F(ProjectRename, DirtyHeaderOverlayIsTheOriginalImageAndDiskIsUntouched) {
    const auto path = scratch.join("type.h");
    const std::string dirty = "#pragma once\nstruct Widget { int changed; };\n";
    auto overlays = Json::array({Json::object().set("path", path).set("contents", dirty)});
    auto result =
        plan({{"type.h", "#pragma once\nstruct Widget {};\n"}, {"one.cpp", "#include \"type.h\"\nWidget value;\n"}},
             {"one.cpp"}, "type.h", dirty.find("Widget"), "Gadget", {}, std::move(overlays));
    ASSERT_EQ(result.stringAt("status"), "ready");
    const auto* changed = image(result, "type.h");
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->stringAt("before"), dirty);
    EXPECT_EQ(changed->boolAt("overlay"), true);
}
#ifdef _WIN32
TEST_F(ProjectRename, MsvcYuPchIsReplayedFromItsCapturedHeaderWithoutArtifacts) {
    const std::string header = "#pragma once\nstruct Widget {};\n";
    auto result = plan({{"type.h", header}, {"one.cpp", "#include \"type.h\"\nWidget value;\n"}}, {"one.cpp"}, "type.h",
                       header.find("Widget"), "Gadget", {"/Yutype.h", "/Fp" + scratch.join("unread.pch")});
    ASSERT_EQ(result.stringAt("status"), "ready");
    ASSERT_NE(result.find("pch"), nullptr);
    EXPECT_FALSE(result.find("pch")->items().empty());
    EXPECT_FALSE(std::filesystem::exists(scratch.join("unread.pch")));
}
#endif
} // namespace
