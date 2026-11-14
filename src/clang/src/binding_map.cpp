// SPDX-License-Identifier: Apache-2.0
// FEC-13: read-only semantic mapping of actual schema registration arguments, never textual rewrites.

#include "binding_map.hpp"

#include "binding_map_command.hpp"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace frost::clang_engine {
namespace {

struct Range {
    std::string path;
    unsigned offset = 0;
    unsigned length = 0;
};

struct Target {
    std::string role;
    std::string usr;
    std::string qualifiedName;
    std::string kind;
    Range declaration;
    std::optional<Range> definition;
    Range argument;
};

struct NameLiteral {
    Range range;
    std::string decodedValue;
    std::string spelling;
};
struct SignatureLiteral {
    NameLiteral literal;
    unsigned nameOffset = 0, nameLength = 0;
};

struct Candidate {
    Range registration;
    std::string status = "mapped";
    std::string reason;
    std::vector<Target> targets;
    std::optional<NameLiteral> nameLiteral;
    std::string nameLiteralReason;
    std::optional<SignatureLiteral> docSignature;
    std::string docSignatureReason;
};

struct Registration {
    std::string id;
    std::string qualifiedName;
    std::string name;
    std::string kind;
    std::string file;
    std::string physicalFile;
    unsigned line = 0;
    std::vector<Candidate> candidates;
};

struct Request {
    std::string root;
    std::string database;
    std::string source;
    std::optional<std::int64_t> commandIndex;
    std::vector<Registration> registrations;
    std::map<std::string, std::string> sourceInputs;
    bool sourceInputsComplete = true;
    BindingCompileContext compileContext;
    std::vector<NameLiteral> embeddedLua;
    std::vector<std::string> embeddedLuaRefusals;
};

std::optional<std::string> physicalPath(llvm::StringRef input) {
    if (!llvm::sys::path::is_absolute(input))
        return std::nullopt;
    llvm::SmallString<256> result;
    if (llvm::sys::fs::real_path(input, result))
        return std::nullopt;
    llvm::sys::path::native(result, llvm::sys::path::Style::posix);
    return result.str().str();
}

bool samePath(llvm::StringRef left, llvm::StringRef right) {
#ifdef _WIN32
    return left.equals_insensitive(right);
#else
    return left == right;
#endif
}

bool insideRoot(llvm::StringRef root, llvm::StringRef path) {
    return samePath(root, path) || (path.size() > root.size() && samePath(path.take_front(root.size()), root) &&
                                    (root.ends_with("/") || path[root.size()] == '/'));
}

llvm::json::Object rangeJson(const Range& range) {
    return {{"path", range.path}, {"offset", range.offset}, {"length", range.length}};
}

llvm::json::Object targetJson(const Target& target) {
    llvm::json::Object result{{"role", target.role},
                              {"usr", target.usr},
                              {"qualifiedName", target.qualifiedName},
                              {"kind", target.kind},
                              {"declaration", rangeJson(target.declaration)},
                              {"argumentRange", rangeJson(target.argument)}};
    if (target.definition)
        result["definition"] = rangeJson(*target.definition);
    return result;
}

llvm::json::Object nameLiteralJson(const NameLiteral& literal) {
    return {
        {"range", rangeJson(literal.range)}, {"decodedValue", literal.decodedValue}, {"spelling", literal.spelling}};
}

void printJson(llvm::json::Object object) {
    llvm::outs() << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(object)));
}

int inputError(llvm::StringRef message) {
    printJson({{"schemaVersion", 1}, {"error", llvm::json::Object{{"code", "invalid-request"}, {"message", message}}}});
    return 2;
}

