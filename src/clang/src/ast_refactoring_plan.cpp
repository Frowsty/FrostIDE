// SPDX-License-Identifier: Apache-2.0
// AST operations over the same immutable VFS and complete command admission as R8 rename.
#include "ast_refactoring_plan.hpp"

#include "ast_refactoring_operations.hpp"
#include "ast_create_from_usage.hpp"
#include "ast_signature.hpp"
#include "semantic_snapshot.hpp"
namespace frost::clang_engine {
namespace {
using namespace semantic;
class ProjectCommand final : public clang::tooling::CompilationDatabase {
public:
    clang::tooling::CompileCommand command;
    explicit ProjectCommand(clang::tooling::CompileCommand value) : command(std::move(value)) {}
    std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef) const override { return {command}; }
    std::vector<std::string> getAllFiles() const override { return {command.Filename}; }
};

const clang::NamedDecl* projectCanonical(const clang::NamedDecl* declaration) {
    if (const auto* templ = llvm::dyn_cast<clang::ClassTemplateDecl>(declaration))
        declaration = templ->getTemplatedDecl();
    if (const auto* templ = llvm::dyn_cast<clang::FunctionTemplateDecl>(declaration))
        declaration = templ->getTemplatedDecl();
    if (const auto* specialization = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(declaration))
        declaration = specialization->getSpecializedTemplate()->getTemplatedDecl();
    return clang::tooling::getCanonicalSymbolDeclaration(declaration);
}

template<class DiagnosticSink>
std::unique_ptr<clang::ASTUnit> projectAST(const clang::tooling::CompileCommand& command,
                                           llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs,
                                           const std::map<std::string, std::string>& changes,
                                           DiagnosticSink& diagnostics, bool recovery = false) {
    ProjectCommand database(command);
    std::vector<std::string> files{command.Filename};
    clang::tooling::ClangTool tool(database, files, std::make_shared<clang::PCHContainerOperations>(), fs);
    tool.setDiagnosticConsumer(&diagnostics);
    tool.appendArgumentsAdjuster(clang::tooling::getClangStripDependencyFileAdjuster());
    for (const auto& [path, bytes] : changes)
        tool.mapVirtualFile(path, bytes);
    std::vector<std::unique_ptr<clang::ASTUnit>> units;
    const auto built = tool.buildASTs(units);
    if ((!recovery && (built || diagnostics.failed)) || units.size() != 1)
        return {};
    return std::move(units[0]);
}

llvm::json::Object astPlan(const llvm::json::Object& input, llvm::StringRef resource) {
    llvm::json::Object result{{"kind", "cpp-refactoring-plan"},
                              {"schemaVersion", 1},
                              {"status", "invalid-input"},
                              {"atomic", true},
                              {"files", llvm::json::Array{}}};
    auto refuse = [&](llvm::StringRef status, const std::string& why) {
        result["status"] = status;
        result["diagnostics"] = llvm::json::Array{why};
        result["files"] = llvm::json::Array{};
        return std::move(result);
    };
    if (resource.empty())
        return refuse("unsupported", "Pinned LLVM resource directory unavailable");
    const auto rootValue = input.getString("projectRoot"), seedValue = input.getString("seedTranslationUnit"),
               anchorValue = input.getString("path"), newValue = input.getString("newName"),
               databaseValue = input.getString("compilationDatabase");
    const auto offset = input.getInteger("offset");
    if (!rootValue || !seedValue || !anchorValue || !newValue || !databaseValue || !offset || *offset < 0 ||
        *offset > std::numeric_limits<unsigned>::max())
        return refuse("invalid-input", "Missing project, compile commands, seed or selection");
    const std::string root = absolute(*rootValue, ""),
                      seed = seedValue->empty() ? std::string{} : absolute(*seedValue, root),
                      anchor = absolute(*anchorValue, root), newName = newValue->str();
    if (root.find('\0') != std::string::npos || seed.find('\0') != std::string::npos ||
        anchor.find('\0') != std::string::npos)
        return refuse("invalid-input", "NUL in project path");
    if (!llvm::sys::path::is_absolute(root) || newName.empty() ||
        !((newName[0] >= 'A' && newName[0] <= 'Z') || (newName[0] >= 'a' && newName[0] <= 'z') || newName[0] == '_') ||
        std::ranges::any_of(newName, [](unsigned char ch) {
            return !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_');
        }))
        return refuse("invalid-input", "New name must be one C++ identifier");
    auto inside = [&](llvm::StringRef path) {
        return path == root ||
               (path.starts_with(root) && path.size() > root.size() &&
                (llvm::sys::path::is_separator(root.back()) || llvm::sys::path::is_separator(path[root.size()])));
    };
    if (!inside(anchor) || (!seed.empty() && !inside(seed)))
        return refuse("unsupported", "Selection must belong to the captured project");
    std::string error;
    auto database = clang::tooling::JSONCompilationDatabase::loadFromBuffer(
        *databaseValue, error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!database)
        return refuse("invalid-input", error);
    auto commands = database->getAllCompileCommands();
    if (commands.empty() || commands.size() > 16384)
        return refuse("unsupported", "Compilation database is empty or exceeds16384 configurations");
    const bool creation = input.getString("operation").value_or("") == "create-from-usage";
    bool hasSeed = seed.empty();
    llvm::json::Array pch;
    for (auto& command : commands) {
        command.Directory = absolute(command.Directory, root);
        command.Filename = absolute(command.Filename, command.Directory);
        if (!inside(command.Filename) || command.CommandLine.empty())
            return refuse("unsupported", "Compilation database contains an unowned source or empty command");
        if (command.Directory.find('\0') != std::string::npos || command.Filename.find('\0') != std::string::npos)
            return refuse("invalid-input", "NUL in compilation path");
        hasSeed |= command.Filename == seed;
        std::vector<std::string> args;
        const auto driverName = llvm::sys::path::filename(command.CommandLine[0]);
        const bool cl =
            driverName == "cl.exe" || driverName == "cl" || driverName == "clang-cl.exe" || driverName == "clang-cl";
        const bool c = driverName == "clang" || driverName == "clang.exe" || driverName == "gcc" ||
                       driverName == "gcc.exe" || driverName == "cc";
        if (!cl && !c && driverName != "clang++" && driverName != "clang++.exe" && driverName != "g++" &&
            driverName != "g++.exe" && driverName != "c++")
            return refuse("unsupported", "Compilation command uses an unsupported compiler wrapper");
        llvm::SmallString<256> driver(resource);
        for (int i = 0; i != 3; ++i)
            llvm::sys::path::remove_filename(driver);
        llvm::sys::path::append(driver, "bin", cl ? "clang-cl" : c ? "clang" : "clang++");
#ifdef _WIN32
        driver += ".exe";
#endif
        args.push_back(driver.str().str());
        for (std::size_t i = 1; i < command.CommandLine.size(); ++i) {
            llvm::StringRef arg(command.CommandLine[i]);
            if (arg.contains('\0'))
                return refuse("invalid-input", "NUL in compiler argument");
            if (arg.starts_with("@") || arg == "-Xclang" || arg.starts_with("-fplugin") ||
                arg.starts_with("-fmodule") || arg.starts_with("/clang:") || arg == "-include-pch")
                return refuse("unsupported", "Response/plugin/module or opaque PCH actions are not admitted");
            if (cl && (arg.starts_with("/Yu") || arg.starts_with("/Yc"))) {
                auto header = arg.drop_front(3).str();
                if (header.empty()) {
                    if (++i == command.CommandLine.size())
                        return refuse("unsupported", "PCH header missing");
                    header = command.CommandLine[i];
                }
                if (header.empty() || header[0] == '/' || header[0] == '-')
                    return refuse("unsupported", "Default opaque PCH context has no source header");
                args.push_back("/FI" + header);
                pch.push_back(llvm::json::Object{
                    {"translationUnit", command.Filename}, {"header", header}, {"mode", "textual-reparse"}});
                continue;
            }
            if (cl && arg.starts_with("/Fp")) {
                if (arg.size() == 3 && ++i == command.CommandLine.size())
                    return refuse("invalid-input", "Missing PCH output operand");
                continue;
            }
            args.push_back(arg.str());
        }
        args.push_back("-resource-dir=" + resource.str());
        args.push_back("-fno-implicit-modules");
        args.push_back("-fno-implicit-module-maps");
        if (creation) {
            args.insert(args.end(), {"-Xclang", "-frecovery-ast", "-Xclang", "-frecovery-ast-type", "-Werror=return-type"});
        }
        command.CommandLine = std::move(args);
    }
    if (!hasSeed)
        return refuse("invalid-input", "Seed translation unit is absent from complete compilation database");
    std::stable_sort(commands.begin(), commands.end(),
                     [&](const auto& a, const auto& b) { return (a.Filename == seed) > (b.Filename == seed); });
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> physical(new llvm::vfs::OverlayFileSystem(
        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>(llvm::vfs::createPhysicalFileSystem().release())));
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> overlays(new llvm::vfs::InMemoryFileSystem);
    std::set<std::string> overlayPaths;
    if (const auto* supplied = input.getArray("overlays"))
        for (const auto& value : *supplied) {
            const auto* object = value.getAsObject();
            if (!object)
                return refuse("invalid-input", "Invalid overlay");
            auto path = object->getString("path"), bytes = object->getString("contents");
            if (!path || !bytes)
                return refuse("invalid-input", "Invalid overlay snapshot");
            auto name = absolute(*path, root);
            if (!inside(name) || !overlayPaths.insert(name).second || !valueFits(bytes->size(), kMainBytes) ||
                !overlays->addFile(name, 0, llvm::MemoryBuffer::getMemBufferCopy(*bytes)))
                return refuse("invalid-input", "Duplicate, external or oversized overlay");
        }
    physical->pushOverlay(overlays);
    llvm::IntrusiveRefCntPtr<SnapshotFileSystem> frozen(
        new SnapshotFileSystem(physical, 32768, 512ULL * 1024 * 1024, 64ULL * 1024 * 1024));
    const bool generation = input.getString("operation").value_or("").starts_with("generate-");
    const auto operation = input.getString("operation").value_or("");
    const bool signature = operation == "change-signature" || operation == "edit-signature";
    SignaturePlanner signatures(input, root, anchor, static_cast<unsigned>(input.getInteger("offset").value_or(0)));
    if (!generation && !signature && (seed != anchor || seed.empty()))
        return refuse("unsupported", "AST operations require a selected source translation unit, not a header");
    std::map<std::string, std::string> candidates;
    bool selected = false;
    for (const auto& command : commands) {
        Diagnostics diagnostics;
        CreateUsageDiagnostics recovered(anchor, root, static_cast<unsigned>(*offset));
        auto ast = creation ? projectAST(command, frozen, {}, recovered, true)
                            : projectAST(command, frozen, {}, diagnostics);
        if (!ast || !frozen->error.empty() || (creation && recovered.unrelated))
            return refuse("parse-error", creation ? "Original configuration has errors outside the selected unresolved use" : "Original compilation configuration failed");
        if (signature) {
            if (!signatures.discover(*ast, error))
                return refuse("unsupported", error);
            continue;
        }
        if (!generation && command.Filename != anchor)
            continue;
        std::optional<std::string> candidate;
        std::string target = anchor;
        if (creation) {
            if (!recovered.selectedError)
                return refuse("unsupported", "The selected identifier has no exact unresolved-use compiler diagnostic");
            auto created = createFromUsage(*ast, input, error);
            if (created) { target = std::move(created->path); candidate = std::move(created->after); }
        } else candidate = generation ? refactorGenerate(*ast, input, error) : refactorAst(*ast, input, error);
        if (!candidate && generation && error == "not-applicable")
            continue;
        selected = true;
        if (!candidate)
            return refuse("unsupported", error);
        if (!frozen->inputs.contains(target))
            return refuse("unsupported", "Selected file has no exact captured source receipt");
        auto [position, inserted] = candidates.emplace(target, *candidate);
        if (!inserted && position->second != *candidate)
            return refuse("conflict", "AST operation differs between compilation configurations");
    }
    if (!signature && (!selected || candidates.empty()))
        return refuse("unsupported", "Selected translation unit has no captured compile command");
    if (!frozen->capturePositiveProbes())
        return refuse("unsupported", frozen->error);
    frozen->frozen = true;
    if (signature) {
        if (!signatures.found())
            return refuse("unsupported", "Selected function has no captured declaration");
        for (const auto& command : commands) {
            Diagnostics diagnostics;
            auto ast = projectAST(command, frozen, {}, diagnostics);
            if (!ast || !frozen->error.empty())
                return refuse("conflict", "Original signature replay failed");
            if (!signatures.collect(*ast, error))
                return refuse("unsupported", error);
        }
        std::map<std::string, std::shared_ptr<const std::string>> inputs;
        for (const auto& [path, entry] : frozen->inputs)
            inputs.emplace(path, entry.bytes);
        auto planned = signatures.candidates(inputs, error);
        if (!planned)
            return refuse("unsupported", error);
        candidates = std::move(*planned);
    }
    std::size_t configuration = 0;
    for (const auto& command : commands) {
        Diagnostics diagnostics;
        auto ast = projectAST(command, frozen, candidates, diagnostics);
        if (!ast || !frozen->error.empty())
            return refuse("conflict", "Candidate fails a frozen compilation configuration");
        if (signature && !signatures.validate(*ast, configuration, error))
            return refuse("conflict", error);
        ++configuration;
    }
    llvm::json::Array files, dependencies, missing, directories;
    std::size_t outputBytes = 0;
    auto charge = [&](std::size_t bytes) {
        constexpr std::size_t limit = 64 * 1024 * 1024;
        if (outputBytes > limit || bytes > (limit - outputBytes) / 6)
            return false;
        outputBytes += bytes * 6;
        return true;
    };
    for (const auto& [path, bytes] : candidates) {
        const auto& before = *frozen->inputs.at(path).bytes;
        if (!llvm::json::isUTF8(before) || !llvm::json::isUTF8(bytes) || !charge(before.size()) ||
            !charge(bytes.size()))
            return refuse("unsupported", "UTF-8/output budget exceeded");
        files.push_back(llvm::json::Object{{"path", path},
                                           {"before", before},
                                           {"after", bytes},
                                           {"beforeSha256", sha256(before)},
                                           {"afterSha256", sha256(bytes)},
                                           {"overlay", overlayPaths.contains(path)}});
    }
    for (const auto& [path, entry] : frozen->inputs) {
        if (!charge(path.size() + 160))
            return refuse("unsupported", "Dependency receipt output budget exceeded");
        dependencies.push_back(llvm::json::Object{{"path", path},
                                                  {"sha256", sha256(*entry.bytes)},
                                                  {"bytes", entry.bytes->size()},
                                                  {"overlay", overlayPaths.contains(path)}});
    }
    for (const auto& path : frozen->missingInputs()) {
        if (!charge(path.size()))
            return refuse("unsupported", "Namespace receipt budget exceeded");
        missing.push_back(path);
    }
    for (const auto& path : frozen->directoryInputs()) {
        if (!charge(path.size()))
            return refuse("unsupported", "Directory receipt output budget exceeded");
        directories.push_back(path);
    }
    if (dependencies.size() + missing.size() + directories.size() > 32768)
        return refuse("unsupported", "Transaction read-dependency budget exceeded");
    result["directoryDependencies"] = std::move(directories);
    result["status"] = "ready";
    result["files"] = std::move(files);
    result["dependencies"] = std::move(dependencies);
    result["missingDependencies"] = std::move(missing);
    result["translationUnitConfigurations"] = commands.size();
    result["pch"] = std::move(pch);
    result["compilationDatabaseSha256"] = sha256(*databaseValue);
    result["coverage"] = "all captured configurations; frozen dependencies; bounded AST operation; candidate reparse";
    return result;
}
} // namespace
int runAstRefactoringPlan(std::string_view resourceDirectory) {
    std::string request;
    char chunk[4096];
    while (std::cin) {
        std::cin.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        if (request.size() > 32 * 1024 * 1024 - count) {
            llvm::outs()
                << "{\"kind\":\"cpp-refactoring-plan\",\"status\":\"invalid-input\",\"files\":[],\"diagnostics\":["
                   "\"request exceeds32MiB\"]}\n";
            return 1;
        }
        request.append(chunk, count);
    }
    auto parsed = llvm::json::parse(request);
    llvm::json::Object result;
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        result = {{"kind", "cpp-refactoring-plan"}, {"status", "invalid-input"}, {"files", llvm::json::Array{}}};
    } else if (const auto* object = parsed->getAsObject())
        result = astPlan(*object, resourceDirectory);
    else
        result = {{"kind", "cpp-refactoring-plan"}, {"status", "invalid-input"}, {"files", llvm::json::Array{}}};
    const bool ready = result.getString("status") == "ready";
    llvm::outs() << llvm::json::Value(std::move(result)) << '\n';
    return ready ? 0 : 1;
}

} // namespace frost::clang_engine
