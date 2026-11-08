// SPDX-License-Identifier: Apache-2.0
//
// Helpers shared by the Workbench's project translation units (workbench_project.cpp,
// workbench_new_project.cpp, workbench_project_sources.cpp, workbench_file_ops.cpp and
// workbench_clangd.cpp), moved here unchanged when workbench_project.cpp was split (FND-12a).
// Private to frost::app; every function is pure apart from cmakeClangdState's one stat and the
// CMake service reads of cmakeCompileCommandsInput.

#pragma once

#include "frost/build/cmake_service.hpp"
#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/cmake_compile_commands.hpp"
#include "frost/project/cmake_profile.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace frost::app {

// Suffix of cmakeClangdState when the active configuration has a database clangd can read.
inline constexpr std::string_view kCMakeDatabasePresent = "|database";

/*
 * CPP-3: the configuration clangd reads for a CMake folder (lang::CMakeCompileCommandsProvider's
 * input): the active profile's build directory, its configuration when the generator is
 * multi-config (the rule CMakeService reads the reply by), and the current model. Thread-safe (the
 * service's getters are); called on the GUI thread and on the provider's worker.
 */
inline lang::CMakeCompileCommandsInput cmakeCompileCommandsInput(const build::CMakeService& service) {
    lang::CMakeCompileCommandsInput input;
    input.buildDirectory = service.activeBuildDirectory();
    if (const auto profile = service.activeProfile(); profile && project::isMultiConfigGenerator(profile->generator))
        input.configuration = profile->effectiveConfiguration();
    input.model = service.model();
    return input;
}

/*
 * Where a CMake folder's clangd would read its compile commands right now: the active
 * configuration (build directory and, multi-config, the configuration) and whether it has a
 * database: CMake's compile_commands.json in the build directory, or (CMK-4, the Visual Studio
 * generators, which write none) a model of that configuration that compiles C or C++. "" when
 * there is no active profile. Two equal states give clangd the same --compile-commands-dir (the
 * owned directory of the configuration, CPP-3), so no restart is needed: the workbench rewrites
 * the owned copy and clangd reloads a database that changes in place. One stat, on the UI thread.
 */
inline std::string cmakeClangdState(io::IFileSystem& fs, const build::CMakeService& service) {
    const lang::CMakeCompileCommandsInput input = cmakeCompileCommandsInput(service);
    if (input.buildDirectory.empty())
        return {};
    bool present = false;
    if (auto database = io::joinPath(input.buildDirectory, "compile_commands.json")) {
        auto status = fs.status(*database);
        present = status && status->type == io::FileType::Regular;
    }
    if (!present && input.model && lang::cmakeModelDescribes(*input.model, input.buildDirectory, input.configuration))
        present = std::ranges::any_of(input.model->targets, [](const project::CMakeTarget& target) {
            return std::ranges::any_of(target.compileGroups, [](const project::CMakeCompileGroup& group) {
                return group.language == "C" || group.language == "CXX";
            });
        });
    return input.buildDirectory + "|" + input.configuration +
           std::string(present ? kCMakeDatabasePresent : std::string_view("|none"));
}

// The lower-cased extension of the last component, with its dot ("" for none).
inline std::string extensionOf(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    std::string out(name.substr(dot));
    std::ranges::transform(out, out.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
    return out;
}

inline bool isWindowsReservedDeviceName(std::string_view component) {
    const std::size_t dot = component.find('.');
    const std::string_view stem = dot == std::string_view::npos ? component : component.substr(0, dot);
    if (stem.empty())
        return false;
    std::string upper(stem);
    std::ranges::transform(upper, upper.begin(),
                           [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
    static constexpr std::array<std::string_view, 22> kReserved{
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return std::ranges::find(kReserved, std::string_view(upper)) != kReserved.end();
}

inline Result<void> validateProjectNameComponent(std::string_view name) {
    if (name.empty() || name == "." || name == "..") {
        return fail(ErrorCode::InvalidArgument, "enter a valid project name");
    }
    if (name.back() == ' ' || name.back() == '.') {
        return fail(ErrorCode::InvalidArgument, "a project name cannot end with a space or period");
    }
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch < 0x20U || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' ||
            ch == '?' || ch == '*') {
            return fail(ErrorCode::InvalidArgument, "the project name contains invalid path characters");
        }
    }
    if (isWindowsReservedDeviceName(name)) {
        return fail(ErrorCode::InvalidArgument, "the project name is a reserved device name");
    }
    return {};
}

inline std::string escapeXml(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

} // namespace frost::app