std::optional<Request> parseRequest(const llvm::json::Value& value) {
    const auto* object = value.getAsObject();
    if (!object || object->getInteger("schemaVersion") != 1)
        return std::nullopt;
    const auto root = object->getString("workspaceRoot");
    const auto database = object->getString("compileCommandsDirectory");
    const auto source = object->getString("translationUnit");
    const auto* registrations = object->getArray("registrations");
    if (!root || !database || !source || !registrations || registrations->size() > 10000)
        return std::nullopt;
    const auto rootPath = physicalPath(*root);
    const auto databasePath = physicalPath(*database);
    const auto sourcePath = physicalPath(*source);
    if (!rootPath || !databasePath || !sourcePath || !insideRoot(*rootPath, *sourcePath))
        return std::nullopt;
    Request request{*rootPath, *databasePath, *sourcePath, object->getInteger("compileCommandIndex"), {}, {}, true, {}};
    if (object->get("compileCommandIndex") && (!request.commandIndex || *request.commandIndex < 0))
        return std::nullopt;
    std::set<std::string> ids;
    for (const auto& entry : *registrations) {
        const auto* row = entry.getAsObject();
        if (!row)
            return std::nullopt;
        const auto id = row->getString("id");
        const auto qualifiedName = row->getString("qualifiedName");
        const auto name = row->getString("name");
        const auto kind = row->getString("kind");
        const auto* location = row->getObject("source");
        if (!id || id->empty() || !ids.insert(id->str()).second || !qualifiedName || qualifiedName->empty() || !name ||
            !kind || !location)
            return std::nullopt;
        const auto file = location->getString("file");
        const auto line = location->getInteger("line");
        if (!file || file->empty() || file->contains('\\') || file->contains(':') ||
            llvm::sys::path::is_absolute(*file) || !line || *line < 1 || *line > UINT32_MAX)
            return std::nullopt;
        for (auto component = llvm::sys::path::begin(*file, llvm::sys::path::Style::posix);
             component != llvm::sys::path::end(*file); ++component)
            if (*component == "." || *component == "..")
                return std::nullopt;
        llvm::SmallString<256> joined(request.root);
        llvm::sys::path::append(joined, *file);
        const auto path = physicalPath(joined);
        if (!path || !insideRoot(request.root, *path))
            return std::nullopt;
        request.registrations.push_back({id->str(),
                                         qualifiedName->str(),
                                         name->str(),
                                         kind->str(),
                                         file->str(),
                                         *path,
                                         static_cast<unsigned>(*line),
                                         {}});
    }
    return request;
}

std::optional<Range> sourceRange(clang::SourceRange range, const clang::ASTContext& context, llvm::StringRef root) {
    if (range.isInvalid() || range.getBegin().isMacroID() || range.getEnd().isMacroID())
        return std::nullopt;
    const auto& sm = context.getSourceManager();
    const auto chars =
        clang::Lexer::makeFileCharRange(clang::CharSourceRange::getTokenRange(range), sm, context.getLangOpts());
    if (chars.isInvalid() || chars.getBegin().isMacroID() || chars.getEnd().isMacroID() ||
        sm.getFileID(chars.getBegin()) != sm.getFileID(chars.getEnd()))
        return std::nullopt;
    const auto path = physicalPath(sm.getFilename(chars.getBegin()));
    if (!path || !insideRoot(root, *path))
        return std::nullopt;
    const unsigned begin = sm.getFileOffset(chars.getBegin());
    const unsigned end = sm.getFileOffset(chars.getEnd());
    if (end <= begin)
        return std::nullopt;
    return Range{*path, begin, end - begin};
}

std::optional<Range> nativeRange(const clang::NamedDecl& declaration, const clang::ASTContext& context,
                                 llvm::StringRef root) {
    if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration)) {
        const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
        if (method && method->getParent()->isLambda())
            return sourceRange(function->getSourceRange(), context, root);
        return sourceRange(function->getNameInfo().getSourceRange(), context, root);
    }
    return sourceRange(clang::SourceRange(declaration.getLocation()), context, root);
}

const clang::Expr* unwrapped(const clang::Expr* expression) {
    for (;;) {
        expression = expression->IgnoreParenImpCasts();
        if (const auto* cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(expression))
            expression = cleanups->getSubExpr();
        else if (const auto* temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression))
            expression = temporary->getSubExpr();
        else if (const auto* bound = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression))
            expression = bound->getSubExpr();
        else
            return expression;
    }
}

const clang::StringLiteral* literalName(const clang::Expr* expression) {
    expression = unwrapped(expression);
    if (const auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(expression)) {
        if (construct->getConstructor()->getParent()->getQualifiedNameAsString().starts_with(
                "std::basic_string_view") &&
            construct->getNumArgs() == 1)
            expression = unwrapped(construct->getArg(0));
    }
    const auto* literal = llvm::dyn_cast<clang::StringLiteral>(expression);
    return literal && literal->getCharByteWidth() == 1 ? literal : nullptr;
}

