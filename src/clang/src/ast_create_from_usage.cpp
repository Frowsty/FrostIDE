// SPDX-License-Identifier: Apache-2.0
// Recover selected unresolved calls from Clang, not diagnostic prose or guessed argument syntax.
#include "ast_create_from_usage.hpp"
#include "semantic_snapshot.hpp"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Lex/Lexer.h"
#include <set>
namespace frost::clang_engine {
CreateUsageDiagnostics::CreateUsageDiagnostics(std::string p, std::string r, unsigned o)
    : path(std::move(p)), root(std::move(r)), offset(o) {}
void CreateUsageDiagnostics::HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& diagnostic) {
    if (++count > 1024) { unrelated = failed = true; return; }
    if (level < clang::DiagnosticsEngine::Error) return;
    failed = true;
    const auto location = diagnostic.getLocation();
    if (!diagnostic.hasSourceManager() || !location.isFileID()) { unrelated = true; return; }
    const auto& sm = diagnostic.getSourceManager();
    const auto begin = sm.getFileOffset(location);
    clang::Token token;
    const auto id = diagnostic.getID();
    const bool unresolved = id == clang::diag::err_undeclared_var_use || id == clang::diag::err_undeclared_use || id == clang::diag::err_no_member;
    if (!unresolved || semantic::absolute(sm.getFilename(location), root) != path ||
        clang::Lexer::getRawToken(location, token, sm, clang::LangOptions{}, true) ||
        offset < begin || offset >= begin + token.getLength()) { unrelated = true; return; }
    selectedError = true;
}
namespace {
class Calls final : public clang::RecursiveASTVisitor<Calls> {
public:
    clang::ASTContext& context;
    std::string path, root;
    unsigned offset;
    clang::Expr* call = nullptr;
    clang::Expr* base = nullptr;
    clang::UnresolvedLookupExpr* lookup = nullptr;
    std::vector<clang::Expr*> arguments;
    unsigned width = ~0u;
    Calls(clang::ASTContext& c, std::string p, std::string r, unsigned o)
        : context(c), path(std::move(p)), root(std::move(r)), offset(o) {}
    bool covers(clang::SourceRange range) {
        auto& sm = context.getSourceManager();
        if (!range.getBegin().isFileID() || !range.getEnd().isFileID() ||
            sm.getFileID(range.getBegin()) != sm.getFileID(range.getEnd())) return false;
        const auto end = clang::Lexer::getLocForEndOfToken(range.getEnd(), 0, sm, context.getLangOpts());
        return end.isFileID() && semantic::absolute(sm.getFilename(range.getBegin()), root) == path &&
            sm.getFileOffset(range.getBegin()) <= offset && offset < sm.getFileOffset(end);
    }
    bool VisitRecoveryExpr(clang::RecoveryExpr* expression) {
        if (!covers(expression->getSourceRange())) return true;
        const auto children = expression->subExpressions();
        if (children.empty()) return true;
        auto* unresolved = llvm::dyn_cast<clang::UnresolvedLookupExpr>(children.front()->IgnoreParenImpCasts());
        if (!unresolved || unresolved->getNumDecls() || unresolved->hasExplicitTemplateArgs() || unresolved->getQualifier()) return true;
        if (!covers(unresolved->getSourceRange())) return true;
        const auto span = context.getSourceManager().getFileOffset(expression->getEndLoc()) - context.getSourceManager().getFileOffset(expression->getBeginLoc());
        if (span < width) { width = span; call = expression; lookup = unresolved; base = nullptr; arguments.assign(children.begin()+1, children.end()); }
        return true;
    }
    bool VisitCallExpr(clang::CallExpr* expression) {
        if (!covers(expression->getSourceRange())) return true;
        auto* recovery = llvm::dyn_cast<clang::RecoveryExpr>(expression->getCallee()->IgnoreParenImpCasts());
        if (!recovery || !covers(recovery->getSourceRange()) || recovery->subExpressions().size() != 1) return true;
        auto* object = recovery->subExpressions().front();
        if (object->containsErrors() || object->getType().isNull() || object->isTypeDependent()) return true;
        const auto span = context.getSourceManager().getFileOffset(expression->getEndLoc()) - context.getSourceManager().getFileOffset(expression->getBeginLoc());
        if (span < width) { width = span; call = expression; base = object; lookup = nullptr; arguments.assign(expression->arg_begin(), expression->arg_end()); }
        return true;
    }
};
const clang::FunctionDecl* owner(clang::ASTContext& context, const clang::Stmt* statement) {
    clang::DynTypedNode node = clang::DynTypedNode::create(*statement);
    for (unsigned depth = 0; depth < 128; ++depth) {
        const auto parents = context.getParents(node); if (parents.size() != 1) return nullptr;
        if (const auto* function = parents[0].get<clang::FunctionDecl>()) return function;
        node = parents[0];
    }
    return nullptr;
}
clang::QualType expectedType(clang::ASTContext& context, const clang::Expr* expression, const clang::FunctionDecl* function) {
    clang::DynTypedNode node = clang::DynTypedNode::create(*expression);
    for (unsigned depth = 0; depth < 32; ++depth) {
        const auto parents = context.getParents(node); if (parents.size() != 1) return {};
        if (const auto* variable = parents[0].get<clang::VarDecl>()) return variable->getType();
        if (parents[0].get<clang::ReturnStmt>()) return function ? function->getReturnType() : clang::QualType{};
        if (parents[0].get<clang::CompoundStmt>()) return context.VoidTy;
        if (!parents[0].get<clang::ParenExpr>() && !parents[0].get<clang::ImplicitCastExpr>() && !parents[0].get<clang::ExprWithCleanups>()) return {};
        node = parents[0];
    }
    return {};
}
bool userTokens(llvm::StringRef source, bool type, const clang::LangOptions& language) {
    if (source.size() > (type ? 4096U : 65536U) || source.contains('\0')) return false;
    auto buffer = llvm::MemoryBuffer::getMemBufferCopy(source);
    clang::Lexer lexer(clang::SourceLocation{}, language, buffer->getBufferStart(), buffer->getBufferStart(), buffer->getBufferEnd());
    unsigned braces = 0;
    clang::Token token;
    for (;;) {
        const bool end = lexer.LexFromRawLexer(token);
        if (token.is(clang::tok::eof)) break;
        if (token.is(clang::tok::hash) || token.is(clang::tok::hashhash) || token.is(clang::tok::unknown)) return false;
        if (token.is(clang::tok::l_brace)) { if (type) return false; ++braces; }
        if (token.is(clang::tok::r_brace)) { if (!braces || type) return false; --braces; }
        if (type && token.is(clang::tok::semi)) return false;
        if (end) break;
    }
    return braces == 0;
}
}
std::optional<CreatedUsage> createFromUsage(clang::ASTUnit& ast, const llvm::json::Object& request, std::string& error) {
    auto refuse = [&](const char* why) -> std::optional<CreatedUsage> { error = why; return {}; };
    auto& context = ast.getASTContext(); auto& sm = context.getSourceManager();
    const auto root = semantic::absolute(request.getString("projectRoot").value_or(""), "");
    const auto path = semantic::absolute(request.getString("path").value_or(""), root);
    const auto offset = static_cast<unsigned>(request.getInteger("offset").value_or(-1));
    Calls selected(context, path, root, offset); selected.TraverseDecl(context.getTranslationUnitDecl());
    if (!selected.call) return refuse("Select one unresolved non-template free or object-member call with a recovered typed AST");
    const auto* function = owner(context, selected.call);
    if (!function || !function->doesThisDeclarationHaveABody() || function->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate || function->isConstexpr())
        return refuse("Usage must belong to an ordinary non-template runtime function");
    auto location = sm.getLocForStartOfFile(sm.getFileID(selected.call->getBeginLoc())).getLocWithOffset(offset);
    location = clang::Lexer::GetBeginningOfToken(location, sm, context.getLangOpts());
    clang::Token token;
    if (clang::Lexer::getRawToken(location, token, sm, context.getLangOpts(), true) || !token.is(clang::tok::raw_identifier))
        return refuse("Select the unresolved identifier token, outside macros");
    const auto name = clang::Lexer::getSpelling(token, sm, context.getLangOpts());
    if (selected.lookup && selected.lookup->getName().getAsString() != name) return refuse("Selection is not the unresolved call identifier");
    std::string returnType(request.getString("usageReturnType").value_or(""));
    if (returnType.empty()) {
        auto type = expectedType(context, selected.call, function);
        if (type.isNull() || type->isDependentType() || type->isUndeducedType()) return refuse("Return type is unknown here; supply an explicit C++ return type");
        returnType = type.getCanonicalType().getAsString(context.getPrintingPolicy());
    }
    const auto suppliedBody = request.getString("usageBody");
    if (!suppliedBody || !userTokens(returnType, true, context.getLangOpts()) || !userTokens(*suppliedBody, false, context.getLangOpts()))
        return refuse("Supply a return type and balanced function-body statements without preprocessor or scope escapes");
    if (suppliedBody->trim().empty() && returnType != "void") return refuse("A non-void function requires an explicit useful implementation body");
    std::vector<std::string> parameters, names;
    std::set<std::string> used;
    for (auto* argument : selected.arguments) {
        auto type = argument->getType();
        if (argument->getBeginLoc().isMacroID() || argument->getEndLoc().isMacroID() || argument->containsErrors() || type.isNull() || type->isDependentType() || type->isUndeducedType() || type->isVoidType() || type->isFunctionType())
            return refuse("An argument has no independently known parameter type");
        if (type->isArrayType()) type = context.getArrayDecayedType(type);
        type = type.getNonReferenceType().getUnqualifiedType();
        if (type->isRecordType()) type = context.getLValueReferenceType(context.getConstType(type));
        std::string parameter = "arg" + std::to_string(names.size()+1);
        if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(argument->IgnoreParenImpCasts())) {
            const auto actual = reference->getDecl()->getNameAsString();
            if (!actual.empty()) parameter = actual;
        }
        if (!used.insert(parameter).second) { parameter = "arg" + std::to_string(names.size()+1); if (!used.insert(parameter).second) return refuse("Parameter names from this usage are ambiguous"); }
        std::string declaration; llvm::raw_string_ostream out(declaration);
        type.getCanonicalType().print(out, context.getPrintingPolicy(), parameter);
        parameters.push_back(out.str()); names.push_back(parameter);
    }
    std::string target, prefix, suffix;
    clang::SourceLocation insertion;
    if (selected.base) {
        auto type = selected.base->getType(); if (type->isPointerType()) type = type->getPointeeType();
        const auto* record = type->getAsCXXRecordDecl(); record = record ? record->getDefinition() : nullptr;
        if (!record || record->getDescribedClassTemplate() || record->isLocalClass() || record->isUnion() || record->getNumBases() || record->isLambda())
            return refuse("Member creation requires a complete ordinary non-template class without bases");
        for (const auto* declaration : record->decls()) if (const auto* named = llvm::dyn_cast<clang::NamedDecl>(declaration); named && named->getNameAsString() == name)
            return refuse("The record already contains this name; overload and ambiguous lookup creation refuse");
        insertion = record->getBraceRange().getEnd();
        if (type.isVolatileQualified()) return refuse("Volatile object member creation requires an explicit signature profile");
        suffix = type.isConstQualified() ? " const" : "";
        prefix = "\npublic:\n";
    } else {
        if (!function->getDeclContext()->isFileContext() || function->getLexicalDeclContext() != function->getDeclContext())
            return refuse("Free-call creation requires a namespace-scope ordinary function body");
        insertion = function->getBeginLoc(); prefix = "\n";
    }
    if (!insertion.isFileID()) return refuse("Generated declaration location is macro-owned");
    target = semantic::absolute(sm.getFilename(insertion), root);
    if (target != root && !(llvm::StringRef(target).starts_with(root) && target.size() > root.size() && llvm::sys::path::is_separator(target[root.size()])))
        return refuse("Generated member must belong to a captured project source/header");
    const auto bytes = sm.getBufferData(sm.getFileID(insertion));
    const auto at = sm.getFileOffset(insertion);
    std::string generated = prefix + "/// @brief User-supplied implementation of " + name + ".\n";
    for (const auto& parameter : names) generated += "/// @param " + parameter + " Argument from the selected usage.\n";
    if (!selected.base) generated += "static ";
    generated += returnType + " " + name + "(";
    for (std::size_t i = 0; i < parameters.size(); ++i) { if (i) generated += ", "; generated += parameters[i]; }
    generated += ")" + suffix + " {\n" + suppliedBody->str() + "\n}\n\n";
    std::string after = bytes.str(); after.insert(at, generated);
    return CreatedUsage{std::move(target), std::move(after)};
}
}
