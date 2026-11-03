// SPDX-License-Identifier: Apache-2.0
//
// Compile-command providers (compile_commands.hpp): writing databases, finding an existing one,
// synthesizing one for a Visual Studio solution, the generator hook and the chain.

#include "frost/lang/compile_commands.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/json.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/vcxproj.hpp"
#include "frost/sched/scheduler.hpp"

#include "msvc_flags.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>
#ifdef _WIN32
#include <windows.h>

#include <winternl.h>
#endif

namespace frost::lang {

Result<std::optional<CompileCommandsLocation>> ICompileCommandsProvider::locate(std::stop_token stop) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "compile commands lookup cancelled");
    auto result = locate();
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "compile commands lookup cancelled");
    return result;
}

namespace {

constexpr std::string_view kDatabaseName = "compile_commands.json";

using namespace detail; // msvc_flags.hpp

[[nodiscard]] Result<std::optional<CompileCommandsLocation>> writeToCache(io::IFileSystem& fs,
                                                                          std::string_view cacheDirectory,
                                                                          std::span<const CompileCommand> commands,
                                                                          CompileCommandsSource source) {
    if (commands.empty()) {
        return std::optional<CompileCommandsLocation>();
    }
    auto directory = io::normalizePath(cacheDirectory);
    if (!directory) {
        return std::unexpected(std::move(directory.error()));
    }
    if (auto written = writeCompileCommands(fs, *directory, commands); !written) {
        return std::unexpected(std::move(written.error()));
    }
    auto file = io::joinPath(*directory, kDatabaseName);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    return std::optional<CompileCommandsLocation>(
        CompileCommandsLocation{.directory = std::move(*directory), .file = std::move(*file), .source = source});
}

// Directory listings come in file-system order; candidates are tried in name order.
void sortByName(std::vector<io::DirEntry>& entries) { std::ranges::sort(entries, {}, &io::DirEntry::name); }

} // namespace

GeneratedCommandsLease::GeneratedCommandsLease(io::IFileSystem& fs, std::string boundary, std::string directory,
                                               std::vector<std::string> ownedParents,
                                               std::shared_ptr<GeneratedCommandsRetirement> retirement,
                                               std::function<void()> beforeDeleteForTesting,
                                               PendingProbe pendingForTesting)
    : m_fs(&fs == &io::realFileSystem() ? &fs : nullptr), m_boundary(std::move(boundary)),
      m_directory(std::move(directory)), m_ownedParents(std::move(ownedParents)), m_retirement(retirement),
      m_beforeDeleteForTesting(std::move(beforeDeleteForTesting)), m_pendingForTesting(pendingForTesting) {}

namespace {
sched::Task<void> runGeneratedRetirement(std::shared_ptr<GeneratedCommandsRetirement> owner,
                                         std::function<void()> task) {
    task();
    owner->finished();
    co_return;
}
} // namespace
GeneratedCommandsRetirement::GeneratedCommandsRetirement(sched::IScheduler& scheduler) : m_scheduler(&scheduler) {}
void GeneratedCommandsRetirement::close() {
    std::lock_guard lock(m_mutex);
    m_scheduler = nullptr;
}
void GeneratedCommandsRetirement::submit(std::function<void()> task) {
    std::lock_guard lock(m_mutex);
    if (m_scheduler) {
        ++m_pending;
        m_scheduler->spawn(sched::Lane::Background, runGeneratedRetirement(shared_from_this(), std::move(task)))
            .detach();
    }
}
void GeneratedCommandsRetirement::finished() {
    std::lock_guard lock(m_mutex);
    if (--m_pending == 0)
        m_idle.notify_all();
}
void GeneratedCommandsRetirement::waitForIdle() {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] { return m_pending == 0; });
}