void captureNameLiteral(const clang::StringLiteral& literal, const clang::ASTContext& context, llvm::StringRef root,
                        Candidate& candidate) {
    if (literal.getNumConcatenated() != 1) {
        candidate.nameLiteralReason = "concatenated-registration-name";
        return;
    }
    const auto range = sourceRange(literal.getSourceRange(), context, root);
    bool invalid = false;
    const auto spelling = clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(literal.getSourceRange()),
                                                      context.getSourceManager(), context.getLangOpts(), &invalid);
    if (!range || invalid || spelling.size() != range->length) {
        candidate.nameLiteralReason = "macro-or-nonfile-registration-name";
        return;
    }
    candidate.nameLiteral = NameLiteral{*range, literal.getBytes().str(), spelling.str()};
}

void captureDocSignature(const clang::CXXMemberCallExpr& call, const Registration& registration,
                         const clang::ASTContext& context, llvm::StringRef root, Candidate& candidate) {
    candidate.docSignatureReason = "structured-signature-literal-unavailable";
    if (call.getNumArgs() < 2)
        return;
    const clang::Expr* expression = unwrapped(call.getArg(call.getNumArgs() - 2));
    for (unsigned depth = 0; depth < 8; ++depth) {
        const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(expression);
        const auto* variable = ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        if (!variable)
            break;
        if (!variable->hasInit() || (!variable->isConstexpr() && (!variable->getType().isConstQualified() ||
                                                                  variable->getType().isVolatileQualified())))
            return;
        expression = unwrapped(variable->getInit());
    }
    const auto* aggregate = llvm::dyn_cast<clang::InitListExpr>(expression);
    if (!aggregate)
        return;
    if (!aggregate->isSemanticForm())
        aggregate = aggregate->getSemanticForm();
    const auto* record = aggregate ? aggregate->getType()->getAsCXXRecordDecl() : nullptr;
    if (!record || record->getQualifiedNameAsString() != "Frost::ScriptDoc")
        return;
    unsigned index = 0;
    for (const auto* field : record->fields()) {
        if (field->getName() != "Signature") {
            ++index;
            continue;
        }
        const auto* value = index < aggregate->getNumInits() ? aggregate->getInit(index) : nullptr;
        const auto* literal = value ? literalName(value) : nullptr;
        if (!literal)
            return;
        Candidate captured;
        captureNameLiteral(*literal, context, root, captured);
        if (!captured.nameLiteral) {
            candidate.docSignatureReason = captured.nameLiteralReason;
            return;
        }
        const llvm::StringRef decoded(captured.nameLiteral->decodedValue);
        const auto separator = decoded.find(registration.kind == "Property" ? ':' : '(');
        const auto name = decoded.take_front(separator).trim(" \t\r\n");
        if (separator == llvm::StringRef::npos || name != registration.name) {
            candidate.docSignatureReason = "structured-signature-name-mismatch";
            return;
        }
        const auto nameOffset = static_cast<unsigned>(name.data() - decoded.data());
        candidate.docSignature =
            SignatureLiteral{std::move(*captured.nameLiteral), nameOffset, static_cast<unsigned>(name.size())};
        candidate.docSignatureReason.clear();
        return;
    }
}

struct ResolvedArgument {
    std::vector<const clang::NamedDecl*> declarations;
    std::string reason;
};

