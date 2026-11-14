// SPDX-License-Identifier: Apache-2.0
// REF-8 safe deletion uses canonical AST references, never identifier text searches.
#include "ast_refactoring_operations.hpp"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Lex/Lexer.h"

#include <iterator>
namespace frost::clang_engine {
namespace {
class Selection final : public clang::RecursiveASTVisitor<Selection> {
public:
    clang::ASTContext& context;
    unsigned offset;
    clang::ValueDecl* selected = nullptr;
    bool ambiguous = false;
    Selection(clang::ASTContext& context, unsigned offset) : context(context), offset(offset) {}
    void consider(clang::ValueDecl* declaration, clang::SourceLocation location) {
        auto& sm = context.getSourceManager();
        if (!location.isFileID() || !sm.isWrittenInMainFile(location))
            return;
        const auto begin = sm.getFileOffset(location);
        if (offset < begin || offset - begin >= clang::Lexer::MeasureTokenLength(location, sm, context.getLangOpts()))
            return;
        ambiguous |= selected && selected->getCanonicalDecl() != declaration->getCanonicalDecl();
        selected = declaration;
    }
    bool VisitVarDecl(clang::VarDecl* d) {
        consider(d, d->getLocation());
        return true;
    }
    bool VisitFunctionDecl(clang::FunctionDecl* d) {
        consider(d, d->getLocation());
        return true;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* e) {
        consider(e->getDecl(), e->getLocation());
        return true;
    }
};
class Uses final : public clang::RecursiveASTVisitor<Uses> {
public:
    const clang::Decl* target;
    bool used = false, opaque = false;
    explicit Uses(const clang::Decl* d) : target(d->getCanonicalDecl()) {}
    bool VisitDeclRefExpr(clang::DeclRefExpr* e) {
        used |= e->getDecl()->getCanonicalDecl() == target;
        return true;
    }
    bool VisitStmt(clang::Stmt* s) {
        opaque |= llvm::isa<clang::AsmStmt>(s);
        return true;
    }
    bool VisitOverloadExpr(clang::OverloadExpr* expression) {
        for (const auto* declaration : expression->decls())
            used |= declaration->getCanonicalDecl() == target;
        return true;
    }
    bool VisitUsingShadowDecl(clang::UsingShadowDecl* declaration) {
        used |= declaration->getTargetDecl()->getCanonicalDecl() == target;
        return true;
    }
    bool VisitFileScopeAsmDecl(clang::FileScopeAsmDecl*) {
        opaque = true;
        return true;
    }
};
class Initializer final : public clang::RecursiveASTVisitor<Initializer> {
public:
    bool safe = true;
    bool VisitExpr(clang::Expr* e) {
        safe &=
            e->getBeginLoc().isFileID() && e->getEndLoc().isFileID() && !e->getType().isVolatileQualified() &&
            llvm::isa<clang::IntegerLiteral, clang::FloatingLiteral, clang::CharacterLiteral, clang::CXXBoolLiteralExpr,
                      clang::CXXNullPtrLiteralExpr, clang::UnaryOperator, clang::BinaryOperator,
                      clang::ConditionalOperator, clang::ParenExpr, clang::ImplicitCastExpr, clang::DeclRefExpr>(e);
        return safe;
    }
};
} // namespace
std::optional<std::string> refactorSafeDelete(clang::ASTUnit& ast, const llvm::json::Object& request,
                                              std::string& error) {
    const auto offset = request.getInteger("offset");
    auto refuse = [&](std::string why) -> std::optional<std::string> {
        error = std::move(why);
        return {};
    };
    auto& context = ast.getASTContext();
    auto& sm = ast.getSourceManager();
    const auto bytes = sm.getBufferData(sm.getMainFileID());
    if (!offset || *offset < 0 || static_cast<std::uint64_t>(*offset) >= bytes.size())
        return refuse("Invalid safe-delete selection");
    Selection selection(context, static_cast<unsigned>(*offset));
    selection.TraverseDecl(context.getTranslationUnitDecl());
    if (!selection.selected || selection.ambiguous)
        return refuse("Select one canonical automatic local or internal function");
    Uses uses(selection.selected);
    uses.TraverseDecl(context.getTranslationUnitDecl());
    if (uses.used || uses.opaque)
        return refuse("Safe delete found a real reference or opaque assembly use");
    clang::SourceRange range;
    if (auto* variable = llvm::dyn_cast<clang::VarDecl>(selection.selected)) {
        if (llvm::isa<clang::ParmVarDecl>(variable) || !variable->isLocalVarDecl() || variable->isStaticLocal() ||
            !variable->getType()->isScalarType() || variable->getType().isVolatileQualified() || variable->hasAttrs())
            return refuse("Deletion requires an automatic non-volatile scalar local");
        const auto parents = context.getParents(*variable);
        const auto* statement = parents.size() == 1 ? parents[0].get<clang::DeclStmt>() : nullptr;
        if (!statement || !statement->isSingleDecl())
            return refuse("The local must have its own declaration statement");
        const auto statementParents = context.getParents(*statement);
        if (statementParents.size() != 1 || !statementParents[0].get<clang::CompoundStmt>())
            return refuse("Safe-delete local declarations must be direct compound-block statements");
        if (variable->hasInit()) {
            Initializer initializer;
            initializer.TraverseStmt(variable->getInit());
            if (!initializer.safe || variable->getInit()->HasSideEffects(context))
                return refuse("Deletion would discard initializer effects or unsupported evaluation");
        }
        range = statement->getSourceRange();
    } else if (auto* function = llvm::dyn_cast<clang::FunctionDecl>(selection.selected)) {
        if (llvm::isa<clang::CXXMethodDecl>(function) || !function->doesThisDeclarationHaveABody() ||
            function->hasAttrs() || function->getFormalLinkage() != clang::Linkage::Internal ||
            function->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate ||
            std::distance(function->redecls_begin(), function->redecls_end()) != 1)
            return refuse("Function deletion requires one unreferenced internal non-template free-function definition");
        range = function->getSourceRange();
    } else
        return refuse("Declaration is outside the safe-delete profile");
    if (!range.getBegin().isFileID() || !range.getEnd().isFileID() || !sm.isWrittenInMainFile(range.getBegin()) ||
        !sm.isWrittenInMainFile(range.getEnd()))
        return refuse("Deletion range is generated or outside this file");
    const auto end = clang::Lexer::getLocForEndOfToken(range.getEnd(), 0, sm, context.getLangOpts());
    if (end.isInvalid())
        return refuse("Deletion token boundary unavailable");
    const auto beginOffset = sm.getFileOffset(range.getBegin()), endOffset = sm.getFileOffset(end);
    if (endOffset > bytes.size() || beginOffset >= endOffset)
        return refuse("Invalid deletion range");
    auto candidate = bytes.str();
    candidate.erase(beginOffset, endOffset - beginOffset);
    return candidate;
}
} // namespace frost::clang_engine