GeneratedCommandsLease::~GeneratedCommandsLease() {
    if (!m_fs || m_uses || m_uncertain)
        return;
    auto retirement = m_retirement.lock();
    if (!retirement)
        return;
    retirement->submit([boundary = std::move(m_boundary), directory = std::move(m_directory),
                        ownedParents = std::move(m_ownedParents), beforeDelete = std::move(m_beforeDeleteForTesting),
                        pendingProbe = m_pendingForTesting] {
        static_cast<void>(pendingProbe);
        static_cast<void>(ownedParents);
        static_cast<void>(beforeDelete);
        if (!io::isAbsolutePath(directory) || !io::isAbsolutePath(boundary) || directory == boundary)
            return;
        std::vector<std::string> components;
        for (std::string current = directory; current != boundary;) {
            const std::string parent(io::parentPath(current));
            if (parent.empty() || parent == current)
                return;
            components.push_back(current);
            current = parent;
        }
#ifdef _WIN32
        // Resolve each component relative to an already opened directory handle. An
        // attribute-only writer can retarget a directory even while sharing denies writes.
        struct Handle {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Handle() {
                if (value != INVALID_HANDLE_VALUE)
                    CloseHandle(value);
            }
            Handle() = default;
            explicit Handle(HANDLE h) : value(h) {}
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
            Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
            Handle& operator=(Handle&& other) noexcept {
                if (this != &other) {
                    if (value != INVALID_HANDLE_VALUE)
                        CloseHandle(value);
                    value = std::exchange(other.value, INVALID_HANDLE_VALUE);
                }
                return *this;
            }
        };
        using CreateRelative = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                                PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto createRelative =
            ntdll ? reinterpret_cast<CreateRelative>(GetProcAddress(ntdll, "NtCreateFile")) : nullptr;
        if (!createRelative)
            return;
        struct Request {
            std::wstring name;
            UNICODE_STRING unicode{};
            OBJECT_ATTRIBUTES attributes{};
            IO_STATUS_BLOCK status{};
            HANDLE parent = nullptr;
            HANDLE handle = nullptr;
            ~Request() {
                if (handle && handle != INVALID_HANDLE_VALUE)
                    CloseHandle(handle);
                if (parent && parent != INVALID_HANDLE_VALUE)
                    CloseHandle(parent);
            }
        };
        NTSTATUS openStatus = 0;
        const auto openChild = [&](HANDLE parent, std::string_view component, bool directory,
                                   bool mayDelete) -> Handle {
            auto name = io::toWide(component);
            if (!name || name->empty() || name->size() > 32766 || name->find_first_of(L"\\/:") != std::wstring::npos ||
                name->find(L'\0') != std::wstring::npos || *name == L"." || *name == L"..")
                return {};
            auto request = std::make_unique<Request>();
            request->name = std::move(*name);
            if (!DuplicateHandle(GetCurrentProcess(), parent, GetCurrentProcess(), &request->parent, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS))
                return {};
            request->unicode.Buffer = request->name.data();
            request->unicode.Length = static_cast<USHORT>(request->name.size() * sizeof(wchar_t));
            request->unicode.MaximumLength = request->unicode.Length;
            request->attributes.Length = sizeof(OBJECT_ATTRIBUTES);
            request->attributes.RootDirectory = request->parent;
            request->attributes.ObjectName = &request->unicode;
            request->attributes.Attributes = OBJ_DONT_REPARSE;
            request->status.Status = 0x103;
            const ACCESS_MASK access = FILE_READ_ATTRIBUTES | SYNCHRONIZE | (mayDelete ? DELETE : 0);
            const ULONG options = FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT |
                                  (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
            if (pendingProbe != PendingProbe::None) {
                request->handle = CreateEventW(nullptr, TRUE, pendingProbe == PendingProbe::StillPending, nullptr);
                if (!request->handle)
                    return {};
                request->status.Status = 0x103;
                openStatus = 0x103;
            } else {
                openStatus = createRelative(&request->handle, access, &request->attributes, &request->status, nullptr,
                                            FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                            options, nullptr, 0);
            }
            if (openStatus == 0x103) {
                // On an anomalous pending open, retain all kernel-visible buffers unless
                // completion is confirmed. Never free an IOSB still owned by the kernel.
                if (!request->handle || request->handle == INVALID_HANDLE_VALUE ||
                    WaitForSingleObject(request->handle, 0) != WAIT_OBJECT_0) {
                    (void)request.release();
                    return {};
                }
                openStatus = request->status.Status;
                if (openStatus == 0x103) {
                    (void)request.release();
                    return {};
                }
            }
            Handle handle(request->handle ? std::exchange(request->handle, nullptr) : INVALID_HANDLE_VALUE);
            if (openStatus < 0 || handle.value == INVALID_HANDLE_VALUE)
                return {};
            FILE_ATTRIBUTE_TAG_INFO info{};
            if (GetFileType(handle.value) != FILE_TYPE_DISK ||
                !GetFileInformationByHandleEx(handle.value, FileAttributeTagInfo, &info, sizeof(info)) ||
                (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                static_cast<bool>(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
                return {};
            return handle;
        };
        auto boundaryWide = io::toWin32Path(boundary);
        if (!boundaryWide || boundaryWide->size() < 7 || boundaryWide->substr(0, 4) != L"\\\\?\\" ||
            (*boundaryWide)[5] != L':' || (*boundaryWide)[6] != L'\\')
            return;
        const std::wstring volume = boundaryWide->substr(0, 7);
        std::vector<Handle> handles;
        handles.emplace_back(CreateFileW(volume.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (handles.back().value == INVALID_HANDLE_VALUE)
            return;
        FILE_ATTRIBUTE_TAG_INFO volumeInfo{};
        if (!GetFileInformationByHandleEx(handles.back().value, FileAttributeTagInfo, &volumeInfo,
                                          sizeof(volumeInfo)) ||
            !(volumeInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (volumeInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return;
        std::vector<std::string> boundaryChain;
        for (std::string current = boundary;;) {
            const std::string parent(io::parentPath(current));
            if (parent.empty() || parent == current)
                break;
            boundaryChain.push_back(current);
            current = parent;
        }
        for (auto it = boundaryChain.rbegin(); it != boundaryChain.rend(); ++it) {
            handles.push_back(openChild(handles.back().value, io::fileName(*it), true, false));
            if (handles.back().value == INVALID_HANDLE_VALUE)
                return;
        }
        for (auto it = components.rbegin(); it != components.rend(); ++it) {
            handles.push_back(openChild(handles.back().value, io::fileName(*it), true, true));
            if (handles.back().value == INVALID_HANDLE_VALUE)
                return;
        }
        if (beforeDelete)
            beforeDelete();
        auto fileHandle = openChild(handles.back().value, "compile_commands.json", false, true);
        if (fileHandle.value != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(fileHandle.value, FileDispositionInfo, &disposition, sizeof(disposition)))
                return;
            fileHandle = Handle{};
        } else if (static_cast<unsigned long>(openStatus) != 0xC0000034UL) {
            return;
        }
        std::vector<std::string> toRemove{directory};
        toRemove.insert(toRemove.end(), ownedParents.begin(), ownedParents.end());
        for (std::size_t i = 0; i < toRemove.size(); ++i) {
            if (i >= components.size() || toRemove[i] != components[i])
                return;
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(handles.back().value, FileDispositionInfo, &disposition,
                                            sizeof(disposition)))
                return;
            handles.pop_back();
        }
#endif
    });
}

void GeneratedCommandsLease::beginUse() {
    std::lock_guard lock(m_mutex);
    ++m_uses;
}
void GeneratedCommandsLease::confirmExit() {
    std::lock_guard lock(m_mutex);
    if (m_uses)
        --m_uses;
}
void GeneratedCommandsLease::abandonUse() {
    std::lock_guard lock(m_mutex);
    m_uncertain = true;
    if (m_uses)
        --m_uses;
}

std::string compileCommandsJson(std::span<const CompileCommand> commands) {
    std::string out = "[\n";
    for (std::size_t i = 0; i < commands.size(); ++i) {
        Json arguments = Json::array();
        for (const std::string& argument : commands[i].arguments) {
            arguments.push(argument);
        }
        out += "  ";
        out += Json::object()
                   .set("directory", commands[i].directory)
                   .set("file", commands[i].file)
                   .set("arguments", std::move(arguments))
                   .dump();
        out += i + 1 < commands.size() ? ",\n" : "\n";
    }
    out += "]\n";
    return out;
}

Result<bool> writeCompileCommands(io::IFileSystem& fs, std::string_view directory,
                                  std::span<const CompileCommand> commands) {
    if (auto created = fs.createDirectories(directory); !created) {
        return std::unexpected(std::move(created.error()));
    }
    auto path = io::joinPath(directory, kDatabaseName);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    const std::string text = compileCommandsJson(commands);
    auto existing = fs.readFile(*path);
    if (existing && *existing == text) {
        return false;
    }
    if (!existing && existing.error().code() != ErrorCode::NotFound) {
        return std::unexpected(std::move(existing.error()));
    }
    if (auto written = fs.writeFileAtomic(*path, text); !written) {
        return std::unexpected(std::move(written.error()));
    }
    return true;
}

std::string_view toString(CompileCommandsSource source) noexcept {
    switch (source) {
    case CompileCommandsSource::Existing: return "existing";
    case CompileCommandsSource::Synthesized: return "synthesized";
    case CompileCommandsSource::Generator: return "generator";
    case CompileCommandsSource::Evaluated: return "evaluated";
    }
    return "existing";
}

Result<std::optional<CompileCommandsLocation>> findCompileCommands(io::IFileSystem& fs, std::string_view root) {
    auto base = io::normalizePath(root);
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }
    std::vector<std::string> candidates{*base};
    if (auto build = io::joinPath(*base, "build")) {
        candidates.push_back(std::move(*build));
    }
    // out/build/<preset>/ (Visual Studio's CMake layout).
    if (auto outBuild =
            io::joinPath(*base, io::kNativePathStyle == io::PathStyle::Windows ? "out\\build" : "out/build")) {
        if (auto entries = fs.listDirectory(*outBuild)) {
            sortByName(*entries);
            for (const io::DirEntry& entry : *entries) {
                if (entry.type == io::FileType::Directory) {
                    if (auto dir = io::joinPath(*outBuild, entry.name)) {
                        candidates.push_back(std::move(*dir));
                    }
                }
            }
        }
    }
    // cmake-build-<profile>/ (CLion's layout).
    if (auto entries = fs.listDirectory(*base)) {
        sortByName(*entries);
        for (const io::DirEntry& entry : *entries) {
            if (entry.type == io::FileType::Directory && entry.name.starts_with("cmake-build-")) {
                if (auto dir = io::joinPath(*base, entry.name)) {
                    candidates.push_back(std::move(*dir));
                }
            }
        }
    }
    for (const std::string& directory : candidates) {
        auto file = io::joinPath(directory, kDatabaseName);
        if (!file) {
            continue;
        }
        auto status = fs.status(*file);
        if (status && status->type == io::FileType::Regular) {
            return std::optional<CompileCommandsLocation>(CompileCommandsLocation{
                .directory = directory, .file = std::move(*file), .source = CompileCommandsSource::Existing});
        }
    }
    return std::optional<CompileCommandsLocation>();
}

Result<std::optional<CompileCommandsLocation>> ExistingCompileCommandsProvider::locate() {
    return findCompileCommands(m_fs, m_root);
}

Result<std::optional<CompileCommandsLocation>> BuildDirectoryCompileCommandsProvider::locate() {
    const std::string directory = m_buildDirectory ? m_buildDirectory() : std::string();
    if (directory.empty()) {
        return std::optional<CompileCommandsLocation>();
    }
    auto base = io::normalizePath(directory);
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }
    auto file = io::joinPath(*base, kDatabaseName);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    auto status = m_fs.status(*file);
    if (!status || status->type != io::FileType::Regular) {
        return std::optional<CompileCommandsLocation>();
    }
    return std::optional<CompileCommandsLocation>(CompileCommandsLocation{
        .directory = *base, .file = std::move(*file), .source = CompileCommandsSource::Existing});
}

Result<std::size_t> resolveConfigurationIndex(const project::SolutionModel& model, std::string_view configuration) {
    if (model.configurations().empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("{} has no solution configurations", model.path()));
    }
    if (configuration.empty()) {
        return std::size_t{0};
    }
    const std::optional<std::size_t> found = model.findConfiguration(configuration);
    if (!found) {
        return fail(ErrorCode::NotFound, std::format("{} has no configuration '{}'", model.path(), configuration));
    }
    return *found;
}

std::vector<CompileCommand> synthesizeProjectCompileCommands(const project::SolutionModel& model, project::ProjectId id,
                                                             std::size_t configurationIndex,
                                                             const SynthesisOptions& options) {
    std::vector<CompileCommand> out;
    const project::ProjectInfo& info = model.project(id);
    if (!info.vcxproj) {
        return out;
    }
    const std::optional<project::ProjectConfigurationMapping>& mapping =
        model.projectConfiguration(id, configurationIndex);
    if (!mapping) {
        return out;
    }
    const project::VcxProject& vcx = *info.vcxproj;
    const std::string configName = mapping->configuration + '|' + mapping->platform;
    const auto metadata = project::clCompileMetadata(vcx, configName);
    std::string characterSet;
    for (const project::ProjectConfiguration& config : vcx.configurations) {
        if (iequals(config.name(), configName)) {
            characterSet = config.characterSet;
        }
    }

    std::vector<std::string> common{options.compiler, "--driver-mode=cl", std::string(targetFor(mapping->platform)),
                                    runtimeFlag(metadataOf(metadata, "runtimelibrary"), mapping->configuration)};
    if (!iequals(metadataOf(metadata, "exceptionhandling"), "false")) {
        common.emplace_back("/EHsc");
    }
    if (iequals(metadataOf(metadata, "conformancemode"), "true")) {
        common.emplace_back("/permissive-");
    }
    if (iequals(characterSet, "Unicode")) {
        common.emplace_back("/DUNICODE");
        common.emplace_back("/D_UNICODE");
    } else if (iequals(characterSet, "MultiByte")) {
        common.emplace_back("/D_MBCS");
    }
    for (const std::string& define : resolvedEntries(metadataOf(metadata, "preprocessordefinitions"))) {
        common.push_back("/D" + define);
    }
    for (const std::string& entry : resolvedEntries(metadataOf(metadata, "additionalincludedirectories"))) {
        if (auto dir = resolvePath(vcx.directory, entry)) {
            common.push_back("/I" + *dir);
        }
    }
    for (const std::string& dir : options.systemIncludeDirectories) {
        common.push_back("/imsvc" + dir);
    }
    for (const std::string& entry : resolvedEntries(metadataOf(metadata, "forcedincludefiles"))) {
        if (auto file = resolvePath(vcx.directory, entry)) {
            common.push_back("/FI" + *file);
        }
    }
    const std::string cxxStd = cxxStandard(metadataOf(metadata, "languagestandard"));
    const std::optional<std::string> cStd = cStandard(metadataOf(metadata, "languagestandard_c"));

    for (const project::ProjectItem& item : vcx.items) {
        if (item.type != project::ItemType::ClCompile) {
            continue;
        }
        CompileCommand command{.directory = vcx.directory, .file = item.path, .arguments = common};
        if (isCSource(item.path)) {
            if (cStd) {
                command.arguments.push_back(*cStd);
            }
        } else {
            command.arguments.push_back(cxxStd);
        }
        command.arguments.emplace_back("/c");
        command.arguments.push_back(item.path);
        out.push_back(std::move(command));
    }
    return out;
}

Result<std::vector<CompileCommand>> synthesizeCompileCommands(const project::SolutionModel& model,
                                                              const SynthesisOptions& options) {
    auto configurationIndex = resolveConfigurationIndex(model, options.configuration);
    if (!configurationIndex) {
        return std::unexpected(std::move(configurationIndex.error()));
    }
    std::vector<CompileCommand> out;
    for (std::size_t p = 0; p < model.projects().size(); ++p) {
        std::vector<CompileCommand> project = synthesizeProjectCompileCommands(
            model, project::ProjectId{static_cast<std::uint32_t>(p)}, *configurationIndex, options);
        out.insert(out.end(), std::make_move_iterator(project.begin()), std::make_move_iterator(project.end()));
    }
    return out;
}

Result<std::optional<CompileCommandsLocation>> writeCompileCommandsLocation(io::IFileSystem& fs,
                                                                            std::string_view cacheDirectory,
                                                                            std::span<const CompileCommand> commands,
                                                                            CompileCommandsSource source) {
    return writeToCache(fs, cacheDirectory, commands, source);
}

SolutionCompileCommandsProvider::SolutionCompileCommandsProvider(io::IFileSystem& fs,
                                                                 std::shared_ptr<const project::SolutionModel> model,
                                                                 std::string cacheDirectory, SynthesisOptions options,
                                                                 std::function<Result<MsvcEnvironment>()> msvc)
    : m_fs(fs), m_model(std::move(model)), m_cacheDirectory(std::move(cacheDirectory)), m_options(std::move(options)),
      m_msvc(std::move(msvc)) {}

Result<std::optional<CompileCommandsLocation>> SolutionCompileCommandsProvider::locate() {
    if (m_options.systemIncludeDirectories.empty() && m_msvc && !m_msvcLooked) {
        m_msvcLooked = true;
        if (auto environment = m_msvc()) {
            m_options.systemIncludeDirectories = std::move(environment->includeDirectories);
        } // Without MSVC the commands still parse code that avoids the Windows headers.
    }
    auto commands = synthesizeCompileCommands(*m_model, m_options);
    if (!commands) {
        return std::unexpected(std::move(commands.error()));
    }
    return writeToCache(m_fs, m_cacheDirectory, *commands, CompileCommandsSource::Synthesized);
}

Result<std::optional<CompileCommandsLocation>> GeneratorCompileCommandsProvider::locate() {
    if (!m_generate) {
        return std::optional<CompileCommandsLocation>();
    }
    auto commands = m_generate();
    if (!commands) {
        return std::unexpected(std::move(commands.error()));
    }
    return writeToCache(m_fs, m_cacheDirectory, *commands, CompileCommandsSource::Generator);
}

Result<std::optional<CompileCommandsLocation>> FirstAvailableCompileCommandsProvider::locate() { return locate({}); }

Result<std::optional<CompileCommandsLocation>> FirstAvailableCompileCommandsProvider::locate(std::stop_token stop) {
    std::optional<Error> firstError;
    for (const std::shared_ptr<ICompileCommandsProvider>& provider : m_providers) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "compile commands lookup cancelled");
        if (!provider) {
            continue;
        }
        auto found = provider->locate(stop);
        if (stop.stop_requested() || (!found && found.error().code() == ErrorCode::Cancelled))
            return fail(ErrorCode::Cancelled, "compile commands lookup cancelled");
        if (found && *found) {
            return found;
        }
        if (!found && !firstError) {
            firstError = std::move(found.error());
        }
    }
    if (firstError) {
        return std::unexpected(std::move(*firstError));
    }
    return std::optional<CompileCommandsLocation>();
}

} // namespace frost::lang
