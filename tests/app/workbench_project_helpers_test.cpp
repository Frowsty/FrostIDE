// SPDX-License-Identifier: Apache-2.0
//
// The helpers the Workbench's project translation units share (src/app/src/
// workbench_project_helpers.hpp, FND-12a): extensions, Windows reserved device names, project name
// validation and XML escaping. The reserved names are the ones Windows documents for file names
// ("Naming Files, Paths, and Namespaces"); the escapes are XML 1.0's five predefined entities.

#include "../../src/app/src/workbench_project_helpers.hpp"
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using frost::ErrorCode;
using frost::app::escapeXml;
using frost::app::extensionOf;
using frost::app::isWindowsReservedDeviceName;
using frost::app::validateProjectNameComponent;

TEST(WorkbenchProjectHelpers, ExtensionOfLowerCasesTheLastComponentsExtension) {
    EXPECT_EQ(extensionOf("C:/src/Main.CPP"), ".cpp");
    EXPECT_EQ(extensionOf("C:\\src\\archive.Tar.GZ"), ".gz");
    EXPECT_EQ(extensionOf("/home/u/project.sln"), ".sln");
    EXPECT_EQ(extensionOf("App.SLNX"), ".slnx");
}

TEST(WorkbenchProjectHelpers, ExtensionOfIsEmptyWithoutAnExtension) {
    EXPECT_EQ(extensionOf(""), "");
    EXPECT_EQ(extensionOf("Makefile"), "");
    // A dot in a directory name is not the file's extension.
    EXPECT_EQ(extensionOf("C:/build.d/Makefile"), "");
    EXPECT_EQ(extensionOf("C:\\build.d\\Makefile"), "");
    // A leading dot names a hidden file, not an extension.
    EXPECT_EQ(extensionOf("/repo/.gitignore"), "");
    EXPECT_EQ(extensionOf("dir/"), "");
}

TEST(WorkbenchProjectHelpers, ReservedDeviceNamesMatchWhateverTheCaseAndExtension) {
    for (const std::string_view name : {"CON", "PRN", "AUX", "NUL", "COM1", "COM9", "LPT1", "LPT9"})
        EXPECT_TRUE(isWindowsReservedDeviceName(name)) << name;
    EXPECT_TRUE(isWindowsReservedDeviceName("con"));
    EXPECT_TRUE(isWindowsReservedDeviceName("Com3"));
    // Windows reserves the name with any extension too.
    EXPECT_TRUE(isWindowsReservedDeviceName("nul.txt"));
    EXPECT_TRUE(isWindowsReservedDeviceName("LPT2.tar.gz"));
}

TEST(WorkbenchProjectHelpers, OtherNamesAreNotReserved) {
    for (const std::string_view name : {"", "CONSOLE", "COM10", "LPT0X", "AUXILIARY", "NULL", "MyApp", ".hidden"})
        EXPECT_FALSE(isWindowsReservedDeviceName(name)) << name;
}

TEST(WorkbenchProjectHelpers, ValidProjectNamesPass) {
    for (const std::string_view name : {"MyApp", "my app 2", "Engine.Core", "a", "CONSOLE", "_tool-1"})
        EXPECT_TRUE(validateProjectNameComponent(name).has_value()) << name;
}

TEST(WorkbenchProjectHelpers, EmptyAndDotNamesAreRejected) {
    for (const std::string_view name : {"", ".", ".."}) {
        const frost::Result<void> result = validateProjectNameComponent(name);
        ASSERT_FALSE(result.has_value()) << name;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(result.error().message(), "enter a valid project name");
    }
}

TEST(WorkbenchProjectHelpers, TrailingSpaceOrPeriodIsRejected) {
    for (const std::string_view name : {"App.", "App ", "App. "}) {
        const frost::Result<void> result = validateProjectNameComponent(name);
        ASSERT_FALSE(result.has_value()) << name;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(result.error().message(), "a project name cannot end with a space or period");
    }
}

TEST(WorkbenchProjectHelpers, InvalidPathCharactersAreRejected) {
    const std::string control = std::string("a") + '\x01' + "b";
    for (const std::string_view name :
         {std::string_view("a<b"), std::string_view("a>b"), std::string_view("a:b"), std::string_view("a\"b"),
          std::string_view("a/b"), std::string_view("a\\b"), std::string_view("a|b"), std::string_view("a?b"),
          std::string_view("a*b"), std::string_view("tab\there"), std::string_view(control)}) {
        const frost::Result<void> result = validateProjectNameComponent(name);
        ASSERT_FALSE(result.has_value()) << name;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(result.error().message(), "the project name contains invalid path characters");
    }
}

TEST(WorkbenchProjectHelpers, ReservedDeviceNamesAreRejectedAsProjectNames) {
    for (const std::string_view name : {"CON", "aux", "Com1.Core"}) {
        const frost::Result<void> result = validateProjectNameComponent(name);
        ASSERT_FALSE(result.has_value()) << name;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(result.error().message(), "the project name is a reserved device name");
    }
}

TEST(WorkbenchProjectHelpers, EscapeXmlReplacesThePredefinedEntities) {
    EXPECT_EQ(escapeXml("&"), "&amp;");
    EXPECT_EQ(escapeXml("<"), "&lt;");
    EXPECT_EQ(escapeXml(">"), "&gt;");
    EXPECT_EQ(escapeXml("\""), "&quot;");
    EXPECT_EQ(escapeXml("'"), "&apos;");
    EXPECT_EQ(escapeXml("<Name a=\"x\" b='y'>R&D</Name>"),
              "&lt;Name a=&quot;x&quot; b=&apos;y&apos;&gt;R&amp;D&lt;/Name&gt;");
}

TEST(WorkbenchProjectHelpers, EscapeXmlKeepsOtherBytes) {
    EXPECT_EQ(escapeXml(""), "");
    EXPECT_EQ(escapeXml("MyApp_1.0 (x64)"), "MyApp_1.0 (x64)");
    // UTF-8 passes through byte for byte; an already escaped entity is escaped again.
    EXPECT_EQ(escapeXml("caf\xC3\xA9"), "caf\xC3\xA9");
    EXPECT_EQ(escapeXml("&amp;"), "&amp;amp;");
}

} // namespace
