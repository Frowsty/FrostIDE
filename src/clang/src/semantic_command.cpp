// SPDX-License-Identifier: Apache-2.0
// CPP-8/13 preserves distinct full compile-command identities and captures actual frontend inputs.
#include "semantic_command.hpp"

namespace frost::clang_engine {
namespace {
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
    auto parsed = clang::tooling::JSONCompilationDatabase::loadFromBuffer(
        *database, error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!parsed)
        return {};
    const auto commands = parsed->getAllCompileCommands();
    if (commands.size() > 32768 || static_cast<std::uint64_t>(*ordinal) >= commands.size()) {
        error = "Command ordinal outside captured database";
        return {};
    }
    result.command = commands[static_cast<std::size_t>(*ordinal)];
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
    const bool cl = driver == "cl.exe" || driver == "cl" || driver == "clang-cl.exe" || driver == "clang-cl";
    const bool c =
        driver == "clang" || driver == "clang.exe" || driver == "gcc" || driver == "gcc.exe" || driver == "cc";
    if (!cl && !c && driver != "clang++" && driver != "clang++.exe" && driver != "g++" && driver != "g++.exe" &&
        driver != "c++") {
        error = "Compiler wrapper is unsupported; supply its expanded compile command";
        return {};
    }
    llvm::SmallString<256> pinned(resource);
    for (unsigned index = 0; index < 3; ++index)
        llvm::sys::path::remove_filename(pinned);
    llvm::sys::path::append(pinned, "bin", cl ? "clang-cl" : c ? "clang" : "clang++");
#ifdef _WIN32
    pinned += ".exe";
#endif
    std::vector<std::string> arguments{pinned.str().str()};
    for (std::size_t index = 1; index < command.CommandLine.size(); ++index) {
        const llvm::StringRef argument(command.CommandLine[index]);
        if (argument.starts_with("@") || argument == "-Xclang" || argument.starts_with("-Xclang=") ||
            argument.starts_with("-fplugin") || argument.starts_with("-fmodule") || argument.starts_with("/clang:") ||
            argument.starts_with("-save-temps") || argument.starts_with("-ftime-trace") ||
            argument.starts_with("-fprofile") || argument.starts_with("-MJ") || argument == "-include-pch" ||
            argument.starts_with("--serialize-diagnostics") || argument.starts_with("-serialize-diagnostics") ||
            argument.starts_with("-fdiagnostics-serialization-file") || argument.starts_with("--output") ||
            (cl && (argument.starts_with("/sourceDependencies") || argument.starts_with("/Fi"))) ||
            (cl && (argument.starts_with("/Yu") || argument.starts_with("/Yc")))) {
            error = "Response/plugin/module/opaque PCH command is not a textual semantic unit";
            return {};
        }
        arguments.push_back(argument.str());
    }
    arguments.push_back("-resource-dir=" + resource.str());
    arguments.push_back("-fno-implicit-modules");
    arguments.push_back("-fno-implicit-module-maps");
    command.CommandLine = std::move(arguments);
    // Producers receive a syntax-only command already stripped of ordinary output/dependency
    // actions; this contract is not left to an individual consumer's FrontendAction choice.
    command.CommandLine = clang::tooling::getClangStripOutputAdjuster()(command.CommandLine, command.Filename);
    command.CommandLine = clang::tooling::getClangStripDependencyFileAdjuster()(command.CommandLine, command.Filename);
    command.CommandLine = clang::tooling::getClangSyntaxOnlyAdjuster()(command.CommandLine, command.Filename);
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> physical(new llvm::vfs::OverlayFileSystem(
        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>(llvm::vfs::createPhysicalFileSystem().release())));
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
    result.files = new SnapshotFileSystem(physical, 32768, 512ULL * 1024 * 1024, 64ULL * 1024 * 1024);
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
    return {{"kind", kind},
            {"schema", 1},
            {"status", "ready"},
            {"configuration", command.configuration},
            {"commandIdentity", command.identity},
            {"source", command.command.Filename},
            {"compilationDatabaseSha256", command.databaseSha256},
            {"commandIndex", command.commandIndex},
            {"dependencies", semanticDependencies(command)},
            {"missingDependencies", std::move(missing)},
            {"directoryDependencies", std::move(directories)}};
}
} // namespace frost::clang_engine
