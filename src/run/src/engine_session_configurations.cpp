// SPDX-License-Identifier: Apache-2.0
// Actual FrostEditor build.export and FrostRuntime --server/--connect command-line contracts.
#include "frost/run/engine_session_configurations.hpp"

#include "frost/io/path.hpp"
namespace frost::run {
Result<std::vector<RunConfiguration>> engineSessionRunConfigurations(std::string_view id, std::string_view label,
                                                                     std::string_view editor, std::string_view runtime,
                                                                     std::string_view project, std::uint16_t port) {
    if (id.empty() || !io::isAbsolutePath(editor) || !io::isAbsolutePath(project) || !port ||
        (!runtime.empty() && !io::isAbsolutePath(runtime)))
        return fail(ErrorCode::InvalidArgument,
                    "Engine configurations require explicit absolute tools/project and a port");
    std::vector<RunConfiguration> result;
    RunConfiguration exportRun;
    exportRun.id = "engine.export." + std::string(id);
    exportRun.name = "Export game (" + std::string(label) + ")";
    exportRun.program = editor;
    exportRun.arguments = {"build.export", "--project", std::string(project)};
    exportRun.workingDirectory = io::parentPath(editor);
    result.push_back(std::move(exportRun));
    if (!runtime.empty()) {
        RunConfiguration server;
        server.id = "engine.multiplayer.server." + std::string(id);
        server.name = "Dedicated server (" + std::string(label) + ")";
        server.program = runtime;
        server.workingDirectory = io::parentPath(runtime);
        server.arguments = {"--project", std::string(project), "--server", "--port", std::to_string(port)};
        result.push_back(server);
        std::vector<std::string> members{server.id};
        for (unsigned index = 1; index <= 2; ++index) {
            auto client = server;
            client.id = "engine.multiplayer.client" + std::to_string(index) + "." + std::string(id);
            client.name = "Client " + std::to_string(index) + " (" + std::string(label) + ")";
            client.arguments = {"--project", std::string(project),
                                "--connect", "127.0.0.1:" + std::to_string(port),
                                "--name",    "FrostIDE client " + std::to_string(index)};
            members.push_back(client.id);
            result.push_back(std::move(client));
        }
        RunConfiguration compound;
        compound.id = "engine.multiplayer." + std::string(id);
        compound.name =
            "Multiplayer: server + 2 clients (localhost:" + std::to_string(port) + "; " + std::string(label) + ")";
        compound.type = RunType::Compound;
        compound.configurations = std::move(members);
        result.push_back(std::move(compound));
    }
    for (const auto& config : result)
        if (auto valid = validateRunConfiguration(config); !valid)
            return std::unexpected(valid.error());
    return result;
}
} // namespace frost::run
