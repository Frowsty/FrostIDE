// SPDX-License-Identifier: Apache-2.0
//
// Tests for the .vcxproj.user writer (frost/build/vcxproj_user.hpp, SLN-11): a new file in Visual
// Studio's shape, edits that change only the configuration's group (the rest of the file byte for
// byte), DebuggerFlavor, removal, line ends and BOMs, refusals, and the file round trip over a
// MemFs. That MSBuild reads what is written is checked with the real MSBuild in
// msbuild_build_test.cpp (MsBuildRun.TheUserFileFrostIdeWritesIsWhatMsBuildAndRunUse).

#include "frost/build/vcxproj_user.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::build::UserFileProperty;

std::string update(std::string_view existing, std::string_view configuration, std::string_view platform,
                   const std::vector<UserFileProperty>& updates) {
    auto result = frost::build::updateUserFileProperties(existing, configuration, platform, updates);
    EXPECT_TRUE(result.has_value()) << result.error().describe();
    return result.value_or("");
}

// The file Visual Studio 2022 writes when one configuration's command arguments are set.
TEST(VcxprojUser, ANewFileHasVisualStudiosShape) {
    EXPECT_EQ(frost::build::userFilePath("C:\\src\\App\\App.vcxproj"), "C:\\src\\App\\App.vcxproj.user");
    EXPECT_EQ(frost::build::userFileCondition("Debug", "x64"), "'$(Configuration)|$(Platform)'=='Debug|x64'");
    EXPECT_EQ(update("", "Debug", "x64", {{"LocalDebuggerCommandArguments", "--level 2"}}),
              "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
              "<Project ToolsVersion=\"Current\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n"
              "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\r\n"
              "    <LocalDebuggerCommandArguments>--level 2</LocalDebuggerCommandArguments>\r\n"
              "    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>\r\n"
              "  </PropertyGroup>\r\n"
              "</Project>\r\n");
    // Only removals for a file that does not exist: nothing to write.
    EXPECT_EQ(update("", "Debug", "x64", {{"LocalDebuggerCommandArguments", ""}}), "");
}

// tests/build/fixtures/tiny_sln/App/App.vcxproj.user's shape: LF line ends, no BOM, comments.
constexpr std::string_view kExisting =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!-- A comment before the root. -->\n"
    "<Project ToolsVersion=\"Current\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
    "  <PropertyGroup>\n"
    "    <ShowAllFiles>true</ShowAllFiles>\n"
    "  </PropertyGroup>\n"
    "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)' == 'debug|X64'\">\n"
    "    <LocalDebuggerCommandArguments>--print \"two words\" --exit 3</LocalDebuggerCommandArguments>\n"
    "    <!-- The user's note. -->\n"
    "    <LocalDebuggerWorkingDirectory>$(SolutionDir)Lib</LocalDebuggerWorkingDirectory>\n"
    "    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>\n"
    "  </PropertyGroup>\n"
    "</Project>";

TEST(VcxprojUser, AnEditChangesOnlyTheConfigurationsGroup) {
    // The condition matches whatever its spacing and case; only the value's line changes.
    std::string expected(kExisting);
    expected.replace(expected.find("--print \"two words\" --exit 3"), 28, "--frames 10 > out.txt");
    std::string escaped = expected;
    escaped.replace(escaped.find("> out.txt"), 1, "&gt;");
    EXPECT_EQ(update(kExisting, "Debug", "x64", {{"LocalDebuggerCommandArguments", "--frames 10 > out.txt"}}), escaped);

    // The same value again changes nothing.
    EXPECT_EQ(update(kExisting, "Debug", "x64", {{"LocalDebuggerCommandArguments", "--print \"two words\" --exit 3"}}),
              kExisting);

    // A new property goes after the group's last element, indented like its siblings.
    std::string added(kExisting);
    added.insert(added.find("\n  </PropertyGroup>\n</Project>"),
                 "\n    <LocalDebuggerEnvironment>A=1\nB=$(Configuration)</LocalDebuggerEnvironment>");
    EXPECT_EQ(update(kExisting, "Debug", "x64", {{"LocalDebuggerEnvironment", "A=1\nB=$(Configuration)"}}), added);

    // An empty value removes the property with its line; the comment and the rest stay.
    std::string removed(kExisting);
    removed.erase(
        removed.find("\n    <LocalDebuggerWorkingDirectory>"),
        std::string_view("\n    <LocalDebuggerWorkingDirectory>$(SolutionDir)Lib</LocalDebuggerWorkingDirectory>")
            .size());
    EXPECT_EQ(update(kExisting, "Debug", "x64", {{"LocalDebuggerWorkingDirectory", ""}}), removed);
}

TEST(VcxprojUser, AnotherConfigurationGetsAGroupOfItsOwn) {
    std::string expected(kExisting);
    expected.insert(expected.rfind("\n</Project>"),
                    "\n  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|Win32'\">"
                    "\n    <LocalDebuggerWorkingDirectory>C:\\work</LocalDebuggerWorkingDirectory>"
                    "\n    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>"
                    "\n  </PropertyGroup>");
    EXPECT_EQ(update(kExisting, "Release", "Win32", {{"LocalDebuggerWorkingDirectory", "C:\\work"}}), expected);

    // A non-debugger property adds no DebuggerFlavor; the unconditioned group is never touched.
    const std::string other = update(kExisting, "Profile", "x64", {{"ShowAllFiles", "false"}});
    EXPECT_TRUE(
        other.contains("'Profile|x64'\">\n    <ShowAllFiles>false</ShowAllFiles>\n  </PropertyGroup>\n</Project>"))
        << other;
    EXPECT_TRUE(other.contains("<ShowAllFiles>true</ShowAllFiles>")) << other;
}

