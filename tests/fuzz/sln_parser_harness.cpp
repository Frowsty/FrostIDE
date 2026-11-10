// SPDX-License-Identifier: Apache-2.0
//
// Fuzz harness for frost::project's .sln, .slnx, .vcxproj and SolutionModel parsers
// (FND-14, PLAN.md Appendix C):
//   - parses arbitrary byte sequences as .sln (parseSln) and .slnx (parseSlnx) under both
//     POSIX and Windows solution paths;
//   - parses arbitrary byte sequences as .vcxproj and .vcxproj.filters;
//   - loads a synthetic workspace in MemFs through SolutionModel::load() and exercises node,
//     configuration and project queries.

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/project/solution_file.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/vcxproj.hpp"

#include "fuzz/fuzz_harness.hpp"
#include "support/crash_dialogs.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace frost::fuzz {
namespace {

void inspectSolutionFile(const project::SolutionFile& sln) {
    for (const auto& cfg : sln.configurations) {
        (void)cfg.name();
    }
    for (const auto& folder : sln.folders) {
        (void)folder.id;
        (void)folder.name;
        (void)folder.parentId;
        (void)folder.files.size();
    }
    for (const auto& proj : sln.projects) {
        (void)proj.name;
        (void)proj.path;
        (void)proj.projectGuid;
        (void)proj.dependencies.size();
    }
    (void)sln.warnings.size();
}

void inspectSolutionModel(const project::SolutionModel& model) {
    (void)model.nodeCount();
    (void)model.name();
    (void)model.configurations();
    (void)model.warnings();
    (void)model.findConfiguration("Debug|x64");
    for (std::size_t i = 0; i < model.projects().size(); ++i) {
        const project::ProjectId pid{static_cast<std::uint32_t>(i)};
        (void)model.project(pid);
        (void)model.projectFilePaths(pid);
        for (std::size_t c = 0; c < model.configurations().size(); ++c) {
            (void)model.projectConfiguration(pid, c);
        }
    }
}

} // namespace

void initialize() { frost::test::disableCrashDialogs(); }

Result<void> runInput(std::span<const std::uint8_t> data) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());

    if (auto slnPosix = project::parseSln(text, "/workspace/Fuzz.sln")) {
        inspectSolutionFile(*slnPosix);
    }
    if (auto slnWin = project::parseSln(text, "C:\\workspace\\Fuzz.sln")) {
        inspectSolutionFile(*slnWin);
    }
    if (auto slnxPosix = project::parseSlnx(text, "/workspace/Fuzz.slnx")) {
        inspectSolutionFile(*slnxPosix);
    }
    if (auto slnxWin = project::parseSlnx(text, "C:\\workspace\\Fuzz.slnx")) {
        inspectSolutionFile(*slnxWin);
    }

    ManualClock clock;
    io::MemFs fs(clock, io::PathStyle::Posix);
    (void)fs.createDirectories("/workspace/App");

    constexpr std::string_view kSeparator = "\n---VCXPROJ---\n";
    const std::size_t splitPos = text.find(kSeparator);
    const std::string_view slnPart = splitPos == std::string_view::npos ? text : text.substr(0, splitPos);
    const std::string_view vcxPart =
        splitPos == std::string_view::npos ? text : text.substr(splitPos + kSeparator.size());

    (void)fs.writeFileAtomic("/workspace/Fuzz.sln", slnPart);
    (void)fs.writeFileAtomic("/workspace/Fuzz.slnx", slnPart);
    (void)fs.writeFileAtomic("/workspace/App/App.vcxproj", vcxPart);

    if (auto vcx = project::parseVcxproj(fs, vcxPart, "/workspace/App/App.vcxproj")) {
        (void)project::clCompileMetadata(*vcx, "Debug|x64");
        (void)project::parseVcxprojFilters(fs, text, *vcx);
    } else {
        project::VcxProject emptyProject;
        emptyProject.path = "/workspace/App/App.vcxproj";
        emptyProject.directory = "/workspace/App";
        (void)project::parseVcxprojFilters(fs, text, emptyProject);
    }
    (void)project::parseConfigurationProperties(vcxPart, "/workspace/App/App.vcxproj", "Debug", "x64");

    if (auto modelSln = project::SolutionModel::load(fs, "/workspace/Fuzz.sln")) {
        inspectSolutionModel(*modelSln);
    }
    if (auto modelSlnx = project::SolutionModel::load(fs, "/workspace/Fuzz.slnx")) {
        inspectSolutionModel(*modelSlnx);
    }
    return {};
}

} // namespace frost::fuzz
