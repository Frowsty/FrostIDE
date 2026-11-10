// SPDX-License-Identifier: Apache-2.0
//
// Tests for the single-file compile's item query (frost/build/msbuild_compile.hpp, SLN-9): the
// -getItem:ClCompile command line, the JSON MSBuild prints for it, and picking the Identity of a
// file (portable; the real MSBuild runs in msbuild_build_test.cpp).

#include "frost/build/msbuild_compile.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::build::CompileItem;

frost::build::RunPropertyRequest coreRequest() {
    return {.msbuildPath = "C:\\VS\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe",
            .projectPath = "C:\\src\\Gen\\Core\\Core.vcxproj",
            .solutionPath = "C:\\src\\Gen\\Generated.sln",
            .configuration = "Debug",
            .platform = "x64"};
}

TEST(CompileItemQuery, AsksForTheClCompileItemsOnly) {
    auto spec = frost::build::compileItemQuerySpec(coreRequest());
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    EXPECT_EQ(spec->executable, "C:\\VS\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe");
    EXPECT_EQ(spec->arguments,
              (std::vector<std::string>{"C:\\src\\Gen\\Core\\Core.vcxproj", "-getItem:ClCompile",
                                        "-p:Configuration=Debug", "-p:Platform=x64", "-p:SolutionDir=C:\\src\\Gen\\",
                                        "-p:SolutionPath=C:\\src\\Gen\\Generated.sln", "-p:SolutionName=Generated",
                                        "-p:SolutionFileName=Generated.sln", "-p:SolutionExt=.sln", "-nologo",
                                        "-nodeReuse:false"}));
    EXPECT_EQ(spec->workingDirectory, "C:\\src\\Gen\\Core");
    EXPECT_TRUE(spec->killOnClose);

    auto relative = coreRequest();
    relative.projectPath = "Core.vcxproj";
    ASSERT_FALSE(frost::build::compileItemQuerySpec(relative).has_value());
}

// The shape MSBuild 17.14 printed for tests/project/fixtures/msvc_generated/Core (2026-11-09),
// trimmed to two metadata: a property sheet's item has a full Identity, the project's own relative ones.
constexpr std::string_view kCoreItems = R"({
  "Items": {
    "ClCompile": [
      {
        "Identity": "C:\\src\\Gen\\shared\\deep_shared.cpp",
        "FullPath": "C:\\src\\Gen\\shared\\deep_shared.cpp",
        "PrecompiledHeader": "NotUsing"
      },
      {
        "Identity": "src\\a.cpp",
        "FullPath": "C:\\src\\Gen\\Core\\src\\a.cpp"
      },
      {
        "Identity": "src\\detail\\c.cpp",
        "FullPath": "C:\\src\\Gen\\Core\\src\\detail\\c.cpp"
      }
    ]
  }
})";

TEST(CompileItemQuery, ParsesTheItemsAndPicksTheFilesIdentity) {
    auto items = frost::build::parseCompileItems(kCoreItems);
    ASSERT_TRUE(items.has_value()) << items.error().describe();
    ASSERT_EQ(items->size(), 3U);
    EXPECT_EQ((*items)[1], (CompileItem{"src\\a.cpp", "C:\\src\\Gen\\Core\\src\\a.cpp"}));

    // Another spelling of the same path (separators, case, a dot segment) finds the item.
    EXPECT_EQ(frost::build::selectedFileIdentity(*items, "c:/src/gen/core/./src/A.cpp").value_or(""), "src\\a.cpp");
    EXPECT_EQ(frost::build::selectedFileIdentity(*items, "C:\\src\\Gen\\shared\\deep_shared.cpp").value_or(""),
              "C:\\src\\Gen\\shared\\deep_shared.cpp");

    // A file the project does not compile (a header, another project's source) is NotFound: the
    // caller must not pass a guessed Identity, which would compile every item.
    for (const std::string_view other :
         {"C:\\src\\Gen\\Core\\include\\core\\core.h", "C:\\src\\Gen\\Core\\src\\b.cpp"}) {
        auto missing = frost::build::selectedFileIdentity(*items, other);
        ASSERT_FALSE(missing.has_value()) << other;
        EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
        EXPECT_TRUE(missing.error().message().contains("3 ClCompile items")) << missing.error().message();
    }
}

TEST(CompileItemQuery, NoClCompileItemsAndMalformedOutput) {
    auto none = frost::build::parseCompileItems(R"({"Items": {}})");
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->empty());
    EXPECT_EQ(frost::build::selectedFileIdentity(*none, "C:\\a.cpp").error().code(), ErrorCode::NotFound);

    for (const std::string_view bad : {"MSBUILD : error MSB1009: Project file does not exist.", R"({"Properties": {}})",
                                       R"({"Items": {"ClCompile": {}}})", R"({"Items": {"ClCompile": ["a.cpp"]}})"}) {
        auto parsed = frost::build::parseCompileItems(bad);
        ASSERT_FALSE(parsed.has_value()) << bad;
        EXPECT_EQ(parsed.error().code(), ErrorCode::Corrupt) << bad;
    }
}

} // namespace
