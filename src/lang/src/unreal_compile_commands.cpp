// SPDX-License-Identifier: Apache-2.0
// Normalizes Unreal compile commands and stores content-addressed snapshots.
#include "frost/lang/unreal_compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/project/standalone_project.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include <algorithm>
namespace frost::lang {
Result<std::string> normalizeUnrealCompileDatabase(std::string_view bytes, std::string_view originDirectory) {
    if (bytes.size() > 64 * 1024 * 1024 || !io::isAbsolutePath(originDirectory))
        return fail(ErrorCode::InvalidArgument, "Unreal compile database exceeds bounds or has no absolute origin");
    auto sources = project::compilationDatabaseSources(bytes, originDirectory);
    if (!sources) return std::unexpected(sources.error());
    if (sources->empty()) return fail(ErrorCode::Corrupt, "Unreal compilation database contains no commands");
    auto parsed = parseJson(bytes);
    if (!parsed) return std::unexpected(parsed.error());
    std::vector<Json> rows;
    std::size_t outputBytes=2;
    for (const auto& original : parsed->items()) {
        const auto directory = original.stringAt("directory");
        auto absoluteDirectory = io::isAbsolutePath(directory) ? io::normalizePath(directory) : io::joinPath(originDirectory, directory);
        if (!absoluteDirectory) return std::unexpected(absoluteDirectory.error());
        const auto file = original.stringAt("file");
        auto absoluteFile = io::isAbsolutePath(file) ? io::normalizePath(file) : io::joinPath(*absoluteDirectory, file);
        if (!absoluteFile) return std::unexpected(absoluteFile.error());
        if(absoluteDirectory->size()>65536||absoluteFile->size()>65536)return fail(ErrorCode::OutOfRange,"Unreal database path exceeds bounds");
        auto row = Json(original).set("directory", *absoluteDirectory).set("file", *absoluteFile);
        const auto rowBytes=row.dump().size()+1;
        if(rowBytes>64*1024*1024-outputBytes)return fail(ErrorCode::OutOfRange,"normalized Unreal database exceeds 64 MiB");
        outputBytes+=rowBytes;
        rows.push_back(std::move(row));
    }
    return Json::array(std::move(rows)).dump();
}
UnrealCompileCommandsProvider::UnrealCompileCommandsProvider(io::IFileSystem& fs, const project::TrustStore& trust,
    UnrealCompileDatabaseSelection selection, std::string cacheRoot, std::uint64_t trustRevision)
    : m_fs(fs), m_trust(trust), m_selection(std::move(selection)), m_cacheRoot(std::move(cacheRoot)), m_trustRevision(trustRevision) {}
Result<std::optional<CompileCommandsLocation>> UnrealCompileCommandsProvider::locate() { return locate({}); }
Result<std::optional<CompileCommandsLocation>> UnrealCompileCommandsProvider::locate(std::stop_token stop) {
    const auto admitted = [&] {
        const auto snapshot = m_trust.snapshot(io::parentPath(m_selection.projectFile));
        return snapshot.trusted && snapshot.revision == m_trustRevision;
    };
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "Unreal database selection cancelled");
    if (!io::isAbsolutePath(m_selection.projectFile) || !io::isAbsolutePath(m_selection.databaseFile) ||
        !io::isAbsolutePath(m_cacheRoot) || io::fileName(m_selection.databaseFile) != "compile_commands.json" ||
        m_selection.configurationIdentity.empty())
        return fail(ErrorCode::InvalidArgument, "select explicit Unreal profile/database and an IDE cache directory");
    if (!admitted()) return fail(ErrorCode::Untrusted, "Unreal database trust revision changed");
    auto reader = m_fs.openRandomAccess(m_selection.databaseFile);
    if (!reader) return std::unexpected(reader.error());
    if ((*reader)->size() > 64 * 1024 * 1024) return fail(ErrorCode::OutOfRange, "Unreal database exceeds 64 MiB");
    std::string bytes;
    while (bytes.size() < (*reader)->size()) {
        auto chunk = (*reader)->read(bytes.size(), static_cast<std::size_t>(std::min<std::uint64_t>(
            io::IRandomAccessReader::kMaximumReadBytes, (*reader)->size() - bytes.size())), stop);
        if (!chunk) return std::unexpected(chunk.error());
        if (chunk->empty()) return fail(ErrorCode::Busy, "Unreal database changed during read");
        bytes += *chunk;
    }
    auto normalized = normalizeUnrealCompileDatabase(bytes, io::parentPath(m_selection.databaseFile));
    if (!normalized) return std::unexpected(normalized.error());
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "Unreal database selection cancelled");
    if (!admitted()) return fail(ErrorCode::Untrusted, "Unreal database trust revision changed");
    const auto identity = m_selection.projectFile + "\n" + m_selection.databaseFile + "\n" + m_selection.configurationIdentity + "\n" + *normalized;
    auto directory = io::joinPath(m_cacheRoot, "unreal-" + io::sha256Hex(identity));
    if (!directory) return std::unexpected(directory.error());
    auto file = io::joinPath(*directory, "compile_commands.json");
    if (!file) return std::unexpected(file.error());
    if (auto created = m_fs.createDirectories(*directory); !created) return std::unexpected(created.error());
    const auto existing = m_fs.readFileSnapshot(*file,64*1024*1024);
    if (existing && *existing != *normalized) return fail(ErrorCode::Corrupt, "immutable Unreal database cache was modified");
    if (!existing) {
        if (existing.error().code() != ErrorCode::NotFound) return std::unexpected(existing.error());
        if (auto written = m_fs.writeFileAtomic(*file, *normalized); !written) return std::unexpected(written.error());
    }
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "Unreal database selection cancelled");
    if (!admitted()) return fail(ErrorCode::Untrusted, "Unreal database trust revision changed");
    return std::optional<CompileCommandsLocation>{CompileCommandsLocation{*directory, *file, CompileCommandsSource::Existing, {}, m_selection.databaseFile}};
}
} // namespace frost::lang
