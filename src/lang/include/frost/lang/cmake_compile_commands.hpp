// SPDX-License-Identifier: Apache-2.0
//
// CPP-3: the IDE's own compile-commands directory per CMake configuration, and the resolve context
// of a file listed by several targets. CMK-4's synthesis (frost/project/cmake_compile_database.hpp)
// fills it for the generators that write no compile_commands.json.
/*
 * Why the IDE owns the directory. clangd keeps its background index (the shards) in
 * <compile-commands dir>/.cache/clangd/index, one index per directory, and one clangd serves one
 * database. So every CMake configuration (build directory plus, for a multi-config generator, the
 * configuration) gets a directory of its own under the profile's cache, never in the user's tree:
 *   <cache root>/cmake-<build dir name>[-<configuration>]-<16 hex digits of a hash of both>/
 * and clangd runs with --compile-commands-dir pointing there. A configuration switch is a new
 * directory, so the workbench restarts clangd once, and switching back finds the old shards.
 *
 * What the directory holds (CMakeCompileCommandsProvider::locate):
 *   - the compile_commands.json CMake wrote in the build directory (single-config Ninja and
 *     Makefiles), byte for byte (source CMakeCopy); or, when there is none,
 *   - the commands synthesized from the active reply's codemodel (CMakeCodeModel), when that reply
 *     is of this build directory and configuration; otherwise there is no database yet (nullopt).
 *   - Resolve-context choices go first, one entry per chosen file with the chosen target's command,
 *     so they win over the generator's entries (clangd takes a file's first entry).
 * The file is rewritten only when its content changes, so clangd's reload does not invalidate the
 * index for nothing.
 *
 * Resolve context. A file that several targets list (a header shared by two libraries, a source
 * compiled twice) is parsed with one target's flags. The choice, by target name, is kept per
 * workspace in <cache root>/resolve-contexts.json and applies to every configuration. A running
 * clangd takes it at once through the compilationDatabaseChanges extension of
 * workspace/didChangeConfiguration (contextChange), which makes clangd reparse the open file.
 *
 * Thread safety: every member may be called from any thread. locate() calls serialise; the choice
 * getters and setters never wait for a locate() copying a database (the GUI thread calls them).
 * `input` is called on the calling thread of locate().
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/project/cmake_file_api.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::lang {

// The configuration clangd should read: the active profile's build directory, its configuration
// (multi-config generators; "" otherwise) and the current File API model (null before the first).
struct CMakeCompileCommandsInput {
    std::string buildDirectory;
    std::string configuration;
    std::shared_ptr<const project::CMakeProject> model;
};

// The owned directory of a configuration (header comment). Pure. Errors: InvalidArgument (a root
// or build directory that is not absolute).
[[nodiscard]] Result<std::string> cmakeCompileCommandsDirectory(std::string_view cacheRoot,
                                                                std::string_view buildDirectory,
                                                                std::string_view configuration);

// CMK-4: synthesizeCMakeCompileCommands as database entries. Pure.
[[nodiscard]] std::vector<CompileCommand> cmakeCodeModelCompileCommands(const project::CMakeProject& project);

// Whether `model` describes `buildDirectory` (normalised; ASCII case ignored on Windows) and, for a
// multi-config reply, `configuration` (case ignored; "" accepts any).
[[nodiscard]] bool cmakeModelDescribes(const project::CMakeProject& model, std::string_view buildDirectory,
                                       std::string_view configuration);

// The workspace/didChangeConfiguration params that give clangd `command` for its file.
[[nodiscard]] Json compilationDatabaseChange(const CompileCommand& command);

class CMakeCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    // `fs` must outlive the provider; `cacheRoot` is absolute (the workspace's cache directory).
    CMakeCompileCommandsProvider(io::IFileSystem& fs, std::string cacheRoot,
                                 std::function<CMakeCompileCommandsInput()> input);

    // The owned directory with the configuration's database (header comment); nullopt while there
    // is none. Errors: the file system's (reading CMake's database, writing the copy).
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate() override;

    // The target chosen for `file` ("" when automatic: clangd infers its command). Loads the
    // persisted choices on first use.
    [[nodiscard]] std::string context(std::string_view file);
    // Chooses `target` (a target name; "" for automatic) for `file` and saves the choice. Does not
    // rewrite the database: call locate() for that. Errors: the file system's (saving).
    [[nodiscard]] Result<void> setContext(std::string_view file, std::string target);
    // The command `file` gets under its chosen context in `model`, or nullopt when automatic or the
    // model has no such target or command.
    [[nodiscard]] std::optional<CompileCommand> contextCommand(const project::CMakeProject& model,
                                                               std::string_view file);

private:
    // Both expect m_mutex held.
    void loadContexts();
    [[nodiscard]] std::optional<CompileCommand> contextCommandLocked(const project::CMakeProject& model,
                                                                     std::string_view file);

    io::IFileSystem& m_fs;
    std::string m_cacheRoot;
    std::function<CMakeCompileCommandsInput()> m_input;
    std::mutex m_writeMutex; // locate(): one database write at a time.
    std::mutex m_mutex;      // The choices; never held across database I/O.
    bool m_contextsLoaded = false;
    std::map<std::string, std::pair<std::string, std::string>> m_contexts; // Path key -> (path, target).
};

} // namespace frost::lang
