// SPDX-License-Identifier: Apache-2.0
// CPP-8/13 preserves distinct full compile-command identities and captures actual frontend inputs.
#include "semantic_command.hpp"

#include "clang/Driver/Driver.h"

namespace frost::clang_engine {
namespace {
using Iter = llvm::vfs::directory_iterator;
using Entry = llvm::vfs::directory_entry;
using Entries = std::vector<Entry>;

class CapturedDirectoryIterator final : public llvm::vfs::detail::DirIterImpl {
public:
    explicit CapturedDirectoryIterator(std::shared_ptr<const Entries> entries) : m_entries(std::move(entries)) {
        increment();
    }
    std::error_code increment() override {
        CurrentEntry = m_next < m_entries->size() ? (*m_entries)[m_next++] : Entry{};
        return {};
    }

private:
    std::shared_ptr<const Entries> m_entries;
    std::size_t m_next = 0;
};

char entryKind(llvm::sys::fs::file_type type) {
    using Type = llvm::sys::fs::file_type;
    switch (type) {
    case Type::regular_file: return 'f';
    case Type::directory_file: return 'd';
    case Type::symlink_file: return 'l';
    default: return 'o';
    }
}

// Clang's driver uses real SDK installation directories to select its toolchain. Project source
// overlays affect file reads, while they cannot add installations to the on-disk SDK discovery.
class SemanticSnapshotFileSystem final : public semantic::SnapshotFileSystem {
public:
    SemanticSnapshotFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> files,
                               llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> disk)
        : SnapshotFileSystem(std::move(files), 32768, 512ULL * 1024 * 1024, 64ULL * 1024 * 1024),
          m_disk(std::move(disk)) {}

    Iter dir_begin(const llvm::Twine& path, std::error_code& code) override {
        const auto key = keyFor(path);
        if (const auto prior = m_listings.find(key); prior != m_listings.end()) {
            code.clear();
            return Iter(std::make_shared<CapturedDirectoryIterator>(prior->second.entries));
        }
        const auto observed = status(path);
        if (!observed) {
            code = observed.getError();
            return {};
        }
        if (!observed->isDirectory()) {
            code = std::make_error_code(std::errc::not_a_directory);
            return {};
        }
        if (m_listings.size() >= 1024 || !semantic::addWithin(m_bytes, key.size(), semantic::kMainBytes)) {
            error = "Compiler directory discovery exceeds its snapshot budget";
            code = std::make_error_code(std::errc::file_too_large);
            return {};
        }
        auto entries = std::make_shared<Entries>();
        for (auto it = m_disk->dir_begin(path, code); !code && it != Iter{}; it.increment(code)) {
            if (m_count >= 65536 || !semantic::addWithin(m_bytes, it->path().size(), semantic::kMainBytes)) {
                error = "Compiler directory entries exceed their snapshot budget";
                code = std::make_error_code(std::errc::file_too_large);
                return {};
            }
            ++m_count;
            entries->push_back(*it);
        }
        if (code) {
            error = "Compiler directory listing could not be captured: " + key;
            return {};
        }
        std::vector<std::pair<llvm::StringRef, char>> names;
        for (const auto& entry : *entries)
            names.emplace_back(llvm::sys::path::filename(entry.path()), entryKind(entry.type()));
        std::sort(names.begin(), names.end());
        std::string bytes;
        for (const auto& [name, kind] : names) {
            bytes += kind;
            bytes.append(name.data(), name.size());
            bytes += '\0';
        }
        m_listings.emplace(key, Listing{entries, semantic::sha256(bytes)});
        return Iter(std::make_shared<CapturedDirectoryIterator>(std::move(entries)));
    }

