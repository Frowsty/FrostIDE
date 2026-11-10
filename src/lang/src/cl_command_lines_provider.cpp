// SPDX-License-Identifier: Apache-2.0
//
// ClCommandLinesCompileCommandsProvider (frost/lang/cl_command_lines_provider.hpp): GetClCommandLines
// from the solution's evaluation written as clangd's per-configuration database (SLN-5).

#include "frost/lang/cl_command_lines_provider.hpp"

#include "frost/project/cl_command_lines.hpp"

#include <cctype>
#include <format>
#include <thread>

namespace frost::lang {

std::string configurationDirectoryName(std::string_view configuration) {
    std::string out(configuration.empty() ? std::string_view("default") : configuration);
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '.' && c != '-' && c != '_') {
            c = '_';
        }
    }
    return out;
}

ClCommandLinesCompileCommandsProvider::ClCommandLinesCompileCommandsProvider(
    io::IFileSystem& fs, std::weak_ptr<project::SolutionEvaluation> evaluation, std::string directory,
    std::string compiler)
    : m_fs(fs), m_evaluation(std::move(evaluation)), m_directory(std::move(directory)),
      m_compiler(std::move(compiler)) {}

Result<std::optional<CompileCommandsLocation>> ClCommandLinesCompileCommandsProvider::locate() { return locate({}); }

Result<std::optional<CompileCommandsLocation>> ClCommandLinesCompileCommandsProvider::locate(std::stop_token stop) {
    const auto none = [this](std::string why) -> Result<std::optional<CompileCommandsLocation>> {
        m_problem = std::move(why);
        return std::optional<CompileCommandsLocation>();
    };
    project::SolutionEvaluationStatus status;
    for (;;) {
        const std::shared_ptr<project::SolutionEvaluation> evaluation = m_evaluation.lock();
        if (!evaluation) {
            return none("the solution's evaluation was replaced");
        }
        status = evaluation->status();
        if (status.state == project::SolutionEvaluationState::Ready ||
            status.state == project::SolutionEvaluationState::Stopped) {
            break;
        }
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "the compile-commands lookup was cancelled");
        }
        // A status poll: the evaluation publishes no event, and nothing depends on the period.
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (status.state == project::SolutionEvaluationState::Stopped) {
        return none("the solution's evaluation stopped");
    }
    if (!status.msbuildProblem.empty()) {
        return none(status.msbuildProblem);
    }
    const std::shared_ptr<project::SolutionEvaluation> evaluation = m_evaluation.lock();
    if (!evaluation) {
        return none("the solution's evaluation was replaced");
    }
    std::vector<CompileCommand> commands;
    for (const project::ProjectClCommandLines& lines : evaluation->clCommandLines()) {
        for (project::ClangdCompileCommand& command : project::clangdCommands(lines, m_compiler)) {
            commands.push_back(CompileCommand{.directory = std::move(command.directory),
                                              .file = std::move(command.file),
                                              .arguments = std::move(command.arguments)});
        }
    }
    if (commands.empty()) {
        return none(status.clCommandLinesProblem.empty() ? std::string("GetClCommandLines listed no source")
                                                         : status.clCommandLinesProblem);
    }
    auto location = writeCompileCommandsLocation(m_fs, m_directory, commands, CompileCommandsSource::ClCommandLines);
    if (location && *location) {
        m_problem.clear();
    }
    return location;
}

} // namespace frost::lang
