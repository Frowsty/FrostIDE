// SPDX-License-Identifier: Apache-2.0
// CPP-15: tool-backed C++ module dependency graph. No source-text module inference.
#pragma once
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/io/process.hpp"
#include <functional>
#include <map>
#include <chrono>
#include "frost/io/file_system.hpp"
namespace frost::lang {
struct CppModuleProvide { std::string name, sourcePath; bool interface = false; };
struct CppModuleRequire { std::string name, sourcePath, lookupMethod; bool uniqueOnSourcePath = false; };
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
struct CppModuleInputs {
    struct Image { std::string physicalPath, sha256; };
    std::map<std::string, Image> files;
    std::map<std::string, std::vector<io::DirEntry>> directories;
    std::map<std::string, std::string> physicalDirectories;
    std::vector<std::string> absent;
};
// Conservative saved-input profile: snapshot whole project and explicit CDB source/include
// trees, with all config bytes, physical identities and namespace additions/removals.
// Symlink trees, PCH/module/driver config/response inputs and 256MiB/32768-file overflow refuse.
// The installed compiler/system SDK is application-owned; this receipt covers mutable project
// and explicit include inputs. No claim is made about replacing the installed system SDK mid-run.
[[nodiscard]] Result<CppModuleInputs> captureCppModuleInputs(io::IFileSystem& fs, std::string projectRoot,
                                                           std::string_view database, std::stop_token stop = {});
[[nodiscard]] Result<void> validateCppModuleInputs(io::IFileSystem& fs, const CppModuleInputs& inputs,
                                                  std::stop_token stop = {});
struct CppModuleScanRequest {
    std::string scanner; // Absolute pinned clang-scan-deps, from the application toolchain.
    std::string privateDatabasePath; // Captured arguments-form CDB in an owned directory.
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
[[nodiscard]] Result<CppModuleGraph> scanCppModules(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                   const CppModuleScanRequest& request);
} // namespace frost::lang