ResolvedArgument resolveArgument(const clang::Expr* expression, unsigned depth = 0) {
    if (depth > 32)
        return {{}, "alias-depth-exceeded"};
    expression = unwrapped(expression);
    if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(expression))
        return resolveArgument(cast->getSubExpr(), depth + 1);
    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
        if (unary->getOpcode() == clang::UO_AddrOf || unary->getOpcode() == clang::UO_Plus)
            return resolveArgument(unary->getSubExpr(), depth + 1);
    }
    if (const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(expression))
        return {{lambda->getCallOperator()}, {}};
    if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        const clang::ValueDecl* declaration = reference->getDecl();
        if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration))
            return {{function}, {}};
        if (const auto* field = llvm::dyn_cast<clang::FieldDecl>(declaration))
            return {{field}, {}};
        if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration)) {
            if ((variable->isConstexpr() ||
                 (variable->getType().isConstQualified() && !variable->getType().isVolatileQualified())) &&
                variable->hasInit())
                return resolveArgument(variable->getInit(), depth + 1);
            return {{}, "mutable-or-uninitialized-alias"};
        }
    }
    if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
        if (llvm::isa<clang::FunctionDecl>(member->getMemberDecl()))
            return {{member->getMemberDecl()}, {}};
        if (llvm::isa<clang::FieldDecl>(member->getMemberDecl()))
            return {{}, "mutable-member-native-target"};
    }
    if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
        auto left = resolveArgument(conditional->getTrueExpr(), depth + 1);
        auto right = resolveArgument(conditional->getFalseExpr(), depth + 1);
        left.declarations.insert(left.declarations.end(), right.declarations.begin(), right.declarations.end());
        left.reason = "conditional-native-target";
        return left;
    }
    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
        if (const auto* method = llvm::dyn_cast_or_null<clang::CXXConversionDecl>(call->getDirectCallee());
            method && method->getParent()->isLambda())
            return {{method->getParent()->getLambdaCallOperator()}, {}};
        if (const auto* callee = call->getDirectCallee();
            callee && callee->getQualifiedNameAsString() == "sol::overload") {
            ResolvedArgument result{{}, "overloaded-native-target"};
            for (const clang::Expr* argument : call->arguments()) {
                const auto resolved = resolveArgument(argument, depth + 1);
                result.declarations.insert(result.declarations.end(), resolved.declarations.begin(),
                                           resolved.declarations.end());
            }
            return result;
        }
    }
    return {{}, "unsupported-native-expression"};
}

struct CallableSlot {
    unsigned index;
    std::string role;
};

std::vector<CallableSlot> callableSlots(const clang::CXXMethodDecl& method, llvm::StringRef kind) {
    const std::string parent = method.getParent()->getQualifiedNameAsString();
    if (parent != "Frost::ScriptApiBuilder" && parent != "Frost::ScriptNamespaceBuilder" &&
        parent != "Frost::ScriptTypeBuilder" && parent != "Frost::ScriptComponentApiBuilder")
        return {};
    const std::string name = method.getNameAsString();
    if ((name == "Function" || name == "RawFunction") &&
        ((kind == "GlobalFunction" && parent == "Frost::ScriptApiBuilder") ||
         (kind == "Function" && parent == "Frost::ScriptNamespaceBuilder")))
        return {{1, "callable"}};
    if ((name == "StaticFunction" || name == "RawStaticFunction") && kind == "Function" &&
        parent == "Frost::ScriptTypeBuilder")
        return {{1, "callable"}};
    if ((name == "Method" || name == "RawMethod") && kind == "Method")
        return {{1, "callable"}};
    if ((name == "Property" || name == "RawProperty") && kind == "Property") {
        if (method.getNumParams() == 4)
            return {{1, "getter"}};
        if (method.getNumParams() == 5)
            return {{1, "getter"}, {2, "setter"}};
    }
    if (name == "RawStaticProperty" && kind == "Property" && parent == "Frost::ScriptTypeBuilder")
        return {{1, "getter"}};
    if ((name == "Constructor" || name == "RawConstructor") && kind == "Constructor" &&
        parent == "Frost::ScriptTypeBuilder")
        return {{0, "callable"}};
    if ((name == "Operator" || name == "RawOperator") && kind == "Operator" && parent == "Frost::ScriptTypeBuilder")
        return {{1, "callable"}};
    return {};
}

class BindingVisitor final : public clang::RecursiveASTVisitor<BindingVisitor> {
public:
    BindingVisitor(clang::ASTContext& context, Request& request) : m_context(context), m_request(request) {
        for (auto& registration : m_request.registrations)
            m_byLine[registration.line].push_back(&registration);
    }

    bool shouldVisitTemplateInstantiations() const { return true; }

