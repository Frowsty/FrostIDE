// SPDX-License-Identifier: Apache-2.0
//
// SLN-5: clangd's compile commands for a Visual Studio solution from MSBuild's own
// GetClCommandLines, as the solution's evaluation (frost::project::SolutionEvaluation with
// clCommandLines on) collected them through ProjectHost: one owned database directory per
// solution configuration, so switching Debug|x64 to Release|x64 gives clangd another database and
// its own index shards (as CPP-3 does for CMake).
/*
 * locate() waits until the evaluation is Ready (cancellable; it polls the evaluation's status),
 * then writes every project's commands (frost::project::clangdCommands: the command line MSBuild
 * computed, minus the output and PCH-creation switches, plus --target and the IncludePath and
 * ExternalIncludePath directories as /imsvc) to `<directory>/compile_commands.json` and answers
 * with source ClCommandLines. It answers nullopt, so a chain falls through to the next provider,
 * when the evaluation stopped, evaluated nothing (no MSBuild, not trusted, not Windows) or has no
 * command lines (ProjectHost unavailable: the MSBuild.exe fallback evaluates items only);
 * lastProblem() then says why, for the log. Projects whose GetClCommandLines failed have no entry
 * and clangd infers their flags; the evaluation's warnings name them.
 *
 * The evaluation is held weakly: a newer evaluation (another configuration) replaces it, and the
 * old one may be destroyed while clangd still holds this provider; locate() then answers nullopt.
 * Revalidation of cached projects (SLN-4) that changes command lines after Ready reaches clangd at
 * its next start. Thread safety: one locate() at a time (ClangdService's Background lane).
 */
#pragma once

#include "frost/lang/compile_commands.hpp"
#include "frost/project/project_host.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace frost::lang {

class ClCommandLinesCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    // `fs` must outlive the provider; `directory` is absolute (the configuration's own directory).
    ClCommandLinesCompileCommandsProvider(io::IFileSystem& fs, std::weak_ptr<project::SolutionEvaluation> evaluation,
                                          std::string directory, std::string compiler = "clang-cl");

    // See the header comment. Errors: those of writeCompileCommandsLocation.
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate() override;
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate(std::stop_token stop) override;

    // Why the last locate() answered nullopt ("" after a database).
    [[nodiscard]] const std::string& lastProblem() const noexcept { return m_problem; }

private:
    io::IFileSystem& m_fs;
    std::weak_ptr<project::SolutionEvaluation> m_evaluation;
    std::string m_directory;
    std::string m_compiler;
    std::string m_problem;
};

// The directory name of a solution configuration's database: "Debug|x64" -> "Debug_x64" (every
// character other than ASCII letters, digits, '.', '-' and '_' becomes '_'). Pure.
[[nodiscard]] std::string configurationDirectoryName(std::string_view configuration);

} // namespace frost::lang
