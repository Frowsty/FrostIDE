// SPDX-License-Identifier: Apache-2.0
// Project-scoped signature edits over canonical declarations and direct calls in captured CDB contexts.
#pragma once
#include "clang/Frontend/ASTUnit.h"
#include "llvm/Support/JSON.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
namespace frost::clang_engine {
class SignaturePlanner {
public:
    struct State;
    SignaturePlanner(const llvm::json::Object& request, std::string root, std::string anchor, unsigned offset);
    ~SignaturePlanner();
    // Discovery runs in every original AST before any edits. Collection replays the frozen inputs.
    bool discover(clang::ASTUnit& ast, std::string& error);
    bool found() const;
    bool collect(clang::ASTUnit& ast, std::string& error);
    std::optional<std::map<std::string, std::string>>
    candidates(const std::map<std::string, std::shared_ptr<const std::string>>& inputs, std::string& error) const;
    // Candidate direct-call binding is checked against each original compilation configuration.
    bool validate(clang::ASTUnit& ast, std::size_t configuration, std::string& error) const;

private:
    std::unique_ptr<State> state;
};
} // namespace frost::clang_engine