    // Only this actual engine API promises that argument zero is a complete Lua chunk.
    bool VisitCallExpr(clang::CallExpr* call) {
        const auto* callee = call->getDirectCallee();
        if (!callee || callee->getQualifiedNameAsString() != "Frost::ScriptEngine::ExecuteString")
            return true;
        const auto& sm = m_context.getSourceManager();
        const auto location = sm.getExpansionLoc(call->getExprLoc());
        const auto path = physicalPath(sm.getFilename(location));
        if (!path || !insideRoot(m_request.root, *path))
            return true;
        const auto* literal = call->getNumArgs() ? literalName(call->getArg(0)) : nullptr;
        Candidate candidate;
        if (literal)
            captureNameLiteral(*literal, m_context, m_request.root, candidate);
        if (!candidate.nameLiteral) {
            m_request.embeddedLuaRefusals.push_back(*path + ":" + std::to_string(sm.getExpansionLineNumber(location)) +
                                                    ": computed/macro/concatenated Lua chunk");
        } else {
            m_request.embeddedLua.push_back(std::move(*candidate.nameLiteral));
        }
        return true;
    }

    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr* call) {
        const auto* method = call->getMethodDecl();
        if (!method)
            return true;
        const auto& sm = m_context.getSourceManager();
        const auto location = sm.getExpansionLoc(call->getExprLoc());
        if (location.isInvalid())
            return true;
        const unsigned line = sm.getExpansionLineNumber(location);
        const auto matchingLine = m_byLine.find(line);
        if (matchingLine == m_byLine.end())
            return true;
        const auto rawFile = sm.getFilename(location);
        auto [cached, inserted] = m_paths.try_emplace(rawFile.str(), std::nullopt);
        if (inserted)
            cached->second = physicalPath(rawFile);
        if (!cached->second)
            return true;
        for (auto* entry : matchingLine->second) {
            auto& registration = *entry;
            if (!samePath(registration.physicalFile, *cached->second))
                continue;
            const auto slots = callableSlots(*method, registration.kind);
            if (slots.empty() || call->getNumArgs() < 2)
                continue;
            Candidate candidate;
            // Constructors/operators have no literal name argument. Their physical call site and actual
            // builder method identify the registration; colliding calls stay ambiguous below.
            if (registration.kind != "Constructor" && registration.kind != "Operator") {
                const auto* name = literalName(call->getArg(0));
                if (!name) {
                    candidate.status = "unavailable";
                    candidate.reason = "registration-name-not-literal";
                    candidate.nameLiteralReason = candidate.reason;
                } else if (name->getBytes() != registration.name) {
                    continue;
                } else {
                    captureNameLiteral(*name, m_context, m_request.root, candidate);
                }
            } else {
                candidate.nameLiteralReason = "registration-has-no-literal-name";
            }
            const auto range = sourceRange(call->getSourceRange(), m_context, m_request.root);
            captureDocSignature(*call, registration, m_context, m_request.root, candidate);
            if (!range || call->getExprLoc().isMacroID()) {
                candidate.status = "unavailable";
                candidate.reason = "macro-or-nonfile-registration";
            } else {
                candidate.registration = *range;
                if (candidate.status == "mapped")
                    for (const auto& slot : slots)
                        resolveSlot(*call, slot, candidate);
            }
            registration.candidates.push_back(std::move(candidate));
        }
        return true;
    }

