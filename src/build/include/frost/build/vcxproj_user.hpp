// SPDX-License-Identifier: Apache-2.0
//
// The .vcxproj.user writer (SLN-11): stores a project configuration's per-user debugger settings
// (LocalDebuggerCommand, LocalDebuggerCommandArguments, LocalDebuggerWorkingDirectory,
// LocalDebuggerEnvironment, ...) in <project>.vcxproj.user the way Visual Studio does, so Visual
// Studio, MSBuild and FrostIDE's Run and Debug (run_target.hpp, which asks MSBuild) all see them.
//
/*
 * The file. MSBuild imports <project>.user after the project, so its properties win over the
 * project file's. Visual Studio keeps one PropertyGroup per Configuration|Platform:
 *
 *     <?xml version="1.0" encoding="utf-8"?>
 *     <Project ToolsVersion="Current" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
 *       <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
 *         <LocalDebuggerCommandArguments>--level 2</LocalDebuggerCommandArguments>
 *         <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>
 *       </PropertyGroup>
 *     </Project>
 *
 * A new file gets exactly that shape: a UTF-8 BOM, CRLF line ends and two-space indents.
 *
 * An edit (updateUserFileProperties) changes only the group of the configuration: the group whose
 * Condition is the standard form naming that pair (whitespace ignored, ASCII case ignored), else a
 * new one appended after the last element of <Project>. For each update, the group's unconditioned
 * elements of that name are replaced by one element holding the value, at the place of the first
 * one (or appended); an empty value removes them, as Visual Studio's property pages do when a
 * field is cleared. When a LocalDebugger* property gets a value and the group has no
 * DebuggerFlavor, <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor> is added, which is what
 * makes Visual Studio's F5 use the local debugger settings. Everything else (other groups,
 * comments, the declaration, the BOM or its absence, the line ends, the indentation) is kept; new
 * elements copy the indentation and line end the file already uses. Values are written verbatim
 * (XML-escaped only): $(Macros) stay for MSBuild to expand, as Visual Studio writes them.
 *
 * Limits: elements with a Condition of their own, <Choose> blocks, and a later unconditioned
 * group setting the same property are not changed, and they can still override the value
 * (MSBuild's last-definition rule). Run reads the result back through MSBuild, so what it starts
 * is what MSBuild evaluates, never what this writer assumed.
 *
 * Thread safety: pure functions, except writeUserFileProperties (one file read and one atomic
 * write through IFileSystem; callers serialise writers of one file). O(file size).
 */
#pragma once

#include "frost/core/error.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::build {

// One property to set (non-empty value) or remove (empty value).
struct UserFileProperty {
    std::string name;  // An MSBuild property name ("LocalDebuggerCommandArguments").
    std::string value; // Written verbatim; "" removes the property from the configuration's group.

    friend bool operator==(const UserFileProperty&, const UserFileProperty&) = default;
};

// "<project>.user", the file MSBuild's Microsoft.Common.props imports for `projectPath`.
[[nodiscard]] std::string userFilePath(std::string_view projectPath);

// The standard condition of a configuration's group: '$(Configuration)|$(Platform)'=='Debug|x64'.
[[nodiscard]] std::string userFileCondition(std::string_view configuration, std::string_view platform);

/*
 * `existing` (the .user file's bytes; "" for a file that does not exist yet) with `updates`
 * applied to the group of `configuration`|`platform` (header comment). Errors: InvalidArgument
 * (an empty configuration or platform, a property name MSBuild would refuse); Corrupt (`existing`
 * is not well-formed XML or its root is not <Project>; nothing is changed then).
 */
[[nodiscard]] Result<std::string> updateUserFileProperties(std::string_view existing, std::string_view configuration,
                                                           std::string_view platform,
                                                           const std::vector<UserFileProperty>& updates);

/*
 * Reads userFilePath(projectPath) (a missing file is empty), applies updateUserFileProperties and
 * writes the result atomically. An update that changes nothing writes nothing. Errors: those of
 * updateUserFileProperties, IFileSystem::readFile (other than NotFound) and writeFileAtomic.
 */
[[nodiscard]] Result<void> writeUserFileProperties(io::IFileSystem& fs, std::string_view projectPath,
                                                   std::string_view configuration, std::string_view platform,
                                                   const std::vector<UserFileProperty>& updates);

} // namespace frost::build
