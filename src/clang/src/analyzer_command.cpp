// SPDX-License-Identifier: Apache-2.0
// Exact captured-command analyzer producer; actual AST/CFG findings and SARIF, no target execution.
#include "analyzer.hpp"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Config/llvm-config.h"
#include "semantic_command.hpp"
namespace frost::clang_engine {
namespace {
class Database final : public clang::tooling::CompilationDatabase {
    clang::tooling::CompileCommand value;

public:
    explicit Database(clang::tooling::CompileCommand command) : value(std::move(command)) {}
    std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef) const override { return {value}; }
    std::vector<std::string> getAllFiles() const override { return {value.Filename}; }
};
class Consumer final : public clang::ASTConsumer {
    SemanticCommand& command;
    const std::set<std::string>& enabled;
    llvm::json::Array& findings;
    llvm::json::Array& limitations;

public:
    Consumer(SemanticCommand& c, const std::set<std::string>& e, llvm::json::Array& f, llvm::json::Array& l)
        : command(c), enabled(e), findings(f), limitations(l) {}
    void HandleTranslationUnit(clang::ASTContext& context) override {
        if (context.getDiagnostics().hasErrorOccurred())
            return;
        AnalyzerResults output(context, command.root, enabled);
        analyzeSemantics(output);
        analyzeDataflow(output);
        findings = std::move(output.findings);
        limitations = std::move(output.limitations);
    }
};
class Action final : public clang::ASTFrontendAction {
    SemanticCommand& command;
    const std::set<std::string>& enabled;
    llvm::json::Array& findings;
    llvm::json::Array& limitations;

public:
    Action(SemanticCommand& c, const std::set<std::string>& e, llvm::json::Array& f, llvm::json::Array& l)
        : command(c), enabled(e), findings(f), limitations(l) {}
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override {
        return std::make_unique<Consumer>(command, enabled, findings, limitations);
    }
};
class Factory final : public clang::tooling::FrontendActionFactory {
    SemanticCommand& command;
    const std::set<std::string>& enabled;
    llvm::json::Array& findings;
    llvm::json::Array& limitations;

public:
    Factory(SemanticCommand& c, const std::set<std::string>& e, llvm::json::Array& f, llvm::json::Array& l)
        : command(c), enabled(e), findings(f), limitations(l) {}
    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<Action>(command, enabled, findings, limitations);
    }
};

} // namespace
int runAnalyzerUnit(std::string_view resourceDirectory, bool asSarif) {
    std::string bytes;
    char chunk[4096];
    while (std::cin) {
        std::cin.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        if (bytes.size() > 32U * 1024U * 1024U - count) {
            llvm::errs() << "Analyzer request exceeds32MiB\n";
            return 1;
        }
        bytes.append(chunk, count);
    }
    auto parsed = llvm::json::parse(bytes);
    llvm::json::Object result{{"kind", "frost-analyzer-unit"},
                              {"schema", 1},
                              {"status", "invalid-input"},
                              {"findings", llvm::json::Array{}},
                              {"rules", analyzerRegistry()}};
    std::string error;
    auto* request = parsed ? parsed->getAsObject() : nullptr;
    auto command = request ? semanticCommand(*request, resourceDirectory, error) : std::nullopt;
    if (!parsed)
        llvm::consumeError(parsed.takeError());
    if (command) {
        std::set<std::string> enabled;
        bool valid = true;
        for (const auto& rule : analyzerRules())
            enabled.insert(rule.id);
        if (request->get("rules") && !request->getArray("rules")) {
            valid = false;
            error = "Analyzer rules must be an array of stable IDs";
        }
        if (const auto* rules = request->getArray("rules")) {
            auto allowed = enabled;
            enabled.clear();
            for (const auto& value : *rules) {
                auto id = value.getAsString();
                if (!id || !allowed.contains(id->str())) {
                    valid = false;
                    error = "Unknown analyzer rule identity";
                    break;
                }
                enabled.insert(id->str());
            }
        }
        if (valid) {
            llvm::json::Array findings, limitations;
            Database database(command->command);
            Factory factory(*command, enabled, findings, limitations);
            clang::tooling::ClangTool tool(database, {command->command.Filename},
                                           std::make_shared<clang::PCHContainerOperations>(), command->files);
            semantic::Diagnostics diagnostics;
            tool.setDiagnosticConsumer(&diagnostics);
            const auto exit = tool.run(&factory);
            if (!exit && !diagnostics.failed && command->files->error.empty() &&
                command->files->capturePositiveProbes()) {
                result = semanticUnitReceipt(*command, "frost-analyzer-unit");
                result["rules"] = analyzerRegistry();
                result["llvmVersion"] = LLVM_VERSION_STRING;
                result["status"] = limitations.empty() ? "ready" : "partial";
                result["findings"] = std::move(findings);
                result["limitations"] = std::move(limitations);
                result["columnKind"] = "unicodeCodePoints";
            } else {
                result["status"] = "parse-error";
                llvm::json::Array messages;
                for (const auto& message : diagnostics.messages)
                    messages.push_back(message);
                if (!command->files->error.empty())
                    messages.push_back(command->files->error);
                result["diagnostics"] = std::move(messages);
            }
        }
    }
    if (!error.empty())
        result["diagnostics"] = llvm::json::Array{error};
    const int exit = result.getString("status") == "ready" ? 0 : result.getString("status") == "partial" ? 2 : 1;
    std::string serialized;
    llvm::raw_string_ostream stream(serialized);
    stream << llvm::json::Value(asSarif ? analyzerSarif(std::move(result)) : std::move(result));
    stream.flush();
    if (serialized.size() > 64U * 1024U * 1024U) {
        llvm::outs() << "{\"kind\":\"frost-analyzer-unit\",\"schema\":1,\"status\":\"resource-limit\",\"findings\":[],"
                        "\"diagnostics\":[\"Analyzer report exceeds64MiB\"]}\n";
        return 1;
    }
    llvm::outs() << serialized << '\n';
    return exit;
}
} // namespace frost::clang_engine