    llvm::json::Array directoryListings() const {
        llvm::json::Array result;
        for (const auto& [path, listing] : m_listings)
            result.push_back(llvm::json::Object{{"path", path}, {"sha256", listing.digest}});
        return result;
    }

private:
    struct Listing {
        std::shared_ptr<const Entries> entries;
        std::string digest;
    };
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> m_disk;
    std::map<std::string, Listing> m_listings;
    std::size_t m_count = 0;
    std::size_t m_bytes = 0;
};

bool inside(llvm::StringRef path, llvm::StringRef root) {
    return path == root ||
           (path.starts_with(root) && path.size() > root.size() &&
            (llvm::sys::path::is_separator(root.back()) || llvm::sys::path::is_separator(path[root.size()])));
}
} // namespace
std::optional<SemanticCommand> semanticCommand(const llvm::json::Object& request, llvm::StringRef resource,
                                               std::string& error) {
    using namespace semantic;
    const auto root = request.getString("projectRoot"), database = request.getString("compilationDatabase"),
               configuration = request.getString("configuration");
    const auto ordinal = request.getInteger("commandIndex");
    if (!root || root->empty() || !database || !configuration || configuration->empty() || !ordinal || *ordinal < 0 ||
        database->size() > 32 * 1024 * 1024 || resource.empty()) {
        error = "Expected captured project/CDB/configuration/command index and pinned resources";
        return {};
    }
    SemanticCommand result;
    result.root = absolute(*root, "");
    if (!llvm::sys::path::is_absolute(result.root) || result.root.find('\0') != std::string::npos ||
        configuration->size() > 1024 || configuration->contains('\0')) {
        error = "Invalid project or configuration";
        return {};
    }
    result.configuration = configuration->str();
    result.databaseSha256 = sha256(*database);
    result.commandIndex = static_cast<std::uint64_t>(*ordinal);
    auto captured = llvm::json::parse(*database);
    if (!captured) {
        error = llvm::toString(captured.takeError());
        return {};
    }
    const auto* entries = captured->getAsArray();
    if (!entries || entries->size() > 32768 || static_cast<std::uint64_t>(*ordinal) >= entries->size()) {
        error = "Command ordinal outside captured database";
        return {};
    }
    // CompilationDatabase enumeration is not a JSON-row-order contract. Decode precisely the
    // requested captured row through Clang, which still owns command-string quoting semantics.
    llvm::json::Array selected;
    selected.push_back((*entries)[static_cast<std::size_t>(*ordinal)]);
    std::string selectedBytes;
    llvm::raw_string_ostream selectedStream(selectedBytes);
    selectedStream << llvm::json::Value(std::move(selected));
    selectedStream.flush();
    auto parsed = clang::tooling::JSONCompilationDatabase::loadFromBuffer(
        selectedBytes, error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!parsed)
        return {};
    const auto commands = parsed->getAllCompileCommands();
    if (commands.size() != 1) {
        error = "Captured row does not name one compile command";
        return {};
    }
    result.command = commands.front();
    auto& command = result.command;
    command.Directory = absolute(command.Directory, result.root);
    command.Filename = absolute(command.Filename, command.Directory);
    if (!inside(command.Filename, result.root) || command.CommandLine.empty() || command.CommandLine.size() > 4096 ||
        command.Directory.find('\0') != std::string::npos || command.Filename.find('\0') != std::string::npos) {
        error = "Command is empty, oversized or outside project scope";
        return {};
    }
    llvm::json::Array original;
    std::size_t bytes = 0;
    for (const auto& argument : command.CommandLine) {
        if (argument.find('\0') != std::string::npos || !addWithin(bytes, argument.size(), 1024 * 1024)) {
            error = "Invalid or oversized command arguments";
            return {};
        }
        original.push_back(argument);
    }
    // Hash before rewriting the driver or dropping output flags; -D variants and their working
    // directories remain distinct even when they share the same source file and display label.
    std::string identity;
    llvm::raw_string_ostream stream(identity);
    stream << llvm::json::Value(llvm::json::Object{{"directory", command.Directory},
                                                   {"file", command.Filename},
                                                   {"arguments", std::move(original)},
                                                   {"output", command.Output},
                                                   {"configuration", result.configuration}});
    stream.flush();
    result.identity = sha256(identity);
    const auto driver = llvm::sys::path::filename(command.CommandLine.front());
    const bool clDriver = driver == "cl.exe" || driver == "cl" || driver == "clang-cl.exe" || driver == "clang-cl";
    const bool c =
        driver == "clang" || driver == "clang.exe" || driver == "gcc" || driver == "gcc.exe" || driver == "cc";
    if (!clDriver && !c && driver != "clang++" && driver != "clang++.exe" && driver != "g++" && driver != "g++.exe" &&
        driver != "c++") {
        error = "Compiler wrapper is unsupported; supply its expanded compile command";
        return {};
    }
    llvm::SmallString<256> pinned(resource);
    for (unsigned index = 0; index < 3; ++index)
        llvm::sys::path::remove_filename(pinned);
    llvm::sys::path::append(pinned, "bin", clDriver ? "clang-cl" : c ? "clang" : "clang++");
#ifdef _WIN32
    pinned += ".exe";
#endif
    std::vector<const char*> driverArguments;
    for (std::size_t index = 1; index < command.CommandLine.size(); ++index)
        driverArguments.push_back(command.CommandLine[index].c_str());
    const auto mode = clang::driver::getDriverMode(pinned, driverArguments);
    if (!mode.empty() && mode != "cl" && mode != "gcc" && mode != "g++" && mode != "cpp") {
        error = "Unsupported semantic compiler driver mode";
        return {};
    }
    const bool clMode = clang::driver::IsClangCL(mode);
    std::vector<std::string> arguments{pinned.str().str()};
    for (std::size_t index = 1; index < command.CommandLine.size(); ++index) {
        const llvm::StringRef argument(command.CommandLine[index]);
        if (argument.starts_with("@") || argument == "-Xclang" || argument.starts_with("-Xclang=") ||
            argument.starts_with("-fplugin") || argument.starts_with("-fmodule") || argument.starts_with("/clang:") ||
            argument.starts_with("-save-temps") || argument.starts_with("-ftime-trace") ||
            argument.starts_with("-fprofile") || argument.starts_with("-MJ") || argument == "-include-pch" ||
            argument.starts_with("--serialize-diagnostics") || argument.starts_with("-serialize-diagnostics") ||
            argument.starts_with("-fdiagnostics-serialization-file") || argument.starts_with("--output") ||
            (clMode && (argument.starts_with("/sourceDependencies") || argument.starts_with("/Fi"))) ||
            (clMode && (argument.starts_with("/Yu") || argument.starts_with("/Yc")))) {
            error = "Response/plugin/module/opaque PCH command is not a textual semantic unit";
            return {};
        }
        arguments.push_back(argument.str());
    }
    arguments.push_back("-resource-dir=" + resource.str());
    arguments.push_back(clMode ? "/clang:-fno-implicit-modules" : "-fno-implicit-modules");
    arguments.push_back(clMode ? "/clang:-fno-implicit-module-maps" : "-fno-implicit-module-maps");
    command.CommandLine = std::move(arguments);
    // Producers receive a syntax-only command already stripped of ordinary output/dependency
    // actions; this contract is not left to an individual consumer's FrontendAction choice.
    command.CommandLine = clang::tooling::getClangStripOutputAdjuster()(command.CommandLine, command.Filename);
    command.CommandLine = clang::tooling::getClangStripDependencyFileAdjuster()(command.CommandLine, command.Filename);
    command.CommandLine = clang::tooling::getClangSyntaxOnlyAdjuster()(command.CommandLine, command.Filename);
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> disk(llvm::vfs::createPhysicalFileSystem().release());
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> physical(new llvm::vfs::OverlayFileSystem(disk));
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> overlays(new llvm::vfs::InMemoryFileSystem);
    std::set<std::string> paths;
    std::size_t overlayBytes = 0;
    if (const auto* values = request.getArray("overlays"))
        for (const auto& value : *values) {
            const auto* object = value.getAsObject();
            if (!object) {
                error = "Invalid source overlay";
                return {};
            }
            const auto path = object->getString("path"), contents = object->getString("contents");
            if (!path || !contents) {
                error = "Overlay lacks path or contents";
                return {};
            }
            const auto name = absolute(*path, result.root);
            if (!inside(name, result.root) || paths.size() >= 4096 || !paths.insert(name).second ||
                contents->size() > 8 * 1024 * 1024 || !addWithin(overlayBytes, contents->size(), 64 * 1024 * 1024) ||
                !overlays->addFile(name, 0, llvm::MemoryBuffer::getMemBufferCopy(*contents))) {
                error = "External, duplicate or oversized source overlay";
                return {};
            }
        }
    physical->pushOverlay(overlays);
    result.files = new SemanticSnapshotFileSystem(physical, std::move(disk));
    return result;
}
llvm::json::Array semanticDependencies(const SemanticCommand& command) {
    llvm::json::Array result;
    for (const auto& [path, input] : command.files->inputs)
        result.push_back(llvm::json::Object{
            {"path", path}, {"sha256", semantic::sha256(*input.bytes)}, {"bytes", input.bytes->size()}});
    return result;
}
llvm::json::Object semanticUnitReceipt(const SemanticCommand& command, llvm::StringRef kind) {
    llvm::json::Array missing, directories;
    for (const auto& path : command.files->missingInputs())
        missing.push_back(path);
    for (const auto& path : command.files->directoryInputs())
        directories.push_back(path);
    return llvm::json::Object{
        {"kind", kind},
        {"schema", 1},
        {"status", "ready"},
        {"configuration", command.configuration},
        {"commandIdentity", command.identity},
        {"source", command.command.Filename},
        {"compilationDatabaseSha256", command.databaseSha256},
        {"commandIndex", command.commandIndex},
        {"dependencies", semanticDependencies(command)},
        {"missingDependencies", std::move(missing)},
        {"directoryDependencies", std::move(directories)},
        {"directoryListings", static_cast<const SemanticSnapshotFileSystem&>(*command.files).directoryListings()}};
}
} // namespace frost::clang_engine
