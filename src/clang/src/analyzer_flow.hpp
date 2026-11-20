// SPDX-License-Identifier: Apache-2.0
// Bounded must-fact lattice and CFG transfer seam; joins discard facts not true on every live edge.
#pragma once
#include "analyzer_ast.hpp"
#include "clang/Analysis/CFG.h"

#include <map>
namespace frost::clang_engine::flow {
enum class Null { Unknown, Null, NonNull };
enum class Moved { Unknown, Available, Moved };
enum class Optional { Unknown, Empty, Engaged };
enum class Allocation { Unknown, Scalar, Array };
struct Fact {
    Null pointer = Null::Unknown;
    Moved moved = Moved::Available;
    Optional optional = Optional::Unknown;
    Allocation allocation = Allocation::Unknown;
    bool operator==(const Fact&) const = default;
};
using State = std::map<const clang::VarDecl*, Fact>;
struct FunctionFlow {
    AnalyzerResults& output;
    const clang::FunctionDecl& function;
    const clang::CFG& cfg;
    std::set<const clang::Stmt*> roots;
    std::set<const clang::VarDecl*> excluded;
    FunctionFlow(AnalyzerResults&, const clang::FunctionDecl&, const clang::CFG&);
    void transfer(const clang::CFGBlock&, State&, bool emit);
    [[nodiscard]] std::optional<State> edge(const clang::CFGBlock&, unsigned successor, State) const;
    [[nodiscard]] const clang::VarDecl* variable(const clang::Expr*) const;
};
} // namespace frost::clang_engine::flow