private:
    void resolveSlot(const clang::CallExpr& call, const CallableSlot& slot, Candidate& candidate) {
        if (slot.index >= call.getNumArgs()) {
            candidate.status = "unavailable";
            candidate.reason = "missing-native-argument";
            return;
        }
        const clang::Expr* argument = call.getArg(slot.index);
        if (slot.role == "setter" &&
            argument->isNullPointerConstant(m_context, clang::Expr::NPC_ValueDependentIsNotNull))
            return; // An explicitly null raw setter proves the property is read-only.
        const auto range = sourceRange(argument->getSourceRange(), m_context, m_request.root);
        const auto resolved = resolveArgument(argument);
        std::set<std::string> seen;
        if (!range || resolved.declarations.empty()) {
            candidate.status = "unavailable";
            candidate.reason = range ? resolved.reason : "macro-or-nonfile-native-argument";
            return;
        }
        for (const auto* declaration : resolved.declarations) {
            const auto* canonical = llvm::cast<clang::NamedDecl>(declaration->getCanonicalDecl());
            llvm::SmallString<128> usr;
            const auto declarationRange = nativeRange(*canonical, m_context, m_request.root);
            if (clang::index::generateUSRForDecl(canonical, usr) || usr.empty() || !declarationRange) {
                candidate.status = "unavailable";
                candidate.reason = "native-identity-or-workspace-range-unavailable";
                continue;
            }
            if (!seen.insert(usr.str().str()).second)
                continue;
            Target target{slot.role, usr.str().str(), canonical->getQualifiedNameAsString(), "field", *declarationRange,
                          {},        *range};
            if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(canonical)) {
                const auto* member = llvm::dyn_cast<clang::CXXMethodDecl>(function);
                target.kind = member && member->getParent()->isLambda() ? "lambda" : member ? "method" : "function";
                if (const auto* definition = function->getDefinition())
                    target.definition = nativeRange(*definition, m_context, m_request.root);
            }
            candidate.targets.push_back(std::move(target));
        }
        if ((seen.size() > 1 || !resolved.reason.empty()) && candidate.status != "unavailable") {
            candidate.status = "ambiguous";
            candidate.reason = resolved.reason.empty() ? "multiple-native-targets" : resolved.reason;
        }
    }

    clang::ASTContext& m_context;
    Request& m_request;
    // Invocation-local indexes avoid a filesystem canonicalization and catalog scan per AST call.
    std::map<unsigned, std::vector<Registration*>> m_byLine;
    std::map<std::string, std::optional<std::string>, std::less<>> m_paths;
};

class BindingConsumer final : public clang::ASTConsumer {
public:
    explicit BindingConsumer(Request& request) : m_request(request) {}
    void HandleTranslationUnit(clang::ASTContext& context) override {
        BindingVisitor visitor(context, m_request);
        visitor.TraverseDecl(context.getTranslationUnitDecl());
        const auto& sm = context.getSourceManager();
        for (auto entry = sm.fileinfo_begin(); entry != sm.fileinfo_end(); ++entry) {
            const auto file = sm.translateFile(entry->first);
            if (file.isInvalid())
                continue;
            if (sm.isLoadedFileID(file)) {
                // Serialized inputs outside the workspace also affect this AST. A lazily read
                // current buffer cannot prove the bytes originally parsed into any PCH/module.
                m_request.sourceInputsComplete = false;
                continue;
            }
            const auto start = sm.getLocForStartOfFile(file);
            if (start.isInvalid())
                continue;
            const auto real = entry->first.getFileEntry().tryGetRealPathName();
            std::optional<std::string> path;
            if (!real.empty() && llvm::sys::path::is_absolute(real)) {
                // FileManager recorded this physical name when it opened the parsed input. Keep
                // it even if the file is replaced/deleted after parsing; consumers then reject it.
                llvm::SmallString<256> recorded(real);
                llvm::sys::path::native(recorded, llvm::sys::path::Style::posix);
                path = recorded.str().str();
            } else {
                path = physicalPath(entry->first.getName());
            }
            if (!path) {
                m_request.sourceInputsComplete = false;
                continue;
            }
            if (!insideRoot(m_request.root, *path))
                continue;
            // Digest the exact buffer participating in this AST, never a fresh disk read. PCH
            // inputs without retained original buffers cannot prove source provenance here.
            const auto bytes = entry->second->getBufferDataIfLoaded();
            if (!bytes) {
                m_request.sourceInputsComplete = false;
                continue;
            }
            llvm::SHA256 hash;
            hash.update(*bytes);
            const auto digest = llvm::toHex(hash.final(), true);
            const auto [previous, inserted] = m_request.sourceInputs.emplace(*path, digest);
            if (!inserted && previous->second != digest)
                m_request.sourceInputsComplete = false;
        }
        if (std::ranges::none_of(m_request.sourceInputs,
                                 [&](const auto& input) { return samePath(input.first, m_request.source); }))
            m_request.sourceInputsComplete = false;
    }

private:
    Request& m_request;
};

class BindingAction final : public clang::ASTFrontendAction {
public:
    explicit BindingAction(Request& request) : m_request(request) {}
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override {
        return std::make_unique<BindingConsumer>(m_request);
    }

private:
    Request& m_request;
};

