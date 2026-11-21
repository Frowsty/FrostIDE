// SPDX-License-Identifier: Apache-2.0
// CPP-15: tool-backed C++ module dependency graph. No source-text module inference.
#pragma once
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/json.hpp"

#include <chrono>
#include <functional>
namespace frost::lang {
struct CppModuleProvide {
    std::string name, sourcePath;
    bool interface = false;
};
struct CppModuleRequire {
    std::string name, sourcePath, lookupMethod;
    bool uniqueOnSourcePath = false;
};
struct CppModuleRule {
    std::string primaryOutput;
    std::vector<CppModuleProvide> provides;
    std::vector<CppModuleRequire> requiresModules;
};
struct CppModuleGraph {
    std::vector<CppModuleRule> rules;
    std::string rawP1689;
    // Return only unambiguous scanner-reported interface source paths. Missing/external
    // providers return no target; no fallback to filenames or textual declarations.
    [[nodiscard]] std::vector<std::string> interfaceTargets(const CppModuleRequire& dependency) const;
};
// Strict P1689 revision 0/version 1 decoder; bounded O(report + dependency count).
// Malformed/ambiguous records fail the whole report. Unknown extension fields are tolerated.
[[nodiscard]] Result<CppModuleGraph> parseCppModuleGraph(std::string_view report);
// Validate the real llvm::cl::PrintVersionMessage banner against the compiled SDK pin.
[[nodiscard]] Result<void> validateCppModuleScannerVersion(std::string_view banner);
struct CppModuleScanRequest {
    std::string scanner;             // Absolute pinned clang-scan-deps, from the application toolchain.
    // Captured CDB in a scratch directory exclusive to this scan. The scanner's derived
    // arguments-form database is created and removed beside it.
    std::string privateDatabasePath;
    std::string databaseBytes;
    std::string workingDirectory;
    bool trusted = false;
    std::stop_token stop;
    IClock::Duration timeout = std::chrono::seconds(60);
    // Required current-context check: project/trust/configuration, CDB and source images.
    // Invoked before execution and after completion; errors discard all results.
    std::function<Result<void>()> validateCurrent;
};
// Blocking worker-only API; executes no compiler/build command. The scanner may execute
// project compiler inputs, so explicit current trust is mandatory. Caller owns captured CDB
// lifetime and validates disk bytes/source snapshots in validateCurrent. Child trees are joined.
// Entries supply an explicit JSON output or use the separate GNU -o or clang-cl /Fo output forms.
[[nodiscard]] Result<CppModuleGraph> scanCppModules(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                    const CppModuleScanRequest& request);
} // namespace frost::lang
