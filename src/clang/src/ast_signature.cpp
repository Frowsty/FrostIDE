// SPDX-License-Identifier: Apache-2.0
// REF-6 explicit parameter reorder/name/type/add/remove with captured project call-site coverage.
#include "ast_signature.hpp"

#include "clang/AST/ParentMapContext.h"
#include "clang/Tooling/Core/Replacement.h"
#include "semantic_snapshot.hpp"

#include <cctype>
#include <iterator>
namespace frost::clang_engine {
namespace {
struct Parameter {
    std::optional<unsigned> old;
    std::string type, name, argument;
};
struct Binding {
    std::string path, usr;
    unsigned offset = 0;
    bool target = false, intentionalBody = false;
};
bool identifier(llvm::StringRef name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
        return false;
    return std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}
bool fragment(llvm::StringRef value) {
    if (value.size() > 32768 || value.contains('\0') || value.find_first_of(";#\r\n", 0) != llvm::StringRef::npos)
        return false;
    std::vector<char> stack;
    char quote = 0;
    bool escaped = false;
    for (char c : value) {
        if (quote) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '(' || c == '[' || c == '{')
            stack.push_back(c);
        if (c == ')' || c == ']' || c == '}') {
            if (stack.empty() || (c == ')' && stack.back() != '(') || (c == ']' && stack.back() != '[') ||
                (c == '}' && stack.back() != '{'))
                return false;
            stack.pop_back();
        }
    }
    return !quote && stack.empty();
}
std::vector<std::string> usrs(const clang::NamedDecl* decl, clang::ASTContext& context) {
    return clang::tooling::getUSRsForDeclaration(decl, context);
}
bool same(const clang::NamedDecl* decl, clang::ASTContext& context, const std::set<std::string>& family) {
    const auto ids = usrs(decl, context);
    return std::ranges::any_of(ids, [&](const auto& id) { return family.contains(id); });
}
class Pick final : public clang::RecursiveASTVisitor<Pick> {
public:
    clang::ASTContext& context;
    std::string anchor, root;
    unsigned offset;
    clang::FunctionDecl* function = nullptr;
    bool ambiguous = false;
    Pick(clang::ASTContext& context, std::string anchor, std::string root, unsigned offset)
        : context(context), anchor(std::move(anchor)), root(std::move(root)), offset(offset) {}
    void consider(clang::FunctionDecl* candidate, clang::SourceLocation location) {
        auto& sm = context.getSourceManager();
        if (!location.isFileID() || semantic::absolute(sm.getFilename(location), root) != anchor)
            return;
        const auto start = sm.getFileOffset(location);
        if (offset < start || offset - start >= clang::Lexer::MeasureTokenLength(location, sm, context.getLangOpts()))
            return;
        ambiguous |= function && function->getCanonicalDecl() != candidate->getCanonicalDecl();
        function = candidate;
    }
    bool VisitFunctionDecl(clang::FunctionDecl* decl) {
        consider(decl, decl->getLocation());
        return true;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
        if (auto* fn = llvm::dyn_cast<clang::FunctionDecl>(expr->getDecl()))
            consider(fn, expr->getLocation());
        return true;
    }
    bool VisitMemberExpr(clang::MemberExpr* expr) {
        if (auto* fn = llvm::dyn_cast<clang::FunctionDecl>(expr->getMemberDecl()))
            consider(fn, expr->getMemberLoc());
        return true;
    }
};
bool ordinary(const clang::FunctionDecl* fn) {
    if (!fn || !fn->getIdentifier() || fn->isVariadic() ||
        fn->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate || fn->getDeclContext()->isDependentContext())
        return false;
    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
        method && (method->isVirtual() || method->getParent()->getNumBases()))
        return false;
    return true;
}
} // namespace
struct SignaturePlanner::State {
    const llvm::json::Object& request;
    std::string root, anchor, qualified, name, inputError;
    unsigned offset;
    bool explicitEdit = false;
    std::vector<Parameter> parameters;
    std::set<std::string> family;
    std::map<std::string, clang::tooling::Replacements> edits;
    std::vector<std::vector<Binding>> bindings;
    std::size_t editCount = 0, replacementBytes = 0, bindingCount = 0;
    State(const llvm::json::Object& request, std::string root, std::string anchor, unsigned offset)
        : request(request), root(std::move(root)), anchor(std::move(anchor)), offset(offset) {
        explicitEdit = request.getBoolean("editParameters").value_or(false);
        if (explicitEdit) {
            const auto* values = request.getArray("parameters");
            if (!values || values->size() > 256) {
                inputError = "Explicit signature requires at most 256 parameter rows";
                return;
            }
            std::set<unsigned> used;
            for (const auto& value : *values) {
                const auto* object = value.getAsObject();
                if (!object) {
                    inputError = "Invalid parameter row";
                    return;
                }
                Parameter parameter;
                if (const auto* old = object->get("oldIndex"); old && !old->getAsNull()) {
                    const auto index = old->getAsInteger();
                    if (!index || *index < 0 || *index > 255 || !used.insert(static_cast<unsigned>(*index)).second) {
                        inputError = "Existing parameter indices must be unique";
                        return;
                    }
                    parameter.old = static_cast<unsigned>(*index);
                }
                parameter.type = object->getString("type").value_or("").str();
                parameter.name = object->getString("name").value_or("").str();
                parameter.argument = object->getString("argument").value_or("").str();
                if ((!parameter.name.empty() && !identifier(parameter.name)) || !fragment(parameter.type) ||
                    !fragment(parameter.argument) ||
                    (!parameter.old &&
                     (parameter.type.empty() || parameter.name.empty() || parameter.argument.empty()))) {
                    inputError = "New parameters need explicit type, name and balanced call expression; names must be "
                                 "identifiers";
                    return;
                }
                parameters.push_back(std::move(parameter));
            }
        } else {
            const auto* order = request.getArray("parameterOrder");
            if (!order || order->empty() || order->size() > 256) {
                inputError = "Supply a complete existing-parameter permutation";
                return;
            }
            std::set<unsigned> used;
            for (const auto& value : *order) {
                const auto index = value.getAsInteger();
                if (!index || *index < 0 || *index > 255 || !used.insert(static_cast<unsigned>(*index)).second) {
                    inputError = "Parameter order is not a permutation";
                    return;
                }
                parameters.push_back({static_cast<unsigned>(*index), {}, {}, {}});
            }
        }
    }
    bool inside(llvm::StringRef path) const {
        return path.starts_with(root) && (path == root || llvm::sys::path::is_separator(root.back()) ||
                                          llvm::sys::path::is_separator(path[root.size()]));
    }
    bool add(clang::ASTContext& context, clang::SourceLocation begin, clang::SourceLocation end, std::string text,
             std::string& error) {
        auto& sm = context.getSourceManager();
        if (!begin.isFileID() || !end.isFileID() || sm.getFileID(begin) != sm.getFileID(end)) {
            error = "A signature occurrence is generated by a macro";
            return false;
        }
        const auto path = semantic::absolute(sm.getFilename(begin), root);
        const auto first = sm.getFileOffset(begin), last = sm.getFileOffset(end);
        if (!inside(path) || last < first || edits.size() > 32768 || ++editCount > 100000 ||
            !semantic::addWithin(replacementBytes, text.size(), 16U * 1024U * 1024U)) {
            error = "Signature source range or replacement budget is unsupported";
            return false;
        }
        if (auto conflict = edits[path].add(clang::tooling::Replacement(path, first, last - first, text))) {
            error = llvm::toString(std::move(conflict));
            return false;
        }
        return true;
    }
    unsigned mapped(const std::string& path, unsigned original) const {
        std::int64_t delta = 0;
        if (const auto found = edits.find(path); found != edits.end())
            for (const auto& edit : found->second) {
                if (edit.getOffset() <= original && original < edit.getOffset() + edit.getLength())
                    return static_cast<unsigned>(edit.getOffset() + delta);
                if (edit.getOffset() + edit.getLength() <= original)
                    delta += static_cast<std::int64_t>(edit.getReplacementText().size()) - edit.getLength();
            }
        return static_cast<unsigned>(static_cast<std::int64_t>(original) + delta);
    }
};
SignaturePlanner::SignaturePlanner(const llvm::json::Object& request, std::string root, std::string anchor,
                                   unsigned offset)
    : state(std::make_unique<State>(request, std::move(root), std::move(anchor), offset)) {}
