// SPDX-License-Identifier: Apache-2.0
// Genuine Clang CFG must-analysis: fixed point first, diagnostics only after convergence.
#include "analyzer_flow.hpp"
#include "clang/Basic/SourceManager.h"
#include <algorithm>
#include <deque>
namespace frost::clang_engine::flow {
std::optional<State> FunctionFlow::edge(const clang::CFGBlock& block,unsigned successor,State state) const {
    if(block.succ_size()!=2) return state;
    const auto* condition=llvm::dyn_cast_or_null<clang::Expr>(block.getTerminatorCondition());
    condition=analysis::clean(condition); if(!condition) return state;
    bool truth=successor==0;
    if(const auto* negated=llvm::dyn_cast<clang::UnaryOperator>(condition);negated && negated->getOpcode()==clang::UO_LNot) {truth=!truth; condition=analysis::clean(negated->getSubExpr());}
    const clang::VarDecl* pointer=nullptr; bool nonnull=truth;
    if(const auto* direct=variable(condition);direct && direct->getType()->isPointerType()) pointer=direct;
    else if(const auto* comparison=llvm::dyn_cast<clang::BinaryOperator>(condition);comparison && comparison->isEqualityOp()) {
        const auto* left=variable(comparison->getLHS()); const auto* right=variable(comparison->getRHS());
        const auto isNull=[&](const clang::Expr* e){return e->isNullPointerConstant(output.context,clang::Expr::NPC_ValueDependentIsNotNull)!=clang::Expr::NPCK_NotNull;};
        if(left && left->getType()->isPointerType() && isNull(comparison->getRHS())) pointer=left;
        else if(right && right->getType()->isPointerType() && isNull(comparison->getLHS())) pointer=right;
        if(pointer) nonnull=truth==(comparison->getOpcode()==clang::BO_NE);
    }
    if(pointer) {
        auto& fact=state[pointer]; const auto expected=nonnull ? Null::NonNull : Null::Null;
        if(fact.pointer!=Null::Unknown && fact.pointer!=expected) return {};
        fact.pointer=expected; return state;
    }
    const auto* call=llvm::dyn_cast<clang::CXXMemberCallExpr>(condition);
    const auto* method=call ? call->getMethodDecl() : nullptr;
    const auto* optional=call ? variable(call->getImplicitObjectArgument()) : nullptr;
    if(optional && method && analysis::stdRecord(optional->getType(),"optional") &&
        (method->getNameAsString()=="has_value" || (llvm::isa<clang::CXXConversionDecl>(method) && llvm::cast<clang::CXXConversionDecl>(method)->getConversionType()->isBooleanType()))) {
        auto& fact=state[optional]; const auto expected=truth ? Optional::Engaged : Optional::Empty;
        if(fact.optional!=Optional::Unknown && fact.optional!=expected) return {};
        fact.optional=expected;
    }
    return state;
}
namespace {
Fact joinFact(Fact a,const Fact& b) {
    if(a.pointer!=b.pointer) a.pointer=Null::Unknown;
    if(a.moved!=b.moved) a.moved=Moved::Unknown;
    if(a.optional!=b.optional) a.optional=Optional::Unknown;
    if(a.allocation!=b.allocation) a.allocation=Allocation::Unknown;
    return a;
}
State join(const State& a,const State& b) {
    State result;
    for(const auto& [variable,fact]:a) {
        const auto found=b.find(variable); result[variable]=joinFact(fact,found==b.end() ? Fact{} : found->second);
    }
    for(const auto& [variable,fact]:b) if(!result.contains(variable)) result[variable]=joinFact(Fact{},fact);
    return result;
}
bool explicitStatement(const clang::Stmt* statement) {
    return llvm::isa<clang::ReturnStmt,clang::BinaryOperator,clang::CallExpr,clang::CXXDeleteExpr,clang::DeclStmt>(statement);
}
void function(AnalyzerResults& output,const clang::FunctionDecl& declaration) {
    clang::CFG::BuildOptions options;
    options.PruneTriviallyFalseEdges=true;
    auto graph=clang::CFG::buildCFG(&declaration,const_cast<clang::Stmt*>(declaration.getBody()),&output.context,options);
    if(!graph || graph->getNumBlockIDs()>4096) {output.limited(declaration,"CFG unavailable or exceeds4096 blocks");return;}
    FunctionFlow flow(output,declaration,*graph);
    std::vector<std::optional<State>> incoming(graph->getNumBlockIDs());
    incoming[graph->getEntry().getBlockID()]=State{};
    std::deque<const clang::CFGBlock*> work{&graph->getEntry()};
    unsigned steps=0;
    while(!work.empty()) {
        if(++steps>65536) {output.limited(declaration,"CFG fixed point exceeds65536 transfers");return;}
        const auto* block=work.front(); work.pop_front(); State state=*incoming[block->getBlockID()];
        flow.transfer(*block,state,false);
        if(state.size()>512) {output.limited(declaration,"Local fact budget exceeds512 variables");return;}
        unsigned ordinal=0;
        for(const auto& adjacent:block->succs()) {
            const auto* successor=adjacent.getReachableBlock();
            auto edge=successor ? flow.edge(*block,ordinal,state) : std::nullopt; ++ordinal;
            if(!successor || !edge) continue;
            auto& previous=incoming[successor->getBlockID()];
            auto merged=previous ? join(*previous,*edge) : std::move(*edge);
            if(!previous || *previous!=merged) {previous=std::move(merged);work.push_back(successor);}
        }
    }
    for(const auto* block:*graph) {
        if(incoming[block->getBlockID()]) {auto state=*incoming[block->getBlockID()];flow.transfer(*block,state,true);}
        else for(const auto& element:*block) if(auto statement=element.getAs<clang::CFGStmt>();statement && explicitStatement(statement->getStmt())) output.add("frost-unreachable",statement->getStmt()->getSourceRange(),"Statement is unreachable from this function's Clang CFG entry.");
    }
}
class Functions final:public clang::RecursiveASTVisitor<Functions> {
    AnalyzerResults& output; std::set<const clang::FunctionDecl*> seen;
public:
    explicit Functions(AnalyzerResults& o):output(o){}
    bool TraverseDecl(clang::Decl* declaration) {
        if(declaration && declaration->getLocation().isValid() && output.context.getSourceManager().isInSystemHeader(declaration->getLocation())) return true;
        return clang::RecursiveASTVisitor<Functions>::TraverseDecl(declaration);
    }
    bool VisitFunctionDecl(clang::FunctionDecl* declaration) {
        if(declaration->doesThisDeclarationHaveABody() && !declaration->isDependentContext() && !declaration->isImplicit() && seen.insert(declaration).second) function(output,*declaration);
        return true;
    }
};
}
}
namespace frost::clang_engine {
void analyzeDataflow(AnalyzerResults& output) {flow::Functions visitor(output);visitor.TraverseDecl(output.context.getTranslationUnitDecl());}
}