TEST(VcxprojUser, DuplicatesCollapseAndConditionedElementsStay) {
    constexpr std::string_view kDuplicates =
        "<Project>\n"
        "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n"
        "    <LocalDebuggerCommand>a.exe</LocalDebuggerCommand>\n"
        "    <LocalDebuggerCommand Condition=\"'$(X)'=='1'\">x.exe</LocalDebuggerCommand>\n"
        "    <LocalDebuggerCommand>b.exe</LocalDebuggerCommand>\n"
        "    <DebuggerFlavor>WindowsRemoteDebugger</DebuggerFlavor>\n"
        "  </PropertyGroup>\n"
        "</Project>\n";
    // The first unconditioned one holds the value, the later one goes, the conditioned one and the
    // user's own DebuggerFlavor stay.
    EXPECT_EQ(update(kDuplicates, "Debug", "x64", {{"LocalDebuggerCommand", "C:\\tools\\host.exe"}}),
              "<Project>\n"
              "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n"
              "    <LocalDebuggerCommand>C:\\tools\\host.exe</LocalDebuggerCommand>\n"
              "    <LocalDebuggerCommand Condition=\"'$(X)'=='1'\">x.exe</LocalDebuggerCommand>\n"
              "    <DebuggerFlavor>WindowsRemoteDebugger</DebuggerFlavor>\n"
              "  </PropertyGroup>\n"
              "</Project>\n");
}

TEST(VcxprojUser, LineEndsAndTheBomAreKept) {
    std::string crlf;
    for (const char c : kExisting) {
        crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    }
    const std::string withBom = "\xEF\xBB\xBF" + crlf;
    const std::string edited = update(withBom, "Debug", "x64", {{"LocalDebuggerCommandArguments", "--x"}});
    EXPECT_TRUE(edited.starts_with("\xEF\xBB\xBF<?xml")) << edited;
    EXPECT_TRUE(edited.contains("\r\n    <LocalDebuggerCommandArguments>--x</LocalDebuggerCommandArguments>\r\n"))
        << edited;
    EXPECT_FALSE(edited.contains("\n\n")) << edited;
    std::size_t lone = 0;
    for (std::size_t i = 0; i < edited.size(); ++i) {
        lone += edited[i] == '\n' && (i == 0 || edited[i - 1] != '\r') ? 1 : 0;
    }
    EXPECT_EQ(lone, 0U) << "every line end stays CRLF";

    // Without a BOM none is added.
    const std::string plain = update(kExisting, "Debug", "x64", {{"LocalDebuggerCommandArguments", "--x"}});
    EXPECT_TRUE(plain.starts_with("<?xml")) << plain;
}

TEST(VcxprojUser, RefusesWhatItCannotEditSafely) {
    for (const std::string_view bad : {"<Project><PropertyGroup></Project>", "<Other />", "not xml at all"}) {
        auto refused = frost::build::updateUserFileProperties(bad, "Debug", "x64", {{"LocalDebuggerCommand", "a"}});
        ASSERT_FALSE(refused.has_value()) << bad;
        EXPECT_EQ(refused.error().code(), ErrorCode::Corrupt) << bad;
    }
    for (const std::string_view name : {"", "1st", "a b", "a<b"}) {
        auto refused = frost::build::updateUserFileProperties("", "Debug", "x64", {{std::string(name), "1"}});
        ASSERT_FALSE(refused.has_value()) << name;
        EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument) << name;
    }
    EXPECT_EQ(frost::build::updateUserFileProperties("", "", "x64", {}).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(frost::build::updateUserFileProperties("", "Debug", "", {}).error().code(), ErrorCode::InvalidArgument);
}

TEST(VcxprojUser, WritesTheFileBesideTheProject) {
    frost::build::test::MemTree tree;
    frost::io::IFileSystem& fs = tree.fs;
    tree.write("src/App/App.vcxproj", "<Project />");
    const std::string project = frost::build::test::MemTree::path("src/App/App.vcxproj");
    ASSERT_TRUE(
        frost::build::writeUserFileProperties(fs, project, "Debug", "x64", {{"LocalDebuggerCommandArguments", "--one"}})
            .has_value());
    auto written = fs.readFile(project + ".user");
    ASSERT_TRUE(written.has_value());
    EXPECT_TRUE(written->contains("<LocalDebuggerCommandArguments>--one</LocalDebuggerCommandArguments>"));

    // A second configuration is added to the same file; the first stays.
    ASSERT_TRUE(frost::build::writeUserFileProperties(fs, project, "Release", "x64",
                                                      {{"LocalDebuggerWorkingDirectory", "$(ProjectDir)run"}})
                    .has_value());
    written = fs.readFile(project + ".user");
    ASSERT_TRUE(written.has_value());
    EXPECT_TRUE(written->contains("--one"));
    EXPECT_TRUE(written->contains("'Release|x64'\">\r\n    <LocalDebuggerWorkingDirectory>$(ProjectDir)run<"))
        << *written;

    // A damaged file is left as it is.
    ASSERT_TRUE(fs.writeFileAtomic(project + ".user", "<Project><broken").has_value());
    auto refused = frost::build::writeUserFileProperties(fs, project, "Debug", "x64", {{"LocalDebuggerCommand", "x"}});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(fs.readFile(project + ".user").value_or(""), "<Project><broken");
}

} // namespace
