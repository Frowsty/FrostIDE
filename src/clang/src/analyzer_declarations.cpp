// SPDX-License-Identifier: Apache-2.0
// Resolved class/function/lifetime inspections, with explicit narrow applicability.
#include "analyzer_ast.hpp"
#include "clang/AST/Attr.h"
#include "clang/Basic/SourceManager.h"

#include <algorithm>
namespace frost::clang_engine::analysis {
namespace {
const clang::Expr* unwrap(const clang::Expr* expression) {
    // IgnoreParenImpCasts also discards MaterializeTemporaryExpr, which carries lifetime evidence.
    while (expression) {
        const auto* inner = expression->IgnoreImpCasts()->IgnoreParens();
        if (inner == expression)
            break;
        expression = inner;
    }
    return expression;
}
bool temporaryString(const clang::Expr* expression) {
    expression = unwrap(expression);
    const auto* materialized = llvm::dyn_cast_or_null<clang::MaterializeTemporaryExpr>(expression);
    return materialized && materialized->getStorageDuration() == clang::SD_FullExpression &&
           stdRecord(materialized->getType(), "basic_string");
}
bool temporaryStringView(const clang::Expr* expression) {
    expression = unwrap(expression);
    while (const auto* cast = llvm::dyn_cast_or_null<clang::ExplicitCastExpr>(expression)) {
        if (cast->getCastKind() != clang::CK_NoOp || !stdRecord(cast->getType(), "basic_string_view"))
            break;
        expression = unwrap(cast->getSubExpr());
    }
    if (const auto* call = llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(expression)) {
        const auto* method = call->getMethodDecl();
        return llvm::isa_and_nonnull<clang::CXXConversionDecl>(method) &&
               stdRecord(call->getType(), "basic_string_view") && temporaryString(call->getImplicitObjectArgument());
    }
    if (const auto* construction = llvm::dyn_cast_or_null<clang::CXXConstructExpr>(expression);
        construction && construction->getNumArgs() && stdRecord(construction->getType(), "basic_string_view")) {
        const auto* call = llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(unwrap(construction->getArg(0)));
        const auto* method = call ? call->getMethodDecl() : nullptr;
        return method && (method->getNameAsString() == "data" || method->getNameAsString() == "c_str") &&
               temporaryString(call->getImplicitObjectArgument());
    }
    return false;
}
class Declarations final : public clang::RecursiveASTVisitor<Declarations> {
    AnalyzerResults& output;
    clang::ASTContext& context;

public:
    explicit Declarations(AnalyzerResults& o) : output(o), context(o.context) {}
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool TraverseDecl(clang::Decl* d) {
        if (d && d->getLocation().isValid() && context.getSourceManager().isInSystemHeader(d->getLocation()))
            return true;
        return clang::RecursiveASTVisitor<Declarations>::TraverseDecl(d);
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitCXXMethodDecl(clang::CXXMethodDecl* method) {
        if (method->isImplicit() || method->overridden_methods().empty() || method->hasAttr<clang::OverrideAttr>() ||
            method->hasAttr<clang::FinalAttr>())
            return true;
        if (method->doesThisDeclarationHaveABody() && method->getLexicalDeclContext() == method->getParent() &&
            !method->getTrailingRequiresClause()) {
            const auto begin = method->getBody()->getBeginLoc();
            if (auto token = text({begin, begin}, context)) {
                output.add("frost-missing-override", {begin, begin},
                           "Overriding method has no override/final annotation.", " override " + *token);
                return true;
            }
        }
        output.add("frost-missing-override", {method->getLocation(), method->getLocation()},
                   "Overriding method has no override/final annotation.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitCXXRecordDecl(clang::CXXRecordDecl* record) {
        if (!record->isThisDeclarationADefinition() || record->isImplicit() || !record->isPolymorphic() ||
            record->hasAttr<clang::FinalAttr>())
            return true;
        const auto* destructor = record->getDestructor();
        if (!destructor || (!destructor->isVirtual() && destructor->getAccess() == clang::AS_public))
            output.add(
                "frost-polymorphic-destructor", {record->getLocation(), record->getLocation()},
                "Polymorphic non-final class has a public nonvirtual destructor; base-pointer deletion may be unsafe.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitReturnStmt(clang::ReturnStmt* statement) {
        const auto* expression = unwrap(statement->getRetValue());
        if (!expression)
            return true;
        const auto* function = owner(context, statement);
        if (!function)
            return true;
        const auto* address = llvm::dyn_cast<clang::UnaryOperator>(expression);
        if (address && address->getOpcode() == clang::UO_AddrOf)
            if (const auto* variable = llvm::dyn_cast_or_null<clang::VarDecl>(reference(address->getSubExpr()));
                variable && variable->hasLocalStorage() && !variable->getType()->isReferenceType())
                output.add("frost-return-local-address", expression->getSourceRange(),
                           "Returning an address into automatic local storage whose lifetime ends on return.");
        const auto* returnedMove = expression;
        if (const auto* construction = llvm::dyn_cast<clang::CXXConstructExpr>(returnedMove);
            construction && construction->getNumArgs() == 1 &&
            context.hasSameUnqualifiedType(construction->getType(), function->getReturnType()))
            returnedMove = unwrap(construction->getArg(0));
        if (const auto* moved = movedArgument(returnedMove))
            if (const auto* variable = llvm::dyn_cast_or_null<clang::VarDecl>(reference(moved));
                variable && !llvm::isa<clang::ParmVarDecl>(variable) && variable->hasLocalStorage() &&
                !variable->getType().isVolatileQualified() && variable->getType()->isRecordType() &&
                context.hasSameUnqualifiedType(variable->getType(), function->getReturnType())) {
                auto bytes = text(moved->getSourceRange(), context);
                output.add("frost-return-move", returnedMove->getSourceRange(),
                           "Explicit std::move of this local inhibits named return value optimization.", bytes);
            }
        if (stdRecord(function->getReturnType(), "basic_string_view") && temporaryStringView(expression))
            output.add(
                "frost-dangling-string-view", expression->getSourceRange(),
                "Returned string_view refers to a temporary std::string destroyed at the end of this full expression.");
        if (stdRecord(function->getReturnType(), "initializer_list") &&
            llvm::isa<clang::CXXStdInitializerListExpr>(expression))
            output.add("frost-dangling-initializer-list", expression->getSourceRange(),
                       "Returning an initializer_list does not extend its temporary backing array lifetime.");
        if (function->getReturnType()->isReferenceType() && (llvm::isa<clang::MaterializeTemporaryExpr>(expression) ||
                                                             llvm::isa<clang::CXXBindTemporaryExpr>(expression)))
            output.add("frost-return-temporary-reference", expression->getSourceRange(),
                       "Returned reference binds temporary storage that expires on return.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr* call) {
        const auto* method = call->getMethodDecl();
        const auto* function = owner(context, call);
        const auto* member = llvm::dyn_cast_or_null<clang::MemberExpr>(clean(call->getCallee()));
        if (method && member && !member->hasQualifier() && method->isVirtual() && function &&
            (llvm::isa<clang::CXXConstructorDecl>(function) || llvm::isa<clang::CXXDestructorDecl>(function)) &&
            llvm::isa_and_nonnull<clang::CXXThisExpr>(clean(call->getImplicitObjectArgument())))
            output.add("frost-virtual-call-constructor", call->getSourceRange(),
                       "This-object virtual dispatch during construction/destruction does not dispatch to a "
                       "more-derived override.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitCXXConstructExpr(clang::CXXConstructExpr* expression) {
        if (!expression->getConstructor()->isCopyConstructor() || !expression->getNumArgs())
            return true;
        if (const auto* moved = movedArgument(expression->getArg(0)); moved && moved->getType().isConstQualified())
            output.add("frost-const-move-copies", expression->getSourceRange(),
                       "std::move of this const object selects the actual copy constructor, not a move constructor.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitCXXForRangeStmt(clang::CXXForRangeStmt* loop) {
        const auto* variable = loop->getLoopVariable();
        if (!variable || variable->getType()->isReferenceType())
            return true;
        const auto* construction = llvm::dyn_cast_or_null<clang::CXXConstructExpr>(unwrap(variable->getInit()));
        if (construction && construction->getConstructor()->isCopyConstructor() &&
            !construction->getConstructor()->isTrivial())
            output.add("frost-range-copy", variable->getSourceRange(),
                       "Range variable is initialized by a nontrivial record copy; consider a reference if an "
                       "independent copy is unnecessary.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitCXXCatchStmt(clang::CXXCatchStmt* handler) {
        const auto* variable = handler->getExceptionDecl();
        if (!variable || variable->getType()->isReferenceType())
            return true;
        const auto* record = variable->getType()->getAsCXXRecordDecl();
        if (record && record->hasDefinition() && record->isPolymorphic())
            output.add("frost-catch-polymorphic-value", variable->getSourceRange(),
                       "Catching a polymorphic record by value can slice a derived exception.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitCXXThrowExpr(clang::CXXThrowExpr* expression) {
        const auto* function = owner(context, expression);
        if (!function)
            return true;
        const auto* prototype = function->getType()->getAs<clang::FunctionProtoType>();
        if (!prototype || !prototype->isNothrow())
            return true;
        auto node = clang::DynTypedNode::create(*expression);
        for (unsigned depth = 0; depth < 128; ++depth) {
            const auto parents = context.getParents(node);
            if (parents.size() != 1)
                return true;
            if (parents[0].get<clang::CXXTryStmt>())
                return true;
            if (parents[0].get<clang::FunctionDecl>()) {
                output.add("frost-throw-noexcept", expression->getSourceRange(),
                           "Explicit throw escapes this noexcept function and therefore calls terminate.");
                return true;
            }
            node = parents[0];
        }
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitSwitchStmt(clang::SwitchStmt* statement) {
        auto type = clean(statement->getCond())->getType();
        const auto* enumeration = type->getAs<clang::EnumType>();
        if (!enumeration)
            return true;
        std::vector<llvm::APSInt> covered;
        for (const auto* item = statement->getSwitchCaseList(); item; item = item->getNextSwitchCase()) {
            if (llvm::isa<clang::DefaultStmt>(item))
                return true;
            const auto* value = llvm::cast<clang::CaseStmt>(item);
            if (value->getRHS())
                return true;
            if (auto constant = integer(value->getLHS(), context))
                covered.push_back(*constant);
        }
        std::string names;
        for (const auto* enumerator : enumeration->getDecl()->enumerators()) {
            const auto value = enumerator->getInitVal();
            if (std::ranges::none_of(covered,
                                     [&](const auto& actual) { return llvm::APSInt::isSameValue(actual, value); })) {
                if (!names.empty())
                    names += ", ";
                names += enumerator->getNameAsString();
            }
        }
        if (!names.empty())
            output.add("frost-enum-switch", statement->getCond()->getSourceRange(),
                       "Enum switch without default omits: " + names);
        return true;
    }
};
} // namespace
void analyzeDeclarations(AnalyzerResults& output) {
    Declarations visitor(output);
    visitor.TraverseDecl(output.context.getTranslationUnitDecl());
}
} // namespace frost::clang_engine::analysis