class BindingFactory final : public clang::tooling::FrontendActionFactory {
public:
    explicit BindingFactory(Request& request) : m_request(request) {}
    std::unique_ptr<clang::FrontendAction> create() override { return std::make_unique<BindingAction>(m_request); }

private:
    Request& m_request;
};

class OneCommandDatabase final : public clang::tooling::CompilationDatabase {
public:
    explicit OneCommandDatabase(clang::tooling::CompileCommand command) : m_command(std::move(command)) {}
    std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef) const override {
        return {m_command};
    }

private:
    clang::tooling::CompileCommand m_command;
};

bool unsafeCommand(const clang::tooling::CompileCommand& command) {
    for (const auto& argument : command.CommandLine) {
        llvm::StringRef flag(argument);
        flag.consume_front("/clang:");
        // Tooling strips normal object/dependency outputs below. Refuse alternate output, response
        // expansion and executable plugin channels instead of quietly analyzing altered semantics.
        if (flag == "--" || flag.starts_with("@") || flag.contains("-Xclang") || flag.starts_with("-Wp,") ||
            flag.contains("-fplugin") || flag.contains("-load") || flag.contains("serialize-diagnostics") ||
            flag.contains("-ftime-trace") || flag.contains("vfsoverlay") || flag.starts_with("/Yc") ||
            flag.starts_with("-fmodule-output") || flag.starts_with("-save-temps") || flag.starts_with("-MJ") ||
            flag.contains("-gen-reproducer") || flag.contains("-fdiagnostics-serialization-file") ||
            flag.contains("-fdiagnostics-log-file") || flag.contains("--config"))
            return true;
    }
    return false;
}

void report(const Request& request, bool parsed, llvm::StringRef failure) {
    llvm::json::Array rows;
    for (const auto& registration : request.registrations) {
        std::vector<std::pair<std::string, const Candidate*>> distinct;
        for (const auto& candidate : registration.candidates) {
            llvm::json::Array targets;
            for (const auto& target : candidate.targets)
                targets.push_back(targetJson(target));
            const std::string identity =
                llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{{"range", rangeJson(candidate.registration)},
                                                                          {"status", candidate.status},
                                                                          {"reason", candidate.reason},
                                                                          {"targets", std::move(targets)}}))
                    .str();
            if (std::ranges::none_of(distinct, [&](const auto& item) { return item.first == identity; }))
                distinct.emplace_back(identity, &candidate);
        }
        llvm::json::Object row{{"id", registration.id},
                               {"qualifiedName", registration.qualifiedName},
                               {"source", llvm::json::Object{{"file", registration.file}, {"line", registration.line}}},
                               {"status", "unavailable"},
                               {"reason", parsed ? "no-exact-registration" : failure},
                               {"targets", llvm::json::Array{}}};
        if (parsed && distinct.size() == 1) {
            const auto& candidate = *distinct.front().second;
            row["status"] = candidate.status;
            row["reason"] = candidate.reason;
            if (!candidate.registration.path.empty())
                row["registrationRange"] = rangeJson(candidate.registration);
            if (candidate.nameLiteral && candidate.status == "mapped")
                row["nameLiteral"] = nameLiteralJson(*candidate.nameLiteral);
            else
                row["nameLiteralReason"] =
                    candidate.nameLiteral ? "native-binding-unresolved" : candidate.nameLiteralReason;
            if (candidate.docSignature && candidate.status == "mapped") {
                auto signature = nameLiteralJson(candidate.docSignature->literal);
                signature["decodedNameOffset"] = candidate.docSignature->nameOffset;
                signature["decodedNameLength"] = candidate.docSignature->nameLength;
                row["docSignatureLiteral"] = std::move(signature);
            } else {
                row["docSignatureLiteralReason"] =
                    candidate.docSignature ? "native-binding-unresolved" : candidate.docSignatureReason;
            }
            llvm::json::Array targets;
            if (candidate.status == "mapped")
                for (const auto& target : candidate.targets)
                    targets.push_back(targetJson(target));
            row["targets"] = std::move(targets);
        } else if (parsed && distinct.size() > 1) {
            row["status"] = "ambiguous";
            row["reason"] = "multiple-registration-ast-candidates";
            row["nameLiteralReason"] = "ambiguous-registration-site";
        }
        rows.push_back(std::move(row));
    }
    llvm::json::Array sourceInputs;
    for (const auto& [path, digest] : request.sourceInputs)
        sourceInputs.push_back(llvm::json::Object{{"path", path}, {"sha256", digest}});
    llvm::json::Array embeddedLua, embeddedLuaRefusals;
    for (const auto& chunk : request.embeddedLua)
        embeddedLua.push_back(nameLiteralJson(chunk));
    for (const auto& reason : request.embeddedLuaRefusals)
        embeddedLuaRefusals.push_back(reason);
    printJson({{"schemaVersion", 1},
               {"translationUnitParsed", parsed},
               {"embeddedLuaProfile", "Frost::ScriptEngine::ExecuteString/v1"},
               {"embeddedLua", std::move(embeddedLua)},
               {"embeddedLuaRefusals", std::move(embeddedLuaRefusals)},
               {"sourceInputs", std::move(sourceInputs)},
               {"compilationContext", bindingCompileContextJson(request.compileContext)},
               {"results", std::move(rows)}});
}

} // namespace