SignaturePlanner::~SignaturePlanner() = default;
bool SignaturePlanner::found() const { return !state->family.empty(); }
bool SignaturePlanner::discover(clang::ASTUnit& ast, std::string& error) {
    if (!state->inputError.empty()) {
        error = state->inputError;
        return false;
    }
    Pick pick(ast.getASTContext(), state->anchor, state->root, state->offset);
    pick.TraverseDecl(ast.getASTContext().getTranslationUnitDecl());
    if (!pick.function && !pick.ambiguous)
        return true;
    if (pick.ambiguous || !ordinary(pick.function)) {
        error = "Select one ordinary non-overloaded function or nonvirtual method (no dependent/base overload context)";
        return false;
    }
    const auto ids = usrs(pick.function, ast.getASTContext());
    if (ids.empty()) {
        error = "Function has no stable canonical identity";
        return false;
    }
    if (!state->family.empty() && !same(pick.function, ast.getASTContext(), state->family)) {
        error = "Selected function differs across compilation contexts";
        return false;
    }
    state->family.insert(ids.begin(), ids.end());
    state->qualified = pick.function->getQualifiedNameAsString();
    state->name = pick.function->getNameAsString();
    if (!state->explicitEdit && state->parameters.size() != pick.function->getNumParams()) {
        error = "Reorder requires a complete parameter permutation";
        return false;
    }
    for (const auto& parameter : state->parameters)
        if (parameter.old && *parameter.old >= pick.function->getNumParams()) {
            error = "Existing parameter index is outside the selected function";
            return false;
        }
    return true;
}
namespace {
clang::SourceLocation tokenEnd(clang::SourceLocation location, clang::ASTContext& context) {
    return clang::Lexer::getLocForEndOfToken(location, 0, context.getSourceManager(), context.getLangOpts());
}
std::string spelling(clang::SourceRange range, clang::ASTContext& context) {
    if (!range.getBegin().isFileID() || !range.getEnd().isFileID())
        return {};
    return clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(range), context.getSourceManager(),
                                       context.getLangOpts())
        .str();
}
const clang::FunctionDecl* containing(const clang::Stmt* statement, clang::ASTContext& context) {
    auto node = clang::DynTypedNode::create(*statement);
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
class NoCalls final : public clang::RecursiveASTVisitor<NoCalls> {
public:
    bool calls = false;
    bool VisitCallExpr(clang::CallExpr*) {
        calls = true;
        return true;
    }
};
class Collector final : public clang::RecursiveASTVisitor<Collector> {
public:
    clang::Preprocessor& preprocessor;
    SignaturePlanner::State& state;
    clang::ASTContext& context;
    std::string& error;
    std::vector<Binding> bindings;
    struct Pending {
        clang::ParmVarDecl* parameter;
        clang::SourceLocation location;
    };
    struct Covered {
        std::string path;
        unsigned begin, end;
    };
    std::vector<Pending> pending;
    std::vector<Covered> covered;
    std::vector<const clang::FunctionDecl*> definitions;
    Collector(SignaturePlanner::State& state, clang::ASTUnit& ast, std::string& error)
        : preprocessor(ast.getPreprocessor()), state(state), context(ast.getASTContext()), error(error) {}
    std::optional<std::string> renamed(const clang::ParmVarDecl* parameter) const {
        const auto* function = llvm::dyn_cast<clang::FunctionDecl>(parameter->getDeclContext());
        if (!function || !same(function, context, state.family))
            return std::nullopt;
        for (const auto& item : state.parameters)
            if (item.old == parameter->getFunctionScopeIndex())
                return item.name.empty() ? parameter->getNameAsString() : item.name;
        return std::string{}; // Removed: references must refuse, not invent a replacement expression.
    }
    std::set<std::string> changedNames(const clang::FunctionDecl* function) const {
        std::set<std::string> names;
        for (const auto& item : state.parameters)
            if (!item.name.empty() && (!item.old || function->getParamDecl(*item.old)->getNameAsString() != item.name))
                names.insert(item.name);
        return names;
    }
    bool fail(std::string reason) {
        if (error.empty())
            error = std::move(reason);
        return false;
    }
    bool nameIsMacro(llvm::StringRef name, clang::SourceLocation location) {
        auto& identifier = preprocessor.getIdentifierTable().get(name);
        return preprocessor.getMacroDefinitionAtLoc(&identifier, location).getMacroInfo() != nullptr;
    }
    bool VisitFunctionDecl(clang::FunctionDecl* function) {
        if (!same(function, context, state.family))
            return true;
        if (!ordinary(function))
            return fail("Virtual/dependent/variadic signature is outside this project profile");
        if (function->doesThisDeclarationHaveABody())
            definitions.push_back(function);
        for (const auto* declaration : function->getDeclContext()->decls()) {
            if (const auto* other = llvm::dyn_cast<clang::FunctionDecl>(declaration);
                other && other->getNameAsString() == state.name && !same(other, context, state.family))
                return fail("Overloaded declaration families require an explicit overload choice");
            if (const auto* other = llvm::dyn_cast<clang::FunctionTemplateDecl>(declaration);
                other && other->getNameAsString() == state.name)
                return fail("A same-name template overload is not admitted");
        }
        for (const auto* parameter : function->parameters())
            if (parameter->hasDefaultArg() || parameter->hasAttrs() || parameter->getBeginLoc().isMacroID() ||
                parameter->getEndLoc().isMacroID())
                return fail("Defaulted, attributed or macro parameter declarations need explicit context handling");
        const auto* info = function->getTypeSourceInfo();
        const auto location =
            info ? info->getTypeLoc().getAs<clang::FunctionProtoTypeLoc>() : clang::FunctionProtoTypeLoc{};
        if (location.isNull() || !location.getLParenLoc().isFileID() || !location.getRParenLoc().isFileID())
            return fail("Function parameter delimiters are not editable file tokens");
        std::string parameters;
        std::set<std::string> names;
        for (const auto& item : state.parameters) {
            if (item.old && *item.old >= function->getNumParams())
                return fail("Redeclaration has a different parameter shape");
            const auto* old = item.old ? function->getParamDecl(*item.old) : nullptr;
            const auto name = item.name.empty() && old ? old->getNameAsString() : item.name;
            if (!name.empty() && !names.insert(name).second)
                return fail("New parameter names are not unique");
            std::string text;
            if (item.type.empty() && item.name.empty() && old)
                text = spelling(old->getSourceRange(), context);
            else if (item.type.empty() && old) {
                llvm::raw_string_ostream out(text);
                old->getType().getCanonicalType().print(out, context.getPrintingPolicy(), name);
            } else
                text = item.type + (name.empty() ? "" : " " + name);
            if (text.empty())
                return fail("Parameter declaration has no editable spelling");
            for (std::size_t position = 0; position < text.size();) {
                if (!(std::isalpha(static_cast<unsigned char>(text[position])) || text[position] == '_')) {
                    ++position;
                    continue;
                }
                const auto first = position++;
                while (position < text.size() &&
                       (std::isalnum(static_cast<unsigned char>(text[position])) || text[position] == '_'))
                    ++position;
                auto& identifier = preprocessor.getIdentifierTable().get(llvm::StringRef(text).slice(first, position));
                if (preprocessor.getMacroDefinitionAtLoc(&identifier, location.getLParenLoc()).getMacroInfo())
                    return fail("An emitted parameter identifier is a macro at its declaration");
            }
            if (!parameters.empty())
                parameters += ", ";
            parameters += text;
        }
        return state.add(context, tokenEnd(location.getLParenLoc(), context), location.getRParenLoc(), parameters,
                         error);
    }
    bool VisitNamedDecl(clang::NamedDecl* declaration) {
        if (llvm::isa<clang::ParmVarDecl>(declaration))
            return true;
        for (const auto* scope = declaration->getDeclContext(); scope; scope = scope->getParent())
            if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(scope);
                function && same(function, context, state.family))
                if (changedNames(function).contains(declaration->getNameAsString()))
                    return fail("A new parameter name collides with a local declaration");
        return true;
    }
    bool VisitTypeLoc(clang::TypeLoc location) {
        auto& sm = context.getSourceManager();
        const auto begin = location.getBeginLoc();
        if (!begin.isValid())
            return true;
        const auto expanded = sm.getExpansionLoc(begin);
        for (const auto* function : definitions) {
            const auto names = changedNames(function);
            if (names.empty())
                continue;
            const auto* body = function->getBody();
            if (!sm.isPointWithin(expanded, body->getBeginLoc(), body->getEndLoc()))
                continue;
            // TypeLoc records real type syntax, including aliases in sizeof/typeid. Renaming a
            // parameter must not turn such an unqualified type lookup into a value expression.
            if (!begin.isFileID())
                return fail("Parameter editing with macro type syntax requires explicit expansion handling");
            const auto token = spelling({begin, begin}, context);
            if (names.contains(token))
                return fail("A new parameter would capture an existing type-name lookup");
        }
        return true;
    }
    bool directReference(clang::Expr* expression) {
        const clang::Stmt* node = expression;
        for (unsigned depth = 0; depth != 8; ++depth) {
            const auto parents = context.getParents(*node);
            if (parents.size() != 1)
                return false;
            if (const auto* call = parents[0].get<clang::CallExpr>())
                return call->getCallee()->IgnoreParenImpCasts() == expression->IgnoreParenImpCasts();
            if (const auto* parent = parents[0].get<clang::ImplicitCastExpr>())
                node = parent;
            else if (const auto* parent = parents[0].get<clang::ParenExpr>())
                node = parent;
            else
                return false;
        }
        return false;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* expression) {
        if (auto* function = llvm::dyn_cast<clang::FunctionDecl>(expression->getDecl());
            function && same(function, context, state.family))
            if (!directReference(expression))
                return fail("Function address/reference escapes the captured direct-call profile");
        if (auto* parameter = llvm::dyn_cast<clang::ParmVarDecl>(expression->getDecl())) {
            if (auto name = renamed(parameter)) {
                if (name->empty())
                    return fail("A removed parameter is still referenced in its definition");
                if (*name != parameter->getNameAsString())
                    pending.push_back({parameter, expression->getLocation()});
                return true;
            }
        }
        const auto* function = containing(expression, context);
        if (function && same(function, context, state.family) && !expression->hasQualifier() &&
            changedNames(function).contains(expression->getDecl()->getNameAsString()))
            return fail("A new parameter would capture an existing unqualified reference");
        return true;
    }
    bool VisitMemberExpr(clang::MemberExpr* expression) {
        if (auto* function = llvm::dyn_cast<clang::FunctionDecl>(expression->getMemberDecl());
            function && same(function, context, state.family))
            if (!directReference(expression))
                return fail("Member function address/reference escapes the direct-call profile");
        const auto* function = containing(expression, context);
        if (function && same(function, context, state.family) && expression->isImplicitAccess() &&
            changedNames(function).contains(expression->getMemberDecl()->getNameAsString()))
            return fail("A new parameter would capture an implicit member access");
        return true;
    }
    bool VisitOverloadExpr(clang::OverloadExpr* expression) {
        for (const auto* declaration : expression->decls())
            if (same(declaration, context, state.family))
                return fail("An unresolved/dependent overload lookup references the selected function");
        return true;
    }
    bool VisitLambdaExpr(clang::LambdaExpr* expression) {
        const auto* function = containing(expression, context);
        if (function && same(function, context, state.family) && !changedNames(function).empty())
            return fail("Parameter renaming with lambda captures requires explicit capture edits");
        return true;
    }
    bool VisitCallExpr(clang::CallExpr* call) {
        const auto* function = call->getDirectCallee();
        if (!function)
            return true;
        const bool target = same(function, context, state.family);
        auto& sm = context.getSourceManager();
        const auto location = sm.getExpansionLoc(call->getCallee()->getBeginLoc());
        const auto path = semantic::absolute(sm.getFilename(location), state.root);
        if (state.inside(path) && function->getNameAsString() == state.name) {
            auto ids = usrs(function, context);
            if (ids.empty() || ++state.bindingCount > 100000)
                return fail("Call binding identity/budget cannot be established");
            const auto* body = containing(call, context);
            const bool intent = state.explicitEdit && body && same(body, context, state.family);
            bindings.push_back({path, ids.front(), sm.getFileOffset(location), target, intent});
        }
        if (!target)
            return true;
        if (!call->getCallee()->getBeginLoc().isFileID() || !call->getRParenLoc().isFileID())
            return fail("A direct call is generated by a macro");
        if (call->getNumArgs() != function->getNumParams())
            return fail("Default/variadic call arguments are not admitted");
        std::vector<std::string> originals;
        for (auto* argument : call->arguments()) {
            NoCalls calls;
            calls.TraverseStmt(argument);
            if (argument->HasSideEffects(context) || argument->getType().isVolatileQualified() || calls.calls ||
                argument->getBeginLoc().isMacroID() || argument->getEndLoc().isMacroID())
                return fail("Existing call arguments must be editable, non-volatile and free of calls/side effects");
            auto text = spelling(argument->getSourceRange(), context);
            if (text.empty())
                return fail("Call argument has no editable spelling");
            // Pending parameter renames inside a recursive call are folded into that argument
            // before its complete list is replaced, avoiding overlapping independent edits.
            struct References final : clang::RecursiveASTVisitor<References> {
                std::vector<clang::DeclRefExpr*> values;
                bool VisitDeclRefExpr(clang::DeclRefExpr* expression) {
                    values.push_back(expression);
                    return true;
                }
            } refs;
            refs.TraverseStmt(argument);
            std::vector<std::pair<unsigned, std::pair<unsigned, std::string>>> replacements;
            const auto start = sm.getFileOffset(argument->getBeginLoc());
            for (auto* reference : refs.values)
                if (const auto* parameter = llvm::dyn_cast<clang::ParmVarDecl>(reference->getDecl()))
                    if (auto name = renamed(parameter); name && *name != parameter->getNameAsString()) {
                        if (name->empty() || reference->getLocation().isMacroID())
                            return fail("Removed or macro parameter argument reference");
                        if (nameIsMacro(*name, reference->getLocation()))
                            return fail("A renamed parameter is a macro at its reference");
                        replacements.push_back(
                            {sm.getFileOffset(reference->getLocation()) - start,
                             {clang::Lexer::MeasureTokenLength(reference->getLocation(), sm, context.getLangOpts()),
                              *name}});
                    }
            std::ranges::sort(replacements,
                              [](const auto& left, const auto& right) { return left.first > right.first; });
            for (const auto& [offset, replacement] : replacements)
                text.replace(offset, replacement.first, replacement.second);
            originals.push_back(std::move(text));
        }
        std::string arguments;
        for (const auto& parameter : state.parameters) {
            if (parameter.old && *parameter.old >= originals.size())
                return fail("Inconsistent parameter count across declarations");
            if (!arguments.empty())
                arguments += ", ";
            arguments += "(" + (parameter.old ? originals[*parameter.old] : parameter.argument) + ")";
        }
        const auto first = call->getNumArgs() ? call->getArg(0)->getBeginLoc() : call->getRParenLoc();
        const auto last = call->getNumArgs() ? tokenEnd(call->getArg(call->getNumArgs() - 1)->getEndLoc(), context)
                                             : call->getRParenLoc();
        if (!first.isFileID() || !last.isFileID() || sm.getFileID(first) != sm.getFileID(last))
            return fail("Call arguments are not one editable file range");
        covered.push_back(
            {semantic::absolute(sm.getFilename(first), state.root), sm.getFileOffset(first), sm.getFileOffset(last)});
        return state.add(context, first, last, arguments, error);
    }
    bool finish() {
        auto& sm = context.getSourceManager();
        for (const auto& reference : pending) {
            if (!reference.location.isFileID())
                return fail("Parameter reference comes from a macro");
            const auto path = semantic::absolute(sm.getFilename(reference.location), state.root);
            const auto offset = sm.getFileOffset(reference.location);
            if (std::ranges::any_of(covered, [&](const auto& range) {
                    return range.path == path && offset >= range.begin && offset < range.end;
                }))
                continue;
            const auto name = renamed(reference.parameter);
            if (name && nameIsMacro(*name, reference.location))
                return fail("A renamed parameter is a macro at its reference");
            if (!name || !state.add(context, reference.location, tokenEnd(reference.location, context), *name, error))
                return false;
        }
        return error.empty();
    }
};
} // namespace
bool SignaturePlanner::collect(clang::ASTUnit& ast, std::string& error) {
    Collector visitor(*state, ast, error);
    if (!visitor.TraverseDecl(ast.getASTContext().getTranslationUnitDecl()) || !visitor.finish())
        return false;
    state->bindings.push_back(std::move(visitor.bindings));
    return true;
}
std::optional<std::map<std::string, std::string>>
SignaturePlanner::candidates(const std::map<std::string, std::shared_ptr<const std::string>>& inputs,
                             std::string& error) const {
    std::map<std::string, std::string> result;
    for (const auto& [path, edits] : state->edits) {
        const auto before = inputs.find(path);
        if (before == inputs.end()) {
            error = "Edited signature file lacks a captured before-image";
            return {};
        }
        auto after = clang::tooling::applyAllReplacements(*before->second, edits);
        if (!after) {
            error = llvm::toString(after.takeError());
            return {};
        }
        if (*after != *before->second)
            result.emplace(path, std::move(*after));
    }
    if (result.empty()) {
        error = "The requested signature makes no change";
        return {};
    }
    return result;
}
bool SignaturePlanner::validate(clang::ASTUnit& ast, std::size_t configuration, std::string& error) const {
    if (configuration >= state->bindings.size()) {
        error = "Missing original call binding configuration";
        return false;
    }
    struct After final : clang::RecursiveASTVisitor<After> {
        const State& state;
        clang::ASTContext& context;
        std::set<std::string> family;
        std::vector<Binding> calls;
        After(const State& state, clang::ASTContext& context) : state(state), context(context) {}
        bool VisitFunctionDecl(clang::FunctionDecl* function) {
            if (function->getQualifiedNameAsString() == state.qualified) {
                const auto ids = usrs(function, context);
                family.insert(ids.begin(), ids.end());
            }
            return true;
        }
        bool VisitCallExpr(clang::CallExpr* call) {
            const auto* function = call->getDirectCallee();
            if (!function || function->getNameAsString() != state.name)
                return true;
            auto& sm = context.getSourceManager();
            const auto location = sm.getExpansionLoc(call->getCallee()->getBeginLoc());
            if (!location.isFileID())
                return true;
            const auto path = semantic::absolute(sm.getFilename(location), state.root);
            if (!state.inside(path))
                return true;
            const auto ids = usrs(function, context);
            if (!ids.empty())
                calls.push_back({path, ids.front(), sm.getFileOffset(location)});
            return true;
        }
    } after(*state, ast.getASTContext());
    after.TraverseDecl(ast.getASTContext().getTranslationUnitDecl());
    std::map<std::pair<std::string, unsigned>, std::map<std::string, std::size_t>> calls;
    for (const auto& call : after.calls)
        ++calls[{call.path, call.offset}][call.usr];
    for (const auto& before : state->bindings[configuration]) {
        bool matched = false;
        const auto position = calls.find({before.path, state->mapped(before.path, before.offset)});
        if (position != calls.end()) {
            for (auto& [usr, count] : position->second) {
                if (count && ((before.target && after.family.contains(usr)) ||
                              (!before.target && (usr == before.usr || before.intentionalBody)))) {
                    --count;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            error = "A captured direct call changed its declaration binding or disappeared";
            return false;
        }
    }
    return true;
}
} // namespace frost::clang_engine
