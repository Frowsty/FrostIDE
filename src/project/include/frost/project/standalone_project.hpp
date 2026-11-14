// SPDX-License-Identifier: Apache-2.0
// Non-generating C++ folder projects: compilation databases and GNU Make entry points (CMK-17).
#pragma once

#include "frost/core/error.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
}
namespace frost::project {

// Immutable, thread-compatible snapshot. Reading never evaluates Make recipes or launches a tool.
// A database describes translation units, not link products: run configurations remain explicit.
struct StandaloneProject {
    std::string root;
    std::optional<std::string> makefile;
    std::optional<std::string> database;
    std::vector<std::string> sources;
};

// Recognizes GNUmakefile, makefile, Makefile (GNU Make precedence), and a database at the root
// or build/compile_commands.json. Missing markers return nullopt; malformed databases return
// Corrupt when readSources is true. GUI callers use false for marker-only discovery; clangd reads
// its database on its worker. Blocking I/O, O(database bytes) or O(1) markers, no writes/execution.
[[nodiscard]] Result<std::optional<StandaloneProject>> readStandaloneProject(io::IFileSystem& fs, std::string_view root,
                                                                             bool readSources = true);

// Validates a database and resolves its file entries against their directory (relative directories
// resolve against the database directory). Accepts command or nonempty arguments; neither executes.
[[nodiscard]] Result<std::vector<std::string>> compilationDatabaseSources(std::string_view json,
                                                                          std::string_view directory);
// Resolves the unique object output for a source from an actual database (output or -o /Fo).
// Returns a build-directory-relative Ninja target. Missing/ambiguous/outside outputs refuse;
// compiler argv is never executed or guessed. O(database bytes).
[[nodiscard]] Result<std::string> compilationDatabaseObjectTarget(std::string_view json, std::string_view directory,
                                                                  std::string_view source);

// Pure argv production. Targets are literal arguments, never shell fragments. A leading '-' or
// variable assignment is refused rather than letting a target alter Make's invocation policy.
enum class MakeAction { Build, Rebuild, Clean, Install };
[[nodiscard]] Result<std::vector<std::vector<std::string>>>
makeProjectArguments(const StandaloneProject& project, MakeAction action, std::string_view target = {});

} // namespace frost::project
