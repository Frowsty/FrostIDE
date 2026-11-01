// SPDX-License-Identifier: Apache-2.0
//
// Where clangd gets its compile commands from (W2, ADR 0036): the CompileCommandsProvider seam
// and its implementations: an existing compile_commands.json in the usual build directories, a
// compile_commands.json synthesized for a Visual Studio solution from the static project readers,
// a hook for a command list from the user's own generator, and a chain that takes the first
// provider with an answer.
/*
 * The result is always a directory holding compile_commands.json, which ClangdService passes as
 * --compile-commands-dir. Synthesized databases are written to a cache directory the caller owns
 * (never into the user's tree); the file is rewritten only when its content changes, so clangd's
 * background index is not invalidated by a no-op reload.
 *
 * Synthesized commands (VS solutions). For each loaded C++ project that takes part in the active
 * solution configuration, every ClCompile item becomes one clang-cl command in --driver-mode=cl
 * style, from the project configuration's ClCompile metadata (clCompileMetadata in vcxproj.hpp):
 *   AdditionalIncludeDirectories -> /I<abs dir>, PreprocessorDefinitions -> /D<def>,
 *   ForcedIncludeFiles -> /FI<abs file>, LanguageStandard -> /std:c++14|17|20|latest (default
 *   c++14, MSVC's), LanguageStandard_C -> /std:c11|c17 for .c files, RuntimeLibrary -> /MT,
 *   /MTd, /MD, /MDd (default /MDd for configurations named *Debug*, else /MD), ExceptionHandling
 *   -> /EHsc unless "false", ConformanceMode=true -> /permissive-; the configuration's
 *   CharacterSet -> /DUNICODE /D_UNICODE or /D_MBCS; the platform -> --target=<triple>
 *   (Win32 i686, x64 x86_64, ARM64 aarch64); and the MSVC and Windows SDK include directories as
 *   /imsvc<dir> system includes. Entries that still hold an unresolved $(...) or %(...) are
 *   dropped (the static readers cannot evaluate imports; ProjectHost, M2, will).
 *   TODO(M2): commands from ProjectHost's evaluated ClCompile items (path (c), ADR 0006), which
 *   also covers per-file metadata, AdditionalOptions and property sheets.
 *
 * Threads: providers do blocking file I/O; call locate() on a worker (ClangdService does, on the
 * Background lane). The providers here are safe to call from one thread at a time.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/msvc_environment.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {
class SolutionModel;
} // namespace frost::project

namespace frost::lang {

// One entry of a JSON compilation database (clang's JSONCompilationDatabase format).
struct CompileCommand {
    std::string directory;              // Absolute working directory of the command.
    std::string file;                   // Absolute source path.
    std::vector<std::string> arguments; // argv, compiler first.
    friend bool operator==(const CompileCommand&, const CompileCommand&) = default;
};

// The compile_commands.json text for `commands` (one entry per line, "arguments" form). O(size).
[[nodiscard]] std::string compileCommandsJson(std::span<const CompileCommand> commands);

// Writes compileCommandsJson(commands) to `<directory>/compile_commands.json`, creating the
// directory, unless the file already holds exactly that text. Returns whether it wrote.
// Errors: those of IFileSystem::createDirectories, readFile (other than NotFound) and writeFileAtomic.
[[nodiscard]] Result<bool> writeCompileCommands(io::IFileSystem& fs, std::string_view directory,
                                                std::span<const CompileCommand> commands);

enum class CompileCommandsSource : std::uint8_t {
    Existing,    // A compile_commands.json the user's build produced (CMake, Bear, ...).
    Synthesized, // Written by FrostIDE from a Visual Studio solution's static model.
    Generator,   // Written by FrostIDE from a command list the user's generator produced.
};

[[nodiscard]] std::string_view toString(CompileCommandsSource source) noexcept;

struct CompileCommandsLocation {
    std::string directory; // Absolute; holds compile_commands.json (for --compile-commands-dir).
    std::string file;      // <directory>/compile_commands.json.
    CompileCommandsSource source = CompileCommandsSource::Existing;
};

// The seam ClangdService asks before each (re)start.
class ICompileCommandsProvider {
public:
    virtual ~ICompileCommandsProvider() = default;
    // The database to use, or nullopt when this provider has none (clangd then falls back to its
    // default flags). Blocking. Errors: the provider's I/O errors.
    [[nodiscard]] virtual Result<std::optional<CompileCommandsLocation>> locate() = 0;

protected:
    ICompileCommandsProvider() = default;
    ICompileCommandsProvider(const ICompileCommandsProvider&) = default;
    ICompileCommandsProvider& operator=(const ICompileCommandsProvider&) = default;
};

/*
 * The first compile_commands.json under `root` in this order: root itself, build/, then
 * out/build/<any>/ (Visual Studio's CMake layout) and cmake-build-<any>/ (CLion's), each of those
 * in name order. Unreadable directories are skipped. Errors: InvalidArgument (root not absolute).
 */
