// SPDX-License-Identifier: Apache-2.0
// CPP-13 invokes Clang's -E printer; no textual macro substitution or guessed include edges.
#include "semantic_preprocess.hpp"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/PreprocessorOutputOptions.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PPCallbacks.h"
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
class Output final : public llvm::raw_ostream {
public:
    std::string bytes;
    bool exceeded = false;
    Output() : llvm::raw_ostream(true) {}
    void write_impl(const char* data, std::size_t size) override {
        constexpr std::size_t limit = 16U * 1024U * 1024U;
        if (exceeded || size > limit - bytes.size()) {
            exceeded = true;
            return;
        }
        bytes.append(data, size);
    }
    std::uint64_t current_pos() const override { return bytes.size(); }
};
struct Capture {
    SemanticCommand& command;
    llvm::json::Array includes, macros;
    Output output;
    std::size_t strings = 0;
    std::string error;
    explicit Capture(SemanticCommand& command) : command(command) {}
    bool charge(std::size_t bytes) {
        if (!semantic::addWithin(strings, bytes, 8U * 1024U * 1024U)) {
            error = "Preprocessor event text budget exceeded";
            return false;
        }
        return true;
    }
};
llvm::json::Object location(clang::SourceLocation source, const clang::SourceManager& manager,
                            const SemanticCommand& command) {
    source = manager.getExpansionLoc(source);
    if (!source.isFileID() || manager.getFilename(source).empty())
        return llvm::json::Object{{"path", ""}, {"offset", 0}, {"line", 0}, {"column", 0}};
    return llvm::json::Object{{"path", semantic::absolute(manager.getFilename(source), command.command.Directory)},
                              {"offset", manager.getFileOffset(source)},
                              {"line", manager.getSpellingLineNumber(source)},
                              {"column", manager.getSpellingColumnNumber(source)}};
}
class Callbacks final : public clang::PPCallbacks {
    clang::Preprocessor& preprocessor;
    Capture& capture;

public:
    Callbacks(clang::Preprocessor& preprocessor, Capture& capture) : preprocessor(preprocessor), capture(capture) {}
    void InclusionDirective(clang::SourceLocation hash, const clang::Token&, llvm::StringRef name, bool angled,
                            clang::CharSourceRange, clang::OptionalFileEntryRef file, llvm::StringRef, llvm::StringRef,
                            const clang::Module*, bool moduleImported,
                            clang::SrcMgr::CharacteristicKind kind) override {
        if (!capture.error.empty())
            return;
        const auto target =
            file ? semantic::absolute(file->getName(), capture.command.command.Directory) : std::string{};
        if (capture.includes.size() >= 100000 || !capture.charge(name.size() + target.size())) {
            capture.error = "Include graph budget exceeded";
            return;
        }
        capture.includes.push_back(
            llvm::json::Object{{"at", location(hash, preprocessor.getSourceManager(), capture.command)},
                               {"name", name},
                               {"target", target},
                               {"angled", angled},
                               {"moduleImported", moduleImported},
                               {"characteristic", static_cast<unsigned>(kind)}});
    }
    void MacroExpands(const clang::Token& name, const clang::MacroDefinition& definition, clang::SourceRange range,
                      const clang::MacroArgs* arguments) override {
        if (!capture.error.empty())
            return;
        const auto* info = definition.getMacroInfo();
        const auto spelling = preprocessor.getSpelling(name);
        if (capture.macros.size() >= 200000 || !capture.charge(spelling.size())) {
            capture.error = "Macro event budget exceeded";
            return;
        }
        llvm::json::Array tokens, parameters;
        if (info) {
            for (const auto& token : info->tokens()) {
                auto value = preprocessor.getSpelling(token);
                if (!capture.charge(value.size()))
                    return;
                tokens.push_back(std::move(value));
            }
            if (info->isFunctionLike())
                for (const auto* parameter : info->params()) {
                    if (!capture.charge(parameter->getLength()))
                        return;
                    parameters.push_back(parameter->getName());
                }
        }
        auto& manager = preprocessor.getSourceManager();
        llvm::json::Array argumentTokens;
        const bool reflection = spelling == "UCLASS" || spelling == "USTRUCT" || spelling == "UENUM" ||
                                spelling == "UINTERFACE" || spelling == "UPROPERTY" || spelling == "UFUNCTION";
        if (reflection && arguments && !name.getLocation().isMacroID()) {
            std::size_t count = 0;
            for (unsigned argument = 0; argument < arguments->getNumMacroArguments(); ++argument) {
                llvm::json::Array values;
                for (const clang::Token* token = arguments->getUnexpArgument(argument); !token->is(clang::tok::eof);
                     ++token) {
                    const auto value = preprocessor.getSpelling(*token);
                    if (++count > 4096 || !capture.charge(value.size())) {
                        capture.error = "Reflection argument token budget exceeded";
                        return;
                    }
                    values.push_back(value);
                }
                argumentTokens.push_back(std::move(values));
            }
        }
        capture.macros.push_back(llvm::json::Object{
            {"name", spelling},
            {"at", location(name.getLocation(), manager, capture.command)},
            {"end", location(range.getEnd(), manager, capture.command)},
            {"definition", info ? location(manager.getSpellingLoc(info->getDefinitionLoc()), manager, capture.command)
                                : llvm::json::Object{{"path", ""}, {"offset", 0}, {"line", 0}, {"column", 0}}},
            {"functionLike", info && info->isFunctionLike()},
            {"variadic", info && info->isVariadic()},
            {"nested", name.getLocation().isMacroID()},
            {"parameters", std::move(parameters)},
            {"definitionTokens", std::move(tokens)},
            {"argumentTokens", std::move(argumentTokens)}});
    }
};
class Action final : public clang::PreprocessorFrontendAction {
    Capture& capture;

public:
    explicit Action(Capture& capture) : capture(capture) {}
    bool BeginSourceFileAction(clang::CompilerInstance& compiler) override {
        compiler.getPreprocessor().addPPCallbacks(std::make_unique<Callbacks>(compiler.getPreprocessor(), capture));
        return true;
    }
    void ExecuteAction() override {
        auto options = getCompilerInstance().getPreprocessorOutputOpts();
        options.ShowCPP = 1;
        options.ShowLineMarkers = 0;
        options.ShowMacros = 0;
        clang::DoPrintPreprocessedInput(getCompilerInstance().getPreprocessor(), &capture.output, options);
    }
};
class Factory final : public clang::tooling::FrontendActionFactory {
    Capture& capture;

public:
    explicit Factory(Capture& capture) : capture(capture) {}
    std::unique_ptr<clang::FrontendAction> create() override { return std::make_unique<Action>(capture); }
};
llvm::json::Object preprocess(const llvm::json::Object& request, llvm::StringRef resource) {
    const auto refuse = [](llvm::StringRef reason) {
        return llvm::json::Object{
            {"kind", "frost-preprocessor"}, {"schema", 1}, {"status", "unavailable"}, {"error", reason}};
    };
    std::string error;
    auto command = semanticCommand(request, resource, error);
    if (!command)
        return refuse(error);
    Database database(command->command);
    Capture capture(*command);
    Factory factory(capture);
    clang::tooling::ClangTool tool(database, {command->command.Filename},
                                   std::make_shared<clang::PCHContainerOperations>(), command->files);
    semantic::Diagnostics diagnostics;
    tool.setDiagnosticConsumer(&diagnostics);
    const auto status = tool.run(&factory);
    if (status || diagnostics.failed || !capture.error.empty() || capture.output.exceeded ||
        !command->files->error.empty()) {
        if (capture.output.exceeded)
            return refuse("Preprocessed output exceeds 16 MiB; choose a smaller translation unit");
        if (!capture.error.empty())
            return refuse(capture.error);
        if (!command->files->error.empty())
            return refuse(command->files->error);
        return refuse(diagnostics.messages.empty() ? "Preprocessing did not complete successfully"
                                                   : diagnostics.messages.front());
    }
    if (!command->files->capturePositiveProbes())
        return refuse(command->files->error);
    auto result = semanticUnitReceipt(*command, "frost-preprocessor");
    result["text"] = std::move(capture.output.bytes);
    result["includes"] = std::move(capture.includes);
    result["macros"] = std::move(capture.macros);
    result["coverage"] = "one exact compilation command; Clang -E printer plus actual PPCallbacks; definition tokens "
                         "are not claimed as expanded tokens";
    return result;
}
} // namespace
int runSemanticPreprocess(std::string_view resourceDirectory) {
    std::string input;
    char chunk[4096];
    while (std::cin) {
        std::cin.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        if (input.size() > 32U * 1024U * 1024U - count) {
            llvm::outs() << "{\"kind\":\"frost-preprocessor\",\"schema\":1,\"status\":\"unavailable\",\"error\":"
                            "\"input exceeds 32 MiB\"}\n";
            return 1;
        }
        input.append(chunk, count);
    }
    auto request = llvm::json::parse(input);
    if (!request) {
        llvm::consumeError(request.takeError());
        llvm::outs()
            << "{\"kind\":\"frost-preprocessor\",\"schema\":1,\"status\":\"unavailable\",\"error\":\"invalid JSON\"}\n";
        return 1;
    }
    if (!request->getAsObject()) {
        llvm::outs() << "{\"kind\":\"frost-preprocessor\",\"schema\":1,\"status\":\"unavailable\",\"error\":\"request "
                        "must be object\"}\n";
        return 1;
    }
    auto result = preprocess(*request->getAsObject(), resourceDirectory);
    const auto success = result.getString("status") == "ready";
    std::string output;
    llvm::raw_string_ostream stream(output);
    stream << llvm::json::Value(std::move(result));
    stream.flush();
    if (output.size() > 64U * 1024U * 1024U) {
        llvm::outs() << "{\"kind\":\"frost-preprocessor\",\"schema\":1,\"status\":\"unavailable\",\"error\":\"output "
                        "exceeds 64 MiB\"}\n";
        return 1;
    }
    llvm::outs() << output << '\n';
    return success ? 0 : 1;
}
} // namespace frost::clang_engine
