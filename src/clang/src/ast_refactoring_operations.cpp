// SPDX-License-Identifier: Apache-2.0
// REF-6/7 bounded AST transformations. No textual symbol search and no source writes.
#include "ast_refactoring_operations.hpp"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Lex/Lexer.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace frost::clang_engine {
namespace {
struct Edit {
    unsigned begin, end;
    std::string text;
};
struct Source {
    clang::ASTContext& context;
    clang::SourceManager& sm;
    llvm::StringRef bytes;
    std::optional<std::pair<unsigned, unsigned>> range(clang::SourceRange r) const {
        if (!r.getBegin().isFileID() || !r.getEnd().isFileID() || !sm.isWrittenInMainFile(r.getBegin()) ||
            !sm.isWrittenInMainFile(r.getEnd()))
            return {};
        const auto end = clang::Lexer::getLocForEndOfToken(r.getEnd(), 0, sm, context.getLangOpts());
        if (end.isInvalid())
            return {};
        const unsigned beginOffset = sm.getFileOffset(r.getBegin()), endOffset = sm.getFileOffset(end);
        if (beginOffset > endOffset || endOffset > bytes.size())
            return {};
        return std::pair{beginOffset, endOffset};
    }
    std::string text(clang::SourceRange r) const {
        auto offsets = range(r);
        return offsets ? bytes.substr(offsets->first, offsets->second - offsets->first).str() : std::string{};
    }
    bool add(std::vector<Edit>& edits, clang::SourceRange r, std::string text) const {
        auto offsets = range(r);
        if (!offsets)
            return false;
        edits.push_back({offsets->first, offsets->second, std::move(text)});
        return true;
    }
};
bool arithmetic(clang::QualType type) {
    return !type.isNull() && type->isArithmeticType() && !type.isVolatileQualified();
}
std::string typeName(clang::QualType type, clang::ASTContext& context) {
    return type.getCanonicalType().getUnqualifiedType().getAsString(context.getPrintingPolicy());
}
class Facts final : public clang::RecursiveASTVisitor<Facts> {
public:
    explicit Facts(clang::ASTContext& context) : context(context) {}
    clang::ASTContext& context;
    bool safe = true;
    bool portable = true; // No transplanted type names, sizeof/lookups, calls or implicit object state.
    std::vector<clang::DeclRefExpr*> references;
    bool VisitStmt(clang::Stmt* stmt) {
        safe &= stmt->getBeginLoc().isFileID() && stmt->getEndLoc().isFileID() && !llvm::isa<clang::LambdaExpr>(stmt) &&
                !llvm::isa<clang::CXXThisExpr>(stmt);
        return safe;
    }
    bool VisitExpr(clang::Expr* expr) {
        portable &=
            llvm::isa<clang::IntegerLiteral, clang::FloatingLiteral, clang::CharacterLiteral, clang::CXXBoolLiteralExpr,
                      clang::UnaryOperator, clang::BinaryOperator, clang::ConditionalOperator, clang::ParenExpr,
                      clang::ImplicitCastExpr, clang::DeclRefExpr>(expr);
        safe &= !expr->isTypeDependent() && !expr->isValueDependent() && !expr->HasSideEffects(context) &&
                !expr->getType().isVolatileQualified();
        return safe;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
        references.push_back(expr);
        return true;
    }
};
class Selection final : public clang::RecursiveASTVisitor<Selection> {
public:
    Selection(Source& source, unsigned begin, unsigned end) : source(source), begin(begin), end(end) {}
    Source& source;
    unsigned begin, end;
    clang::Expr* expression = nullptr;
    clang::VarDecl* variable = nullptr;
    clang::FunctionDecl* function = nullptr;
    clang::CallExpr* call = nullptr;
    bool at(clang::SourceLocation location) {
        if (!location.isFileID() || !source.sm.isWrittenInMainFile(location))
            return false;
        const auto offset = source.sm.getFileOffset(location);
        return begin >= offset &&
               begin - offset < clang::Lexer::MeasureTokenLength(location, source.sm, source.context.getLangOpts());
    }
    bool VisitExpr(clang::Expr* expr) {
        const auto r = source.range(expr->getSourceRange());
        if (r && r->first == begin && r->second == end && !llvm::isa<clang::ImplicitCastExpr>(expr))
            expression = expr;
        return true;
    }
    bool VisitVarDecl(clang::VarDecl* decl) {
        if (at(decl->getLocation()))
            variable = decl;
        return true;
    }
    bool VisitFunctionDecl(clang::FunctionDecl* decl) {
        if (at(decl->getLocation()))
            function = decl;
        return true;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
        if (at(expr->getLocation())) {
            if (auto* var = llvm::dyn_cast<clang::VarDecl>(expr->getDecl()))
                variable = var;
            if (auto* fn = llvm::dyn_cast<clang::FunctionDecl>(expr->getDecl()))
                function = fn;
        }
        return true;
    }
    bool VisitCallExpr(clang::CallExpr* expr) {
        if (at(expr->getCallee()->getBeginLoc()))
            call = expr;
        return true;
    }
};
class Uses final : public clang::RecursiveASTVisitor<Uses> {
public:
    const clang::Decl* target;
    std::vector<clang::DeclRefExpr*> references;
    std::vector<clang::CallExpr*> calls;
    explicit Uses(const clang::ValueDecl* target) : target(target->getCanonicalDecl()) {}
    bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
        if (expr->getDecl()->getCanonicalDecl() == target)
            references.push_back(expr);
        return true;
    }
    bool VisitCallExpr(clang::CallExpr* expr) {
        if (expr->getDirectCallee() && expr->getDirectCallee()->getCanonicalDecl() == target)
            calls.push_back(expr);
        return true;
    }
};
template<class T>
const T* parent(const clang::Stmt* stmt, clang::ASTContext& context) {
    const auto nodes = context.getParents(*stmt);
    return nodes.size() == 1 ? nodes[0].get<T>() : nullptr;
}
const clang::FunctionDecl* enclosing(const clang::Stmt* stmt, clang::ASTContext& context) {
    auto node = clang::DynTypedNode::create(*stmt);
    for (unsigned depth = 0; depth != 128; ++depth) {
        const auto parents = context.getParents(node);
        if (parents.size() != 1)
            return nullptr;
        if (const auto* function = parents[0].get<clang::FunctionDecl>())
            return function;
        node = parents[0];
    }
    return nullptr;
}
bool ordinary(const clang::FunctionDecl* fn) {
    return fn && fn->doesThisDeclarationHaveABody() && !llvm::isa<clang::CXXMethodDecl>(fn) &&
           fn->getDeclContext()->isTranslationUnit() && !fn->getDescribedFunctionTemplate() && !fn->isVariadic() &&
           fn->getTemplatedKind() == clang::FunctionDecl::TK_NonTemplate &&
           fn->getFormalLinkage() == clang::Linkage::Internal;
}
bool identifier(llvm::StringRef name) {
    if (name.empty() || !((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
        return false;
    return std::ranges::all_of(name, [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}
class Names final : public clang::RecursiveASTVisitor<Names> {
public:
    std::set<std::string> values;
    bool VisitNamedDecl(clang::NamedDecl* decl) {
        if (!decl->getNameAsString().empty())
            values.insert(decl->getNameAsString());
        return true;
    }
};
} // namespace

std::optional<std::string> refactorAst(clang::ASTUnit& ast, const llvm::json::Object& request, std::string& error) {
    if (request.getString("operation") == "safe-delete")
        return refactorSafeDelete(ast, request, error);
    auto& context = ast.getASTContext();
    auto& sm = ast.getSourceManager();
    Source source{context, sm, sm.getBufferData(sm.getMainFileID())};
    const auto begin = request.getInteger("offset"), end = request.getInteger("endOffset");
    const auto operation = request.getString("operation"), name = request.getString("newName");
    auto refuse = [&](std::string why) -> std::optional<std::string> {
        error = std::move(why);
        return {};
    };
    if (!begin || !end || *begin < 0 || *end < *begin || static_cast<std::uint64_t>(*end) > source.bytes.size() ||
        !operation || !name)
        return refuse("Invalid byte selection or AST operation");
    Selection selected(source, static_cast<unsigned>(*begin), static_cast<unsigned>(*end));
    selected.TraverseDecl(context.getTranslationUnitDecl());
    std::vector<Edit> edits;
    if (*operation == "change-signature") {
        auto* fn = selected.function;
        if (!ordinary(fn) || std::distance(fn->redecls_begin(), fn->redecls_end()) != 1)
            return refuse("Change signature supports one defined, internal, non-template free function");
        for (const auto* declaration : context.getTranslationUnitDecl()->decls())
            if (const auto* other = llvm::dyn_cast<clang::FunctionDecl>(declaration);
                other && other->getNameAsString() == fn->getNameAsString() &&
                other->getCanonicalDecl() != fn->getCanonicalDecl())
                return refuse("Change signature refuses overload families");
        const auto* order = request.getArray("parameterOrder");
        if (!order || order->size() != fn->getNumParams() || order->empty())
            return refuse("Supply a complete parameter permutation");
        std::set<unsigned> seen;
        std::vector<unsigned> permutation;
        std::string parameters;
        for (const auto& item : *order) {
            const auto index = item.getAsInteger();
            if (!index || *index < 0 || static_cast<unsigned>(*index) >= fn->getNumParams() ||
                !seen.insert(static_cast<unsigned>(*index)).second)
                return refuse("Parameter order must be a permutation without duplicates");
            auto* parameter = fn->getParamDecl(static_cast<unsigned>(*index));
            if (!arithmetic(parameter->getType()) || parameter->hasDefaultArg())
                return refuse("Only arithmetic by-value parameters without defaults are supported");
            auto text = source.text(parameter->getSourceRange());
            if (text.empty())
                return refuse("Parameter is generated by a macro or another file");
            if (!parameters.empty())
                parameters += ", ";
            parameters += text;
            permutation.push_back(static_cast<unsigned>(*index));
        }
        Uses uses(fn);
        uses.TraverseDecl(context.getTranslationUnitDecl());
        if (uses.references.size() != uses.calls.size())
            return refuse("Function address/reference escapes the direct-call profile");
        if (!source.add(edits,
                        {fn->getParamDecl(0)->getBeginLoc(), fn->getParamDecl(fn->getNumParams() - 1)->getEndLoc()},
                        parameters))
            return refuse("Parameter range is not editable");
        for (auto* call : uses.calls) {
            if (call->getNumArgs() != fn->getNumParams())
                return refuse("Call has implicit/default/variadic arguments");
            std::string arguments;
            for (const auto index : permutation) {
                auto* arg = call->getArg(index);
                Facts facts(context);
                facts.TraverseStmt(arg);
                if (!facts.safe || !arithmetic(arg->getType()))
                    return refuse("Reordering requires side-effect-free, non-volatile arithmetic arguments");
                auto text = source.text(arg->getSourceRange());
                if (text.empty())
                    return refuse("Call argument is not an editable file range");
                if (!arguments.empty())
                    arguments += ", ";
                arguments += text;
            }
            if (!source.add(edits, {call->getArg(0)->getBeginLoc(), call->getArg(call->getNumArgs() - 1)->getEndLoc()},
                            arguments))
                return refuse("Call argument range is not editable");
        }
    } else if (*operation == "inline-variable") {
        auto* var = selected.variable;
        if (!var || !var->isLocalVarDecl() || var->isStaticLocal() || !var->getType().isConstQualified() ||
            !arithmetic(var->getType()) || !var->hasInit() || !var->getInit()->isCXX11ConstantExpr(context))
            return refuse("Inline variable supports automatic const arithmetic constant-expression initializers");
        const auto* statement =
            context.getParents(*var).size() == 1 ? context.getParents(*var)[0].get<clang::DeclStmt>() : nullptr;
        if (!statement || !statement->isSingleDecl())
            return refuse("Variable must have its own declaration statement");
        Facts initializer(context);
        initializer.TraverseStmt(var->getInit());
        const auto initializerText = source.text(var->getInit()->getSourceRange());
        if (!initializer.safe || !initializer.portable || !initializer.references.empty() || initializerText.empty())
            return refuse(
                "Inline constant initializer must be editable literal arithmetic without transplanted names or macros");
        Uses uses(var);
        uses.TraverseDecl(context.getTranslationUnitDecl());
        for (auto* reference : uses.references) {
            const auto* cast = parent<clang::ImplicitCastExpr>(reference, context);
            if (!cast || cast->getCastKind() != clang::CK_LValueToRValue)
                return refuse("Variable identity/address is observable");
            if (!source.add(edits, reference->getSourceRange(),
                            "static_cast<" + typeName(var->getType(), context) + ">(" + initializerText + ")"))
                return refuse("Variable reference is not an editable file token");
        }
        auto range = source.range(statement->getSourceRange());
        if (!range)
            return refuse("Declaration is not an editable range");
        edits.push_back({range->first, range->second, {}});
    } else if (*operation == "inline-function") {
        auto* call = selected.call;
        auto* fn = call ? call->getDirectCallee() : nullptr;
        if (!ordinary(fn) || !arithmetic(fn->getReturnType()) || call->getNumArgs() != fn->getNumParams())
            return refuse("Inline function supports direct calls to internal arithmetic free functions");
        const auto* body = llvm::dyn_cast<clang::CompoundStmt>(fn->getBody());
        const auto* returned =
            body && body->size() == 1 ? llvm::dyn_cast<clang::ReturnStmt>(*body->body_begin()) : nullptr;
        if (!returned || !returned->getRetValue())
            return refuse("Function body must be one return expression");
        for (unsigned index = 0; index != call->getNumArgs(); ++index) {
            const auto* parameter = fn->getParamDecl(index);
            auto* argument = call->getArg(index)->IgnoreParenImpCasts();
            Facts facts(context);
            facts.TraverseStmt(call->getArg(index));
            if (parameter->hasDefaultArg() || !arithmetic(parameter->getType()) || !facts.safe ||
                (!llvm::isa<clang::DeclRefExpr, clang::IntegerLiteral, clang::FloatingLiteral,
                            clang::CXXBoolLiteralExpr>(argument)) ||
                !arithmetic(argument->getType()) || source.text(call->getArg(index)->getSourceRange()).empty())
                return refuse("Every inline argument, including unused parameters, must be an editable arithmetic "
                              "value or literal without side effects");
        }
        Facts facts(context);
        facts.TraverseStmt(const_cast<clang::Expr*>(returned->getRetValue()));
        if (!facts.safe || !facts.portable)
            return refuse(
                "Function expression has side effects, macros, transplanted type names or implicit object state");
        std::vector<Edit> substitutions;
        auto bodyRange = source.range(returned->getRetValue()->getSourceRange());
        if (!bodyRange)
            return refuse("Function body is not in the captured main file");
        for (auto* reference : facts.references) {
            const auto* parameter = llvm::dyn_cast<clang::ParmVarDecl>(reference->getDecl());
            if (!parameter || parameter->getDeclContext() != fn || !arithmetic(parameter->getType()))
                return refuse("Inline expression may only reference its arithmetic parameters");
            const auto index = parameter->getFunctionScopeIndex();
            auto* argument = call->getArg(index)->IgnoreParenImpCasts();
            if ((!llvm::isa<clang::DeclRefExpr>(argument) && !llvm::isa<clang::IntegerLiteral>(argument) &&
                 !llvm::isa<clang::FloatingLiteral>(argument) && !llvm::isa<clang::CXXBoolLiteralExpr>(argument)) ||
                !arithmetic(argument->getType()))
                return refuse("Inline arguments must be non-volatile arithmetic values or literals");
            if (!source.add(substitutions, reference->getSourceRange(),
                            "static_cast<" + typeName(parameter->getType(), context) + ">(" +
                                source.text(call->getArg(index)->getSourceRange()) + ")"))
                return refuse("Parameter substitution range is not editable");
        }
        std::string expression = source.bytes.substr(bodyRange->first, bodyRange->second - bodyRange->first).str();
        std::ranges::sort(substitutions, [](const auto& a, const auto& b) { return a.begin > b.begin; });
        for (const auto& edit : substitutions)
            expression.replace(edit.begin - bodyRange->first, edit.end - edit.begin, edit.text);
        if (!source.add(edits, call->getSourceRange(),
                        "static_cast<" + typeName(fn->getReturnType(), context) + ">(" + expression + ")"))
            return refuse("Call is not an editable range");
    } else if (*operation == "extract-variable" || *operation == "extract-constant" || *operation == "extract-method" ||
               *operation == "extract-field") {
        auto* expression = selected.expression;
        if (!expression || !identifier(*name) || !arithmetic(expression->getType()) || !expression->isPRValue())
            return refuse("Select one complete arithmetic prvalue expression and supply an identifier");
        Facts facts(context);
        facts.TraverseStmt(expression);
        if (!facts.safe)
            return refuse("Extraction requires a macro-free, non-volatile expression without side effects");
        clang::Stmt* outer = expression;
        while (const auto* cast = parent<clang::ImplicitCastExpr>(outer, context))
            outer = const_cast<clang::ImplicitCastExpr*>(cast);
        const auto* returned = parent<clang::ReturnStmt>(outer, context);
        if (!returned || !parent<clang::CompoundStmt>(returned, context))
            return refuse("This profile extracts a complete return expression in a compound block");
        const auto* function = enclosing(expression, context);
        Names names;
        names.TraverseDecl(context.getTranslationUnitDecl());
        if (names.values.contains(name->str()))
            return refuse("The requested name already exists in the translation unit");
        const auto text = source.text(expression->getSourceRange());
        if (*operation == "extract-field") {
            const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(function);
            const auto* record = method ? method->getParent() : nullptr;
            if (!method || method->isStatic() || !record || record->isAggregate() || record->isUnion() ||
                record->isExternallyVisible() || !facts.portable || !facts.references.empty() ||
                !expression->isCXX11ConstantExpr(context))
                return refuse(
                    "Extract field requires a constant expression in an instance method of a non-aggregate class");
            const auto close = record->getBraceRange().getEnd();
            if (!close.isFileID() || !sm.isWrittenInMainFile(close))
                return refuse("Class is not editable in this file");
            edits.push_back({sm.getFileOffset(close), sm.getFileOffset(close),
                             "\nprivate:\n    const " + typeName(expression->getType(), context) + " " + name->str() +
                                 " = " + text + ";\n"});
            if (!source.add(edits, expression->getSourceRange(), "this->" + name->str()))
                return refuse("Expression is not editable");
        } else if (*operation == "extract-method") {
            if (!ordinary(function) || !facts.portable)
                return refuse("Extract method produces an internal helper from portable arithmetic beside an internal "
                              "free function");
            std::map<std::string, const clang::VarDecl*> captures;
            for (auto* reference : facts.references) {
                const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
                if (!variable || (!variable->isLocalVarDecl() && !llvm::isa<clang::ParmVarDecl>(variable)) ||
                    !arithmetic(variable->getType()))
                    return refuse("Helper captures must be arithmetic local values or parameters");
                captures.emplace(variable->getNameAsString(), variable);
            }
            std::string parameters, arguments;
            for (const auto& [capture, variable] : captures) {
                if (!parameters.empty()) {
                    parameters += ", ";
                    arguments += ", ";
                }
                parameters += typeName(variable->getType(), context) + " " + capture;
                arguments += capture;
            }
            const auto location = function->getBeginLoc();
            if (!location.isFileID() || !sm.isWrittenInMainFile(location))
                return refuse("Function is not editable");
            edits.push_back({sm.getFileOffset(location), sm.getFileOffset(location),
                             "static " + typeName(expression->getType(), context) + " " + name->str() + "(" +
                                 parameters + ") { return " + text + "; }\n"});
            if (!source.add(edits, expression->getSourceRange(), name->str() + "(" + arguments + ")"))
                return refuse("Expression is not editable");
        } else {
            if (*operation == "extract-constant" && !expression->isCXX11ConstantExpr(context))
                return refuse("Extract constant requires a constant expression");
            const auto location = returned->getBeginLoc();
            if (!location.isFileID() || !sm.isWrittenInMainFile(location))
                return refuse("Return statement is not editable");
            edits.push_back({sm.getFileOffset(location), sm.getFileOffset(location),
                             std::string(*operation == "extract-constant" ? "constexpr auto " : "const auto ") +
                                 name->str() + " = " + text + ";\n"});
            if (!source.add(edits, expression->getSourceRange(), name->str()))
                return refuse("Expression is not editable");
        }
    } else
        return refuse("Unknown or unsupported AST operation");
    std::ranges::sort(edits, [](const auto& a, const auto& b) { return a.begin > b.begin; });
    std::string candidate = source.bytes.str();
    unsigned boundary = static_cast<unsigned>(candidate.size());
    for (const auto& edit : edits) {
        if (edit.end > boundary || edit.begin > edit.end)
            return refuse("Semantic edits overlap");
        candidate.replace(edit.begin, edit.end - edit.begin, edit.text);
        boundary = edit.begin;
    }
    if (candidate == source.bytes)
        return refuse("The selected operation makes no change");
    return candidate;
}
} // namespace frost::clang_engine
