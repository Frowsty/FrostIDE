// SPDX-License-Identifier: Apache-2.0
//
// Single-file compile support (SLN-9, Visual Studio's Ctrl+F7): asking MSBuild which ClCompile
// item of a project a source file is, so the driver's Compile action (msbuild_driver.hpp) can
// pass that item's Identity as SelectedFiles.
//
/*
 * Why MSBuild is asked. The C++ targets compile the ClCompile items whose Identity equals one of
 * SelectedFiles, and every item when none matches. The Identity is the Include as MSBuild
 * evaluated it ("src\a.cpp", "..\src\MISC\Common\Common.cpp", or a full path for items a property
 * sheet adds through $(MSBuildThisFileDirectory)), so no spelling FrostIDE could derive from the
 * file's path is safe. compileItemQuerySpec runs
 *
 *     MSBuild.exe <project> -getItem:ClCompile -p:Configuration=<c> -p:Platform=<p>
 *                 [the Solution* properties Visual Studio passes] -nologo -nodeReuse:false
 *
 * (evaluation only, no target runs; MSBuild 17.8+) and selectedFileIdentity picks the item whose
 * FullPath is the file. A file that is no ClCompile item of the project is NotFound, so the
 * caller refuses instead of compiling the whole project.
 *
 * Evaluation can run project code (property functions), so only callers past the trust gate
 * start the query. Everything here is pure and portable except queryCompileItems (a child
 * process, Windows only). Thread-safe (no shared state).
 */
#pragma once

#include "frost/build/run_target.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace frost::build {

// One evaluated ClCompile item.
struct CompileItem {
    std::string identity; // As evaluated (header comment); what SelectedFiles must name.
    std::string fullPath; // Absolute.

    friend bool operator==(const CompileItem&, const CompileItem&) = default;
};

/*
 * The -getItem:ClCompile query for request.projectPath in request.configuration|platform, with the
 * Solution* properties of request.solutionPath when set (header comment); in the project's folder,
 * with the build driver's environment (English, no terminal logger, no node reuse). Pure and
 * portable (Windows path rules). Errors: InvalidArgument (a path that is not absolute, an empty
 * configuration or platform).
 */
[[nodiscard]] Result<io::ProcessSpec> compileItemQuerySpec(const RunPropertyRequest& request);

// The items of MSBuild's -getItem JSON ({"Items": {"ClCompile": [{"Identity", "FullPath", ...}]}})
// in document order; no ClCompile array is an empty list. Errors: Corrupt (not that JSON).
[[nodiscard]] Result<std::vector<CompileItem>> parseCompileItems(std::string_view json);

/*
 * The Identity of the item whose FullPath is `filePath` (absolute; both normalised, ASCII case
 * ignored, as Windows compares paths). Errors: NotFound (no item is that file; the message names
 * the file and how many items the project compiles).
 */
[[nodiscard]] Result<std::string> selectedFileIdentity(const std::vector<CompileItem>& items,
                                                       std::string_view filePath);

/*
 * Runs compileItemQuerySpec through `launcher` (blocking, up to `timeout`) and parses the answer.
 * Trusted callers only. Errors: Unsupported (not Windows, or this MSBuild has no -getItem: its
 * MSB1001), IoError (MSBuild failed; the message holds its output), Timeout, Corrupt, the
 * launcher's errors.
 */
[[nodiscard]] Result<std::vector<CompileItem>>
queryCompileItems(io::IProcessLauncher& launcher, const RunPropertyRequest& request, IClock::Duration timeout);

} // namespace frost::build
