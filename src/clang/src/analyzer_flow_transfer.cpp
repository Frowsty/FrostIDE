// SPDX-License-Identifier: Apache-2.0
// CFG-ordered local facts. A cast named std::move is never itself a consumed move.
#include "analyzer_flow.hpp"
namespace frost::clang_engine::flow {
namespace {
using namespace analysis;
const clang::VarDecl* local(const clang::Expr* e) {
    const auto* variable = llvm::dyn_cast_or_null<clang::VarDecl>(reference(e));
    return variable && variable->hasLocalStorage() ? variable->getCanonicalDecl() : nullptr;
}
class Escapes final : public clang::RecursiveASTVisitor<Escapes> {
public:
    std::set<const clang::VarDecl*>& excluded;
    explicit Escapes(std::set<const clang::VarDecl*>& e) : excluded(e) {}
    bool VisitUnaryOperator(clang::UnaryOperator* e) {
        if (e->getOpcode() == clang::UO_AddrOf)
            if (auto* v = local(e->getSubExpr()))
                excluded.insert(v);
        return true;
    }
    bool VisitVarDecl(clang::VarDecl* d) {
        if (d->getType()->isLValueReferenceType() && !d->getType()->getPointeeType().isConstQualified())
            if (auto* v = local(d->getInit()))
                excluded.insert(v);
        return true;
    }
    bool VisitCXXConstructExpr(clang::CXXConstructExpr* construction) {
        const auto* constructor = construction->getConstructor();
        for (unsigned i = 0; i < constructor->getNumParams() && i < construction->getNumArgs(); ++i) {
            const auto type = constructor->getParamDecl(i)->getType();
            if (type->isLValueReferenceType() && !type->getPointeeType().isConstQualified())
                if (auto* v = local(construction->getArg(i)))
                    excluded.insert(v);
        }
        return true;
    }
    bool VisitLambdaExpr(clang::LambdaExpr* lambda) {
        for (const auto& capture : lambda->captures()) {
            if (!capture.capturesVariable() || capture.getCaptureKind() != clang::LCK_ByRef)
                continue;
            if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(capture.getCapturedVar()))
                excluded.insert(variable->getCanonicalDecl());
        }
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* call) {
        const auto* function = call->getDirectCallee();
        if (!function)
            return true;
        if (inStd(function->getDeclContext()) &&
            (function->getNameAsString() == "move" || function->getNameAsString() == "forward"))
            return true;
        unsigned argument =
            llvm::isa<clang::CXXOperatorCallExpr>(call) && llvm::isa<clang::CXXMethodDecl>(function) ? 1 : 0;
        for (unsigned i = 0; i < function->getNumParams() && i + argument < call->getNumArgs(); ++i) {
            const auto type = function->getParamDecl(i)->getType();
            if (type->isLValueReferenceType() && !type->getPointeeType().isConstQualified())
                if (auto* v = local(call->getArg(i + argument)))
                    excluded.insert(v);
        }
        return true;
    }
};
const clang::Expr* unwrap(const clang::Expr* e) {
    e = clean(e);
    while (e) {
        if (const auto* c = llvm::dyn_cast<clang::ExprWithCleanups>(e))
            e = clean(c->getSubExpr());
        else if (const auto* m = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(e))
            e = clean(m->getSubExpr());
        else if (const auto* b = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e))
            e = clean(b->getSubExpr());
        else
            break;
    }
    return e;
}
bool nullopt(const clang::Expr* e) { return e && stdRecord(e->getType(), "nullopt_t"); }
class Transfer final : public clang::RecursiveASTVisitor<Transfer> {
    FunctionFlow& flow;
    State& state;
    const clang::Stmt* root;
    bool emit;
    std::set<const clang::VarDecl*> writeOnly;

public:
    Transfer(FunctionFlow& f, State& s, const clang::Stmt* r, bool e) : flow(f), state(s), root(r), emit(e) {}
    bool shouldTraversePostOrder() const { return true; }
    bool TraverseStmt(clang::Stmt* statement) {
        if (!statement || (statement != root && flow.roots.contains(statement)))
            return true;
        return clang::RecursiveASTVisitor<Transfer>::TraverseStmt(statement);
    }
    bool TraverseLambdaExpr(clang::LambdaExpr* lambda) {
        for (auto* init : lambda->capture_inits())
            if (!TraverseStmt(init))
                return false;
        return true;
    }
    bool TraverseCXXOperatorCallExpr(clang::CXXOperatorCallExpr* call) {
        const bool assignment = call->getOperator() == clang::OO_Equal && call->getNumArgs() > 1;
        const auto* target = assignment ? flow.variable(call->getArg(0)) : nullptr;
        if (target)
            writeOnly.insert(target);
        const auto result = clang::RecursiveASTVisitor<Transfer>::TraverseCXXOperatorCallExpr(call);
        if (target)
            writeOnly.erase(target);
        return result;
    }
    Null pointer(const clang::Expr* expression) {
        const auto* e = unwrap(expression);
        if (!e)
            return Null::Unknown;
        if (e->isNullPointerConstant(flow.output.context, clang::Expr::NPC_ValueDependentIsNotNull) !=
            clang::Expr::NPCK_NotNull)
            return Null::Null;
        if (const auto* v = flow.variable(e))
            return state[v].pointer;
        if (const auto* op = llvm::dyn_cast<clang::UnaryOperator>(e); op && op->getOpcode() == clang::UO_AddrOf)
            return Null::NonNull;
        if (llvm::isa<clang::StringLiteral>(e))
            return Null::NonNull;
        if (const auto* allocation = llvm::dyn_cast<clang::CXXNewExpr>(e);
            allocation && !allocation->getNumPlacementArgs()) {
            const auto* prototype = allocation->getOperatorNew()->getType()->getAs<clang::FunctionProtoType>();
            if (prototype && !prototype->isNothrow())
                return Null::NonNull;
        }
        return Null::Unknown;
    }
    Fact value(const clang::Expr* expression) {
        Fact result;
        const auto* e = unwrap(expression);
        if (!e)
            return result;
        if (const auto* v = flow.variable(e))
            return state[v];
        if (e->getType()->isPointerType() ||
            e->isNullPointerConstant(flow.output.context, clang::Expr::NPC_ValueDependentIsNotNull) !=
                clang::Expr::NPCK_NotNull)
            result.pointer = pointer(e);
        if (const auto* allocation = llvm::dyn_cast<clang::CXXNewExpr>(e))
            result.allocation = allocation->isArray() ? Allocation::Array : Allocation::Scalar;
        if (nullopt(e))
            result.optional = Optional::Empty;
        if (stdRecord(e->getType(), "optional"))
            if (const auto* constructor = llvm::dyn_cast<clang::CXXConstructExpr>(e)) {
                if (constructor->getConstructor()->isDefaultConstructor())
                    result.optional = Optional::Empty;
                else if (constructor->getNumArgs()) {
                    const auto* argument = unwrap(constructor->getArg(0));
                    if (nullopt(argument))
                        result.optional = Optional::Empty;
                    else if (const auto* source = flow.variable(argument);
                             source && stdRecord(source->getType(), "optional"))
                        result.optional = state[source].optional;
                    else if (argument && !stdRecord(argument->getType(), "optional") &&
                             !llvm::isa<clang::CXXDefaultArgExpr>(argument))
                        result.optional = Optional::Engaged;
                }
            }
        return result;
    }
    void warn(const char* id, const clang::Stmt& e, const char* message) {
        if (emit)
            flow.output.add(id, e.getSourceRange(), message);
    }
    void dereference(const clang::Expr* operand, const clang::Stmt& e) {
        if (pointer(operand) == Null::Null)
            warn("frost-null-dereference", e, "This local pointer is null on every incoming live CFG path.");
    }
    bool VisitUnaryOperator(clang::UnaryOperator* e) {
        if (e->getOpcode() == clang::UO_Deref)
            dereference(e->getSubExpr(), *e);
        if (e->isIncrementDecrementOp())
            if (const auto* v = flow.variable(e->getSubExpr()))
                state[v] = Fact{};
        return true;
    }
    bool VisitMemberExpr(clang::MemberExpr* e) {
        if (e->isArrow())
            dereference(e->getBase(), *e);
        return true;
    }
    bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr* e) {
        dereference(e->getBase(), *e);
        return true;
    }
    bool VisitDeclStmt(clang::DeclStmt* e) {
        for (const auto* d : e->decls())
            if (const auto* v = llvm::dyn_cast<clang::VarDecl>(d); v && !flow.excluded.contains(v->getCanonicalDecl()))
                state[v->getCanonicalDecl()] = value(v->getInit());
        return true;
    }
    bool VisitBinaryOperator(clang::BinaryOperator* e) {
        if (e->getOpcode() == clang::BO_Assign) {
            if (const auto* v = flow.variable(e->getLHS()))
                state[v] = value(e->getRHS());
        } else if (e->isCompoundAssignmentOp()) {
            if (const auto* v = flow.variable(e->getLHS()))
                state[v] = Fact{};
        }
        return true;
    }
    bool safeMemberUse(const clang::DeclRefExpr* reference) {
        auto node = clang::DynTypedNode::create(*reference);
        for (unsigned i = 0; i < 8; ++i) {
            const auto parents = flow.output.context.getParents(node);
            if (parents.size() != 1)
                return false;
            if (const auto* call = parents[0].get<clang::CXXMemberCallExpr>()) {
                const auto* method = call->getMethodDecl();
                if (!method || !inStd(method->getParent()->getDeclContext()))
                    return false;
                return method->getNameAsString() == "clear" || method->getNameAsString() == "assign" ||
                       method->getNameAsString() == "reset" || method->getNameAsString() == "emplace";
            }
            if (!parents[0].get<clang::MemberExpr>() && !parents[0].get<clang::ImplicitCastExpr>() &&
                !parents[0].get<clang::ParenExpr>())
                return false;
            node = parents[0];
        }
        return false;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* e) {
        if (const auto* v = flow.variable(e);
            v && !writeOnly.contains(v) && !safeMemberUse(e) && state[v].moved == Moved::Moved)
            warn("frost-use-after-move", *e,
                 "This local record was consumed by an actual nontrivial move on every incoming live CFG path.");
        return true;
    }
    void consumed(const clang::Expr* e) {
        const auto* source = movedArgument(unwrap(e));
        if (!source) {
            source = unwrap(e);
            if (!source || !source->isXValue())
                return;
            if (const auto* cast = llvm::dyn_cast<clang::ExplicitCastExpr>(source))
                source = clean(cast->getSubExpr());
        }
        if (const auto* v = flow.variable(source))
            state[v].moved = Moved::Moved;
    }
    bool VisitCXXConstructExpr(clang::CXXConstructExpr* e) {
        if (e->getConstructor()->isMoveConstructor() && !e->getConstructor()->isTrivial() && e->getNumArgs())
            consumed(e->getArg(0));
        return true;
    }
    bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr* e) {
        const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(e->getDirectCallee());
        if (e->getOperator() == clang::OO_Equal && e->getNumArgs() > 1) {
            if (method && method->isMoveAssignmentOperator() && !method->isTrivial())
                consumed(e->getArg(1));
            if (const auto* target = flow.variable(e->getArg(0))) {
                auto replacement = value(e->getArg(1));
                replacement.moved = Moved::Available;
                if (stdRecord(target->getType(), "optional") && !stdRecord(e->getArg(1)->getType(), "optional"))
                    replacement.optional = nullopt(unwrap(e->getArg(1))) ? Optional::Empty : Optional::Engaged;
                state[target] = replacement;
            }
        }
        if (e->getOperator() == clang::OO_Star && e->getNumArgs() && method &&
            stdRecord(e->getArg(0)->getType(), "optional"))
            if (const auto* v = flow.variable(e->getArg(0)); v && state[v].optional == Optional::Empty)
                warn("frost-empty-optional", *e, "This std::optional is empty on every incoming live CFG path.");
        return true;
    }
    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr* call) {
        const auto* method = call->getMethodDecl();
        const auto* v = flow.variable(call->getImplicitObjectArgument());
        if (!method || !v || !inStd(method->getParent()->getDeclContext()))
            return true;
        const auto name = method->getNameAsString();
        if (stdRecord(v->getType(), "optional")) {
            if ((name == "value" || name == "operator*") && state[v].optional == Optional::Empty)
                warn("frost-empty-optional", *call, "This std::optional is empty on every incoming live CFG path.");
            if (name == "reset")
                state[v].optional = Optional::Empty;
            else if (name == "emplace")
                state[v].optional = Optional::Engaged;
            else if (!method->isConst() && name != "value" && name != "operator*" && name != "operator->")
                state[v].optional = Optional::Unknown;
        }
        if (name == "clear" || name == "assign" || name == "reset" || name == "emplace")
            state[v].moved = Moved::Available;
        return true;
    }
    bool VisitCXXDeleteExpr(clang::CXXDeleteExpr* e) {
        if (const auto* v = flow.variable(e->getArgument())) {
            const auto allocation = state[v].allocation;
            if ((allocation == Allocation::Array && !e->isArrayForm()) ||
                (allocation == Allocation::Scalar && e->isArrayForm()))
                warn("frost-delete-kind", *e,
                     "Delete form disagrees with the allocation kind proven on every incoming live CFG path.");
            state[v] = Fact{};
        }
        return true;
    }
};
} // namespace
FunctionFlow::FunctionFlow(AnalyzerResults& o, const clang::FunctionDecl& f, const clang::CFG& g)
    : output(o), function(f), cfg(g) {
    for (const auto* block : cfg)
        for (const auto& element : *block)
            if (auto statement = element.getAs<clang::CFGStmt>())
                roots.insert(statement->getStmt());
    Escapes scan(excluded);
    scan.TraverseStmt(const_cast<clang::Stmt*>(function.getBody()));
}
const clang::VarDecl* FunctionFlow::variable(const clang::Expr* e) const {
    const auto* v = local(e);
    return v && !excluded.contains(v) ? v : nullptr;
}
void FunctionFlow::transfer(const clang::CFGBlock& block, State& state, bool emit) {
    for (const auto& element : block)
        if (auto statement = element.getAs<clang::CFGStmt>()) {
            Transfer visitor(*this, state, statement->getStmt(), emit);
            visitor.TraverseStmt(const_cast<clang::Stmt*>(statement->getStmt()));
        }
}
} // namespace frost::clang_engine::flow