[[nodiscard]] Result<std::optional<CompileCommandsLocation>> findCompileCommands(io::IFileSystem& fs,
                                                                                 std::string_view root);

// findCompileCommands as a provider. `fs` must outlive it.
class ExistingCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    ExistingCompileCommandsProvider(io::IFileSystem& fs, std::string root) : m_fs(fs), m_root(std::move(root)) {}
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate() override;

private:
    io::IFileSystem& m_fs;
    std::string m_root;
};

struct SynthesisOptions {
    // The solution configuration, "Debug|x64" (ASCII case ignored); empty selects the first.
    std::string configuration;
    // System include directories (findMsvcEnvironment's), each passed as /imsvc<dir>.
    std::vector<std::string> systemIncludeDirectories;
    // argv[0] of every command; clangd picks the driver mode from it and --driver-mode=cl.
    std::string compiler = "clang-cl";
};

// The commands for `model`'s active configuration (see the header comment). Pure. Errors:
// NotFound (the named configuration does not exist), InvalidArgument (no configurations).
[[nodiscard]] Result<std::vector<CompileCommand>> synthesizeCompileCommands(const project::SolutionModel& model,
                                                                            const SynthesisOptions& options);

/*
 * Synthesizes the solution's commands and writes them to `cacheDirectory`. When
 * `options.systemIncludeDirectories` is empty and `msvc` is given, the MSVC and Windows SDK
 * directories are looked up once (findMsvcEnvironment; Windows only) and reused. `fs`, `model`
 * and `msvc`'s launcher must outlive the provider.
 */
class SolutionCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    SolutionCompileCommandsProvider(io::IFileSystem& fs, std::shared_ptr<const project::SolutionModel> model,
                                    std::string cacheDirectory, SynthesisOptions options,
                                    std::function<Result<MsvcEnvironment>()> msvc = {});
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate() override;

private:
    io::IFileSystem& m_fs;
    std::shared_ptr<const project::SolutionModel> m_model;
    std::string m_cacheDirectory;
    SynthesisOptions m_options;
    std::function<Result<MsvcEnvironment>()> m_msvc;
    bool m_msvcLooked = false;
};

/*
 * The hook for a command list from the user's own generator (a build script, Bear, a custom
 * exporter): `generate` returns the commands, which are written to `cacheDirectory` like a
 * synthesized database. An empty list means "no database". TODO(M4): wire the generator
 * configuration (run configuration or setting) to this provider.
 */
class GeneratorCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    GeneratorCompileCommandsProvider(io::IFileSystem& fs, std::string cacheDirectory,
                                     std::function<Result<std::vector<CompileCommand>>()> generate)
        : m_fs(fs), m_cacheDirectory(std::move(cacheDirectory)), m_generate(std::move(generate)) {}
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate() override;

private:
    io::IFileSystem& m_fs;
    std::string m_cacheDirectory;
    std::function<Result<std::vector<CompileCommand>>()> m_generate;
};

// Asks each provider in order and returns the first database found; a provider's error is
// returned only when no later provider has a database.
class FirstAvailableCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    explicit FirstAvailableCompileCommandsProvider(std::vector<std::shared_ptr<ICompileCommandsProvider>> providers)
        : m_providers(std::move(providers)) {}
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate() override;

private:
    std::vector<std::shared_ptr<ICompileCommandsProvider>> m_providers;
};

} // namespace frost::lang
