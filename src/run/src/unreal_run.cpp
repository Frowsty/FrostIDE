// SPDX-License-Identifier: Apache-2.0
#include "frost/run/unreal_run.hpp"
#include "frost/io/path.hpp"
namespace frost::run {
Result<RunConfiguration> unrealEditorRunConfiguration(const UnrealEditorRunRequest& request) {
    if (!io::isAbsolutePath(request.projectFile) || !request.projectFile.ends_with(".uproject") || request.projectFile.find('\0') != std::string::npos)
        return fail(ErrorCode::InvalidArgument, "Unreal Editor requires an absolute .uproject");
    RunConfiguration result;
    result.id = request.id;
    result.name = request.name;
    result.type = RunType::Native;
    result.program = request.editor;
    result.workingDirectory = std::string(io::parentPath(request.projectFile));
    result.arguments.push_back(request.projectFile);
    result.arguments.insert(result.arguments.end(), request.arguments.begin(), request.arguments.end());
    if (auto valid = validateRunConfiguration(result); !valid) return std::unexpected(valid.error());
    return result;
}
} // namespace frost::run
