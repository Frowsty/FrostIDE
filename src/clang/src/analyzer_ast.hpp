// SPDX-License-Identifier: Apache-2.0
// Small shared AST predicates; all facts come from resolved Clang types/declarations.
#pragma once
#include "analyzer.hpp"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Lex/Lexer.h"
namespace frost::clang_engine::analysis {
inline const clang::Expr* clean(const clang::Expr* e) { return e ? e->IgnoreParenImpCasts() : nullptr; }
inline const clang::ValueDecl* reference(const clang::Expr* e) {
    const auto* r = llvm::dyn_cast_or_null<clang::DeclRefExpr>(clean(e));
    return r && !r->getType().isVolatileQualified() ? r->getDecl() : nullptr;
}
inline bool identical(const clang::Expr* a, const clang::Expr* b) {
    return reference(a) && reference(a) == reference(b);
}
inline bool inStd(const clang::DeclContext* context) {
    for (; context; context = context->getParent())
        if (const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(context); ns && ns->isStdNamespace())
            return true;
    return false;
}
inline bool stdRecord(clang::QualType type, llvm::StringRef name) {
    if (type.isNull())
        return false;
    const auto* record = type.getNonReferenceType()->getAsCXXRecordDecl();
    return record && record->getName() == name && inStd(record->getDeclContext());
}
inline const clang::Expr* movedArgument(const clang::Expr* e) {
    const auto* call = llvm::dyn_cast_or_null<clang::CallExpr>(clean(e));
    const auto* callee = call ? call->getDirectCallee() : nullptr;
    return callee && callee->getNameAsString() == "move" && inStd(callee->getDeclContext()) && call->getNumArgs() == 1
               ? clean(call->getArg(0))
               : nullptr;
}
inline std::optional<llvm::APSInt> integer(const clang::Expr* e, clang::ASTContext& context) {
    if (!e || e->isValueDependent() || e->HasSideEffects(context))
        return {};
    clang::Expr::EvalResult result;
    return e->EvaluateAsInt(result, context) ? std::optional<llvm::APSInt>(result.Val.getInt()) : std::nullopt;
}
inline std::optional<std::string> text(clang::SourceRange range, clang::ASTContext& context) {
    if (!range.getBegin().isFileID() || !range.getEnd().isFileID())
        return {};
    bool invalid = false;
    auto bytes = clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(range), context.getSourceManager(),
                                             context.getLangOpts(), &invalid);
    return invalid ? std::nullopt : std::optional<std::string>(bytes.str());
}
inline const clang::ReturnStmt* singleReturn(const clang::Stmt* s) {
    if (const auto* block = llvm::dyn_cast_or_null<clang::CompoundStmt>(s)) {
        if (block->size() != 1)
            return nullptr;
        s = *block->body_begin();
    }
    return llvm::dyn_cast_or_null<clang::ReturnStmt>(s);
}
inline const clang::FunctionDecl* owner(clang::ASTContext& context, const clang::Stmt* statement) {
    auto node = clang::DynTypedNode::create(*statement);
    for (unsigned depth = 0; depth < 128; ++depth) {
        const auto parents = context.getParents(node);
        if (parents.size() != 1)
            return nullptr;
        if (const auto* function = parents[0].get<clang::FunctionDecl>())
            return function;
        node = parents[0];
    }
    return nullptr;
}
void analyzeDeclarations(AnalyzerResults&);
} // namespace frost::clang_engine::analysis
