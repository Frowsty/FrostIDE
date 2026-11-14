// SPDX-License-Identifier: Apache-2.0
// REF-9 member generation from real record/field/base declarations, including included headers.
#include "ast_refactoring_operations.hpp"
#include "clang/AST/Attr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "semantic_snapshot.hpp"

#include <cctype>
namespace frost::clang_engine {
namespace {
class RecordSelection final : public clang::RecursiveASTVisitor<RecordSelection> {
public:
    clang::ASTContext& context;
    std::string path, root;
    unsigned offset;
    clang::CXXRecordDecl* record = nullptr;
    unsigned width = ~0u;
    RecordSelection(clang::ASTContext& context, std::string path, std::string root, unsigned offset)
        : context(context), path(std::move(path)), root(std::move(root)), offset(offset) {}
    bool VisitCXXRecordDecl(clang::CXXRecordDecl* candidate) {
        if (!candidate->isThisDeclarationADefinition() || candidate->isImplicit())
            return true;
        auto& sm = context.getSourceManager();
        const auto begin = candidate->getBeginLoc(), end = candidate->getBraceRange().getEnd();
        if (!begin.isFileID() || !end.isFileID() || sm.getFileID(begin) != sm.getFileID(end) ||
            semantic::absolute(sm.getFilename(begin), root) != path)
            return true;
        const auto first = sm.getFileOffset(begin), last = sm.getFileOffset(end);
        if (offset >= first && offset <= last && last - first < width) {
            record = candidate;
            width = last - first;
        }
        return true;
    }
};
std::string declarator(clang::QualType type, llvm::StringRef name, clang::ASTContext& context) {
    std::string result;
    llvm::raw_string_ostream out(result);
    type.getCanonicalType().print(out, context.getPrintingPolicy(), name);
    return result;
}
std::string suffix(std::string name) {
    if (name.starts_with("m_") && name.size() > 2)
        name.erase(0, 2);
    if (!name.empty() && name.front() >= 'a' && name.front() <= 'z')
        name.front() = static_cast<char>(name.front() - 'a' + 'A');
    return name;
}
bool present(const clang::CXXRecordDecl& record, llvm::StringRef name) {
    for (const auto* declaration : record.decls())
        if (const auto* named = llvm::dyn_cast<clang::NamedDecl>(declaration);
            named && named->getNameAsString() == name)
            return true;
    return false;
}
} // namespace
std::optional<std::string> refactorGenerate(clang::ASTUnit& ast, const llvm::json::Object& request,
                                            std::string& error) {
    auto refuse = [&](std::string reason) -> std::optional<std::string> {
        error = std::move(reason);
        return {};
    };
    auto& context = ast.getASTContext();
    auto& sm = ast.getSourceManager();
    const auto path = request.getString("path"), root = request.getString("projectRoot"),
               operation = request.getString("operation");
    const auto offset = request.getInteger("offset");
    if (!path || !root || !operation || !offset || *offset < 0)
        return refuse("Invalid generation selection");
    RecordSelection selected(context, semantic::absolute(*path, *root), root->str(), static_cast<unsigned>(*offset));
    selected.TraverseDecl(context.getTranslationUnitDecl());
    if (!selected.record)
        return refuse("not-applicable");
    auto& record = *selected.record;
    if (record.isUnion() || record.isDependentContext() || record.getDescribedClassTemplate() ||
        llvm::isa<clang::ClassTemplateSpecializationDecl>(record) || record.getName().empty())
        return refuse("Select a complete named non-template class or struct, not a union");
    const auto close = record.getBraceRange().getEnd();
    const auto file = sm.getFileID(close);
    const auto bytes = sm.getBufferData(file);
    std::string generated = "\npublic:\n";
    std::set<std::string> names;
    const auto reserve = [&](const std::string& name) { return !present(record, name) && names.insert(name).second; };
    if (*operation == "generate-accessors" || *operation == "generate-equality" ||
        *operation == "generate-formatting") {
        if (record.getNumBases() != 0)
            return refuse("This member generator requires a class without bases; base-state policy must be explicit");
        for (const auto* field : record.fields())
            if (!field->getIdentifier() || field->getLocation().isMacroID() || field->getType()->isArrayType() ||
                field->getType()->isReferenceType() || field->getType()->isFunctionPointerType())
                return refuse(
                    "Anonymous, macro, array, reference or function-pointer fields need an explicit member policy");
        if (*operation == "generate-accessors") {
            if (record.field_empty())
                return refuse("The selected class has no instance fields");
            for (const auto* field : record.fields()) {
                const auto fieldName = field->getNameAsString();
                const auto getter = "get" + suffix(fieldName), setter = "set" + suffix(fieldName);
                if (!reserve(getter))
                    return refuse("Accessor name already exists: " + getter);
                auto type = field->getType();
                const auto resultType =
                    type->isScalarType() ? type.getUnqualifiedType() : context.getLValueReferenceType(type.withConst());
                generated += "    /** @brief Read " + fieldName + ". */\n    " +
                             declarator(resultType, getter + "() const", context) + " { return " + fieldName + "; }\n";
                if (!type.isConstQualified()) {
                    if (!reserve(setter))
                        return refuse("Accessor name already exists: " + setter);
                    const auto inputType = type->isScalarType() ? type.getUnqualifiedType()
                                                                : context.getLValueReferenceType(type.withConst());
                    generated += "    /** @brief Assign " + fieldName +
                                 ".\n     * @param value New field value. */\n    void " + setter + "(" +
                                 declarator(inputType, "value", context) + ") { this->" + fieldName + " = value; }\n";
                }
            }
        } else if (*operation == "generate-equality") {
            if (present(record, "operator=="))
                return refuse("The class already declares equality");
            std::string comparison;
            for (const auto* field : record.fields()) {
                if (!comparison.empty())
                    comparison += " && ";
                comparison += "(this->" + field->getNameAsString() + " == other." + field->getNameAsString() + ")";
            }
            generated += "    /** @brief Compare every instance field.\n     * @param other Value to compare.\n     * "
                         "@return Whether all instance fields compare equal. */\n    bool operator==(const " +
                         record.getNameAsString() + "& other) const { return " +
                         (comparison.empty() ? "true" : comparison) + "; }\n";
        } else {
            if (!reserve("formatTo"))
                return refuse("The class already declares formatTo");
            // Stream remains a dependent parameter, so this works with the caller's actual stream
            // without inserting or inventing standard-library include dependencies.
            generated +=
                "    /** @brief Stream named instance fields.\n     * @param out Destination supporting field "
                "insertion. */\n    template<class Stream> void formatTo(Stream& out) const {\n        out << \"" +
                record.getNameAsString() + "{\";\n";
            bool first = true;
            for (const auto* field : record.fields()) {
                generated += "        out << \"" + std::string(first ? "" : ", ") + field->getNameAsString() +
                             "=\" << this->" + field->getNameAsString() + ";\n";
                first = false;
            }
            generated += "        out << \"}\";\n    }\n";
        }
    } else if (*operation == "generate-overrides") {
        if (record.getNumBases() != 1)
            return refuse("Override stub generation requires one non-dependent direct base");
        const auto* base = record.bases_begin()->getType()->getAsCXXRecordDecl();
        if (!base || !base->getDefinition())
            return refuse("The actual base definition is unavailable");
        unsigned count = 0;
        for (const auto* method : base->getDefinition()->methods()) {
            if (!method->isVirtual() || method->isImplicit() || !method->getIdentifier() ||
                method->hasAttr<clang::FinalAttr>())
                continue;
            bool overridden = false;
            for (const auto* own : record.methods())
                for (const auto* original : own->overridden_methods())
                    overridden |= original->getCanonicalDecl() == method->getCanonicalDecl();
            if (overridden)
                continue;
            if (method->isVariadic() || method->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate)
                return refuse("Variadic or templated virtual methods require explicit signatures");
            std::string parameters;
            for (unsigned i = 0; i != method->getNumParams(); ++i) {
                if (i)
                    parameters += ", ";
                parameters += declarator(method->getParamDecl(i)->getType(), "arg" + std::to_string(i), context);
            }
            auto signature = method->getNameAsString() + "(" + parameters + ")";
            if (method->isConst())
                signature += " const";
            if (method->isVolatile())
                signature += " volatile";
            if (method->getRefQualifier() == clang::RQ_LValue)
                signature += " &";
            if (method->getRefQualifier() == clang::RQ_RValue)
                signature += " &&";
            if (const auto* prototype = method->getType()->getAs<clang::FunctionProtoType>();
                prototype && prototype->isNothrow())
                return refuse(
                    "A noexcept virtual needs an explicit nonthrowing stub body; no return value is invented");
            generated += "    /** @brief Override " + method->getQualifiedNameAsString() + ". */\n    " +
                         declarator(method->getReturnType(), signature, context) +
                         " override { throw \"Unimplemented override: " + method->getNameAsString() + "\"; }\n";
            ++count;
        }
        if (!count)
            return refuse("No unimplemented overridable methods in the actual direct base");
    } else
        return refuse("Unknown member-generation operation");
    const auto insertion = sm.getFileOffset(close);
    if (insertion > bytes.size())
        return refuse("Class closing token is outside its captured file");
    // Query declaration-time macro history at the insertion token. An end-of-TU macro lookup
    // would miss a define/undef pair surrounding this class and could silently rename a member.
    for (std::size_t i = 0; i < generated.size();) {
        const auto initial = static_cast<unsigned char>(generated[i]);
        if (!(std::isalpha(initial) || initial == '_' || initial >= 128)) {
            ++i;
            continue;
        }
        const auto first = i++;
        while (i < generated.size()) {
            const auto next = static_cast<unsigned char>(generated[i]);
            if (!(std::isalnum(next) || next == '_' || next == '$' || next >= 128))
                break;
            ++i;
        }
        auto& identifier = ast.getPreprocessor().getIdentifierTable().get(llvm::StringRef(generated).slice(first, i));
        if (ast.getPreprocessor().getMacroDefinitionAtLoc(&identifier, close).getMacroInfo())
            return refuse("A generated identifier is a macro at the insertion point; no member spelling is guessed");
    }
    auto candidate = bytes.str();
    candidate.insert(insertion, generated);
    return candidate;
}
} // namespace frost::clang_engine
