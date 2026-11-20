// SPDX-License-Identifier: Apache-2.0
// Converts an Unreal Editor launch into a native debugger target.
#include "frost/debug/unreal_debug.hpp"

#include "frost/io/path.hpp"
namespace frost::debug {
Result<DebugTarget> unrealEditorDebugTarget(std::string editor, std::string projectFile,
                                            std::vector<std::string> arguments) {
    if (!io::isAbsolutePath(editor) || editor.find('\0') != std::string::npos || !io::isAbsolutePath(projectFile) ||
        !projectFile.ends_with(".uproject") || projectFile.find('\0') != std::string::npos)
        return fail(ErrorCode::InvalidArgument, "Unreal debug launch requires absolute editor and project paths");
    for (const auto& argument : arguments)
        if (argument.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "Unreal debug argument contains NUL");
    DebugTarget result;
    result.name = "Unreal Editor";
    result.program = std::move(editor);
    result.workingDirectory = std::string(io::parentPath(projectFile));
    result.trustRoots = {result.workingDirectory};
    result.arguments.push_back(std::move(projectFile));
    result.arguments.insert(result.arguments.end(), arguments.begin(), arguments.end());
    return result;
}
} // namespace frost::debug
