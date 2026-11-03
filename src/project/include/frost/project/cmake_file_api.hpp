// SPDX-License-Identifier: Apache-2.0
//
// The CMake File API (cmake-file-api(7)): FrostIDE's stateful query and the CMake project model
// read from its reply: targets with their artifacts, sources (generated ones flagged), include
// directories, defines and compile flags per language, the CMake input files, the cache and the
// toolchains (PLAN.md section 2, "CMake"; ADR 0046). Reading a reply runs nothing, so a folder's
// existing replies are shown before it is trusted (PLAN.md section 3.5, restricted mode).
//
/*
 * Query. writeCMakeFileApiQuery writes <build>/.cmake/api/v1/query/client-frostide/query.json
 * requesting codemodel v2, cache v2, cmakeFiles v1 and toolchains v1. CMake answers it at the end
 * of every configure and generate step, in <build>/.cmake/api/v1/reply/.
 *
 * Reply. The current index is the reply directory's index-*.json with the greatest name. Its
 * reply["client-frostide"]["query.json"].responses name the object files; when FrostIDE's client
 * has no entry (another IDE configured the directory), the index's "objects" list is used, so a
 * CLion or Visual Studio build directory still shows its targets. An object whose response is an
 * error, or that is missing, leaves its part of the model empty; only a missing or unreadable
 * codemodel is an error.
 *
 * Paths in the model are absolute, native and normalised. The File API writes paths with '/'
 * separators, relative to the top-level source directory (sources, inputs, target directories) or
 * to the top-level build directory (artifacts) when they lie inside it; both are resolved here.
 *
 * Thread safety: pure functions over the file system; the model is a plain immutable value.
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

// The File API client name FrostIDE's query uses.
inline constexpr std::string_view kCMakeFileApiClient = "client-frostide";

enum class CMakeTargetType : std::uint8_t {
    Executable,
    StaticLibrary,
    SharedLibrary,
    ModuleLibrary,
    ObjectLibrary,
    InterfaceLibrary,
    Utility, // add_custom_target, and the targets CMake generates itself.
};

// "EXECUTABLE", "STATIC_LIBRARY", ... as the File API spells them.
[[nodiscard]] std::string_view toString(CMakeTargetType type) noexcept;

// One frame of a File API backtrace, ordered from the reported command outwards.
struct CMakeBacktraceFrame {
    std::string file{};                // Absolute source path.
    std::optional<std::size_t> line{}; // One-based when supplied by CMake.
    std::string command{};

    friend bool operator==(const CMakeBacktraceFrame&, const CMakeBacktraceFrame&) = default;
};

struct CMakeSource {
    std::string path{};                        // Absolute.
    bool generated = false;                    // isGenerated: produced by the build ("Build to generate").
    std::string sourceGroup{};                 // source_group() name ("Source Files"), or "".
    std::optional<std::size_t> compileGroup{}; // Index into CMakeTarget::compileGroups; empty for headers.

    std::vector<std::vector<CMakeBacktraceFrame>> backtraces{}; // Empty when missing or invalid.

    friend bool operator==(const CMakeSource&, const CMakeSource&) = default;
};

// The flags one group of a target's sources is compiled with.
struct CMakeCompileGroup {
    std::string language{};                    // "C", "CXX", "RC", ...
    std::vector<std::string> includes{};       // Absolute include directories, in order.
    std::vector<std::string> systemIncludes{}; // Those of `includes` marked isSystem.
    std::vector<std::string> defines{};        // "NAME" or "NAME=VALUE".
    std::vector<std::string> flags{};          // compileCommandFragments, each as written.
    std::string languageStandard{};            // "17" (languageStandard.standard), or "".

    friend bool operator==(const CMakeCompileGroup&, const CMakeCompileGroup&) = default;
};

struct CMakeTarget {
    std::string name{};
    std::string id{}; // Unique in the configuration ("app::@6890427a1f51a3e7e1df").
    CMakeTargetType type = CMakeTargetType::Utility;
    std::vector<std::string> artifacts{}; // Absolute output files (executables, libraries, PDBs).
    std::string sourceDirectory{};        // Absolute: the directory whose CMakeLists.txt defines it.
    std::string folder{};                 // The FOLDER property, or "".
    std::vector<CMakeSource> sources{};   // In the File API's order.
    std::vector<CMakeCompileGroup> compileGroups{};
    std::vector<std::string> dependencies{}; // Ids of the targets it depends on.
    bool generatorProvided = false;          // A target CMake adds itself (ALL_BUILD, ZERO_CHECK).

    std::vector<CMakeBacktraceFrame> backtrace{}; // Empty when missing or invalid.

    friend bool operator==(const CMakeTarget&, const CMakeTarget&) = default;
};

struct CMakeInputFile {
    std::string path{};     // Absolute.
    bool generated = false; // Written by CMake into the build tree.
    bool external = false;  // Outside the source and build trees (CMake's modules, toolchains).
    bool cmake = false;     // Part of CMake's own installation.

    friend bool operator==(const CMakeInputFile&, const CMakeInputFile&) = default;
};

struct CMakeCacheEntry {
    std::string name{};
    std::string type{}; // "STRING", "BOOL", "PATH", "FILEPATH", "INTERNAL", ...
    std::string value{};

    friend bool operator==(const CMakeCacheEntry&, const CMakeCacheEntry&) = default;
};

struct CMakeToolchain {
    std::string language{};        // "C", "CXX".
    std::string compilerPath{};    // Absolute, or "" when CMake did not report it.
    std::string compilerId{};      // "MSVC", "Clang", "GNU".
    std::string compilerVersion{}; // "19.44.35207.1".

    friend bool operator==(const CMakeToolchain&, const CMakeToolchain&) = default;
};

// One configured CMake build directory, as the reply describes it.
struct CMakeProject {
    std::string sourceDirectory{}; // Absolute.
    std::string buildDirectory{};  // Absolute.
    std::string generator{};       // "Ninja", or "" when the index does not say.
    bool multiConfig = false;
    std::string cmakeVersion{};  // "4.4.5".
    std::string name{};          // The top-level project() name, or "".
    std::string configuration{}; // The configuration the targets below come from ("Debug"; "" when unnamed).
    std::vector<std::string> configurations{}; // Every configuration of the reply, in order.
    std::vector<CMakeTarget> targets{};        // Of `configuration`, in the File API's order.
    std::vector<CMakeInputFile> inputs{};
    std::vector<CMakeCacheEntry> cache{};
    std::vector<CMakeToolchain> toolchains{};
    std::string replyIndex{};     // The index file read (absolute), which identifies the reply.
    bool inputsAvailable = false; // cmakeFiles input list was present and structurally valid.
    bool cacheAvailable = false;  // cache entries were present and structurally valid.

    // The target named `name`, or nullptr. O(targets).
    [[nodiscard]] const CMakeTarget* findTarget(std::string_view name) const noexcept;
    // The value of cache entry `name`, or nullopt. O(entries).
    [[nodiscard]] std::optional<std::string> cacheValue(std::string_view name) const;
    // The inputs a change to which needs a reconfigure: those that are neither generated nor
    // external (CMakeLists.txt and the project's own *.cmake files).
    [[nodiscard]] std::vector<std::string> projectInputs() const;

    friend bool operator==(const CMakeProject&, const CMakeProject&) = default;
};

// <buildDirectory>/.cmake/api/v1/query/client-frostide/query.json (no I/O). Errors: InvalidArgument.
[[nodiscard]] Result<std::string> cmakeFileApiQueryPath(std::string_view buildDirectory);

// Creates the query directory and writes the query (header comment) unless it already holds
// exactly that text. Errors: InvalidArgument (not absolute), the file system's errors.
[[nodiscard]] Result<void> writeCMakeFileApiQuery(io::IFileSystem& fs, std::string_view buildDirectory);

/*
 * Reads the current reply of `buildDirectory` (absolute). `configuration` picks the configuration
 * of a multi-config reply (ASCII case ignored); empty takes the first. nullopt when there is no
 * reply directory or no index file (the directory was never configured with a File API query).
 * Errors: InvalidArgument (not absolute), NotFound (no such configuration), Corrupt (an index or
 * codemodel that does not parse or lacks required members), the file system's read errors.
 * O(total reply size).
 */
[[nodiscard]] Result<std::optional<CMakeProject>>
readCMakeFileApiReply(io::IFileSystem& fs, std::string_view buildDirectory, std::string_view configuration = {});

} // namespace frost::project