int runBindingMap(int argc, char* argv[], std::string_view resourceDirectory) {
    if (argc != 3)
        return inputError("usage: frost-clang --binding-map <request.json>");
    const auto input = llvm::MemoryBuffer::getFile(argv[2]);
    if (!input || (*input)->getBufferSize() > 16U * 1024U * 1024U)
        return inputError("binding-map request must be a readable JSON file of at most 16 MiB");
    auto json = llvm::json::parse((*input)->getBuffer());
    if (!json) {
        llvm::consumeError(json.takeError());
        return inputError("binding-map request is not valid JSON");
    }
    auto request = parseRequest(*json);
    if (!request)
        return inputError("invalid version, paths, registration identities or compilation context");
    std::string error;
    auto database = clang::tooling::CompilationDatabase::loadFromDirectory(request->database, error);
    if (!database) {
        report(*request, false, "compile-database-unavailable");
        return 1;
    }
    const auto commands = database->getCompileCommands(request->source);
    if (commands.empty() || (!request->commandIndex && commands.size() != 1) ||
        (request->commandIndex && static_cast<std::uint64_t>(*request->commandIndex) >= commands.size())) {
        report(*request, false, commands.empty() ? "compile-command-unavailable" : "ambiguous-compilation-command");
        return 1;
    }
    const auto& command = commands[request->commandIndex ? static_cast<std::size_t>(*request->commandIndex) : 0];
    if (unsafeCommand(command)) {
        report(*request, false, "compile-command-has-unsafe-side-effects");
        return 1;
    }
    request->compileContext = bindingSourceCompileContext(command, request->root);
    if (!request->compileContext.error.empty()) {
        report(*request, false, request->compileContext.error);
        return 1;
    }
    OneCommandDatabase selected(request->compileContext.sourceParse);
    clang::tooling::ClangTool tool(selected, {request->source});
    tool.appendArgumentsAdjuster(clang::tooling::getClangStripOutputAdjuster());
    tool.appendArgumentsAdjuster(clang::tooling::getClangStripDependencyFileAdjuster());
    tool.appendArgumentsAdjuster(clang::tooling::getClangSyntaxOnlyAdjuster());
    tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
        {"--no-default-config", "-fno-implicit-modules", "-fno-implicit-module-maps"},
        clang::tooling::ArgumentInsertPosition::END));
    if (!resourceDirectory.empty())
        tool.appendArgumentsAdjuster(
            clang::tooling::getInsertArgumentAdjuster(("-resource-dir=" + std::string(resourceDirectory)).c_str(),
                                                      clang::tooling::ArgumentInsertPosition::BEGIN));
    tool.appendArgumentsAdjuster([&](const clang::tooling::CommandLineArguments& arguments, llvm::StringRef) {
        request->compileContext.analysisArguments = arguments;
        return arguments;
    });
    BindingFactory factory(*request);
    const bool frontendParsed = tool.run(&factory) == 0;
    const bool parsed = frontendParsed && request->sourceInputsComplete;
    report(*request, parsed, frontendParsed ? "source-input-buffers-unavailable" : "translation-unit-parse-failed");
    return parsed ? 0 : 1;
}

} // namespace frost::clang_engine
