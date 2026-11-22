// SPDX-License-Identifier: Apache-2.0
// Preserve compiler driver mode and MSVC preprocessing semantics in Clang's textual replay.
#pragma once

#include "clang/Driver/Driver.h"

#include <string>
#include <vector>

namespace frost::clang_engine::semantic {

inline bool clangClMode(llvm::StringRef pinnedDriver, const std::vector<std::string>& command, bool& clMode,
                        std::string& error) {
    std::vector<const char*> driverArguments;
    for (std::size_t index = 1; index < command.size(); ++index)
        driverArguments.push_back(command[index].c_str());
    const auto mode = clang::driver::getDriverMode(pinnedDriver, driverArguments);
    if (!mode.empty() && mode != "cl" && mode != "gcc" && mode != "g++" && mode != "cpp") {
        error = "Unsupported semantic compiler driver mode";
        return false;
    }
    clMode = clang::driver::IsClangCL(mode);
    return true;
}

// Normalize only the options whose MSVC spelling or meaning differs during textual Clang replay.
enum class MsvcReplayArgument { Unchanged, Handled, Unsupported };

inline MsvcReplayArgument normalizeMsvcReplayArgument(llvm::StringRef argument, std::vector<std::string>& arguments,
                                                      std::string& error) {
    if (argument == "/c") {
        arguments.emplace_back("-c"); // The syntax-only adjuster strips this spelling.
        return MsvcReplayArgument::Handled;
    }
    if (argument == "/Zc:preprocessor" || argument == "-Zc:preprocessor")
        return MsvcReplayArgument::Handled; // Conforming preprocessing is Clang's default.
    if (argument == "/Zc:preprocessor-" || argument == "-Zc:preprocessor-") {
        error = "MSVC's traditional preprocessor mode cannot be reproduced by Clang";
        return MsvcReplayArgument::Unsupported;
    }
    return MsvcReplayArgument::Unchanged;
}

inline void disableImplicitModules(std::vector<std::string>& arguments, bool clMode) {
    arguments.emplace_back(clMode ? "/clang:-fno-implicit-modules" : "-fno-implicit-modules");
    arguments.emplace_back(clMode ? "/clang:-fno-implicit-module-maps" : "-fno-implicit-module-maps");
}

} // namespace frost::clang_engine::semantic
