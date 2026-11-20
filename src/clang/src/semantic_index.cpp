// SPDX-License-Identifier: Apache-2.0
// CPP-8 symbol occurrences/relations come only from pinned Clang indexing callbacks.
#include "semantic_index.hpp"

#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/IndexingAction.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#include "semantic_command.hpp"

namespace frost::clang_engine {
namespace {
class Database final : public clang::tooling::CompilationDatabase {
    clang::tooling::CompileCommand command;

public:
    explicit Database(clang::tooling::CompileCommand command) : command(std::move(command)) {}
    std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef) const override { return {command}; }
    std::vector<std::string> getAllFiles() const override { return {command.Filename}; }
};
class Consumer final : public clang::index::IndexDataConsumer {
public:
    const SemanticCommand& command;
    clang::ASTContext* context = nullptr;
    llvm::json::Array symbols, occurrences, relations;
    std::set<std::string> known;
    std::size_t stringBytes = 0;
    std::string error;
    explicit Consumer(const SemanticCommand& command) : command(command) {}
    void initialize(clang::ASTContext& ast) override { context = &ast; }
    std::string symbol(const clang::Decl* declaration) {
        llvm::SmallString<128> buffer;
        if (clang::index::generateUSRForDecl(declaration, buffer))
            return {};
        const auto usr = buffer.str().str();
        if (known.contains(usr))
            return usr;
        std::string name;
        llvm::raw_string_ostream stream(name);
        clang::index::printSymbolName(declaration, context->getLangOpts(), stream);
        stream.flush();
        if (name.empty())
            return {};
        known.insert(usr);
        const auto* named = llvm::dyn_cast<clang::NamedDecl>(declaration);
        auto qualified = named ? named->getQualifiedNameAsString() : name;
        if (qualified.empty())
            qualified = name;
        if (known.size() > 100000 ||
            !semantic::addWithin(stringBytes, usr.size() + name.size() + qualified.size(), 32 * 1024 * 1024)) {
            error = "Semantic symbol budget exceeded";
            return {};
        }
        symbols.push_back(llvm::json::Object{
            {"usr", usr},
            {"name", name},
            {"qualifiedName", qualified},
            {"kind", clang::index::getSymbolKindString(clang::index::getSymbolInfo(declaration).Kind)}});
        return usr;
    }
    bool handleDeclOccurrence(const clang::Decl* declaration, clang::index::SymbolRoleSet roles,
                              llvm::ArrayRef<clang::index::SymbolRelation> related, clang::SourceLocation location,
                              ASTNodeInfo) override {
        if (!error.empty())
            return false;
        auto& sm = context->getSourceManager();
        const auto expanded = sm.getExpansionLoc(location);
        if (!expanded.isFileID() || sm.getFilename(expanded).empty())
            return true;
        const auto path = semantic::absolute(sm.getFilename(expanded), command.command.Directory);
        const auto root = llvm::StringRef(command.root);
        if (!(path == root ||
              (llvm::StringRef(path).starts_with(root) && path.size() > root.size() &&
               (llvm::sys::path::is_separator(root.back()) || llvm::sys::path::is_separator(path[root.size()])))))
            return true;
        const auto usr = symbol(declaration);
        if (!error.empty())
            return false;
        if (usr.empty())
            return true; // Clang has no stable USR, so no invented identity is published.
        if (occurrences.size() >= 250000 ||
            !semantic::addWithin(stringBytes, path.size() + usr.size(), 32 * 1024 * 1024)) {
            error = "Semantic occurrence budget exceeded";
            return false;
        }
        const auto spelling = sm.getSpellingLoc(location);
        const auto ordinal = occurrences.size();
        occurrences.push_back(llvm::json::Object{
            {"usr", usr},
            {"path", path},
            {"offset", sm.getFileOffset(expanded)},
            {"line", sm.getSpellingLineNumber(expanded)},
            {"column", sm.getSpellingColumnNumber(expanded)},
            {"roles", roles},
            {"macro", location.isMacroID()},
            {"spellingPath", sm.getFilename(spelling).empty()
                                 ? std::string{}
                                 : semantic::absolute(sm.getFilename(spelling), command.command.Directory)},
            {"spellingOffset", spelling.isFileID() ? sm.getFileOffset(spelling) : 0}});
        for (const auto& relation : related) {
            const auto target = symbol(relation.RelatedSymbol);
            if (!error.empty())
                return false;
            if (target.empty())
                continue;
            if (relations.size() >= 1000000 ||
                !semantic::addWithin(stringBytes, usr.size() + target.size(), 32 * 1024 * 1024)) {
                error = "Semantic relation budget exceeded";
                return false;
            }
            relations.push_back(llvm::json::Object{
                {"occurrence", ordinal}, {"source", usr}, {"target", target}, {"roles", relation.Roles}});
        }
        return true;
    }
};
class Factory final : public clang::tooling::FrontendActionFactory {
    std::shared_ptr<Consumer> consumer;

public:
    explicit Factory(std::shared_ptr<Consumer> consumer) : consumer(std::move(consumer)) {}
    std::unique_ptr<clang::FrontendAction> create() override {
        clang::index::IndexingOptions options;
        options.IndexFunctionLocals = true;
        options.IndexParametersInDeclarations = true;
        options.IndexTemplateParameters = true;
        options.IndexImplicitInstantiation = true;
        options.DeferIndexingToEndOfTranslationUnit = true;
        options.IndexMacros = false; // CPP-13 owns macro events; don't imply AST-only macro reference coverage.
        return clang::index::createIndexingAction(consumer, options);
    }
};
llvm::json::Object index(const llvm::json::Object& request, llvm::StringRef resource) {
    std::string error;
    const auto refuse = [](llvm::StringRef reason) {
        return llvm::json::Object{
            {"kind", "frost-semantic-index"}, {"schema", 1}, {"status", "unavailable"}, {"error", reason}};
    };
    auto command = semanticCommand(request, resource, error);
    if (!command)
        return refuse(error);
    Database database(command->command);
    clang::tooling::ClangTool tool(database, {command->command.Filename},
                                   std::make_shared<clang::PCHContainerOperations>(), command->files);
    semantic::Diagnostics diagnostics;
    tool.setDiagnosticConsumer(&diagnostics);
    tool.appendArgumentsAdjuster(clang::tooling::getClangStripOutputAdjuster());
    tool.appendArgumentsAdjuster(clang::tooling::getClangStripDependencyFileAdjuster());
    tool.appendArgumentsAdjuster(clang::tooling::getClangSyntaxOnlyAdjuster());
    auto consumer = std::make_shared<Consumer>(*command);
    Factory factory(consumer);
    const auto status = tool.run(&factory);
    if (status || diagnostics.failed || !command->files->error.empty() || !consumer->error.empty())
        return refuse(!consumer->error.empty()         ? consumer->error
                      : !command->files->error.empty() ? command->files->error
                                                       : "Compilation configuration did not complete successfully");
    if (!command->files->capturePositiveProbes())
        return refuse(command->files->error);
    auto result = semanticUnitReceipt(*command, "frost-semantic-index");
    result["symbols"] = std::move(consumer->symbols);
    result["occurrences"] = std::move(consumer->occurrences);
    result["relations"] = std::move(consumer->relations);
    result["coverage"] = "one exact captured compilation command; project-spelled declarations/references and Clang "
                         "relations; macros excluded";
    return result;
}
} // namespace
int runSemanticIndex(std::string_view resourceDirectory) {
    std::string input;
    char chunk[4096];
    while (std::cin) {
        std::cin.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        if (input.size() > 32 * 1024 * 1024 - count) {
            llvm::outs()
                << "{\"kind\":\"frost-semantic-index\",\"status\":\"unavailable\",\"error\":\"input exceeds32MiB\"}\n";
            return 1;
        }
        input.append(chunk, count);
    }
    auto parsed = llvm::json::parse(input);
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        llvm::outs() << "{\"kind\":\"frost-semantic-index\",\"status\":\"unavailable\",\"error\":\"invalid JSON\"}\n";
        return 1;
    }
    if (!parsed->getAsObject()) {
        llvm::outs()
            << "{\"kind\":\"frost-semantic-index\",\"status\":\"unavailable\",\"error\":\"request must be object\"}\n";
        return 1;
    }
    auto result = index(*parsed->getAsObject(), resourceDirectory);
    const bool success = result.getString("status") == "ready";
    std::string output;
    llvm::raw_string_ostream stream(output);
    stream << llvm::json::Value(std::move(result));
    stream.flush();
    if (output.size() > 64 * 1024 * 1024) {
        llvm::outs() << "{\"kind\":\"frost-semantic-index\",\"schema\":1,\"status\":\"unavailable\",\"error\":\"output "
                        "exceeds64MiB\"}\n";
        return 1;
    }
    llvm::outs() << output << '\n';
    return success ? 0 : 1;
}
} // namespace frost::clang_engine
