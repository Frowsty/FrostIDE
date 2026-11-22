// SPDX-License-Identifier: Apache-2.0
// Resolved-expression inspections; warnings describe proven syntax/type facts, never lexical guesses.
#include "analyzer_ast.hpp"
#include <clang/Basic/SourceManager.h>

namespace frost::clang_engine {
namespace {
using namespace analysis;
class Expressions final : public clang::RecursiveASTVisitor<Expressions> {
    AnalyzerResults& output;
    clang::ASTContext& context;

public:
    explicit Expressions(AnalyzerResults& o) : output(o), context(o.context) {}
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool TraverseDecl(clang::Decl* declaration) {
        if (declaration && declaration->getLocation().isValid() &&
            context.getSourceManager().isInSystemHeader(declaration->getLocation()))
            return true;
        return clang::RecursiveASTVisitor<Expressions>::TraverseDecl(declaration);
    }
    void add(const char* id, const clang::Stmt& s, const char* message, std::optional<std::string> fix = {}) {
        output.add(id, s.getSourceRange(), message, std::move(fix));
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitBinaryOperator(clang::BinaryOperator* e) {
        const auto* lhs = clean(e->getLHS());
        const auto* rhs = clean(e->getRHS());
        if (e->isTypeDependent())
            return true;
        const auto ltype = lhs->getType(), rtype = rhs->getType();
        if (e->getOpcode() == clang::BO_Assign && identical(lhs, rhs) && ltype->isScalarType())
            add("frost-self-assignment", *e, "Scalar assignment reads and writes the same declaration.");
        if (e->isEqualityOp() && ltype->isBooleanType() && rtype->isBooleanType()) {
            const auto* literal = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(rhs);
            const auto* value = lhs;
            if (!literal) {
                literal = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(lhs);
                value = rhs;
            }
            if (literal) {
                auto original = text(value->getSourceRange(), context);
                std::optional<std::string> fix;
                if (original)
                    fix = ((literal->getValue() == (e->getOpcode() == clang::BO_EQ)) ? "(" : "!(") + *original + ")";
                add("frost-boolean-comparison", *e, "Built-in bool comparison can use the boolean value directly.",
                    std::move(fix));
            }
        }
        if (e->isBitwiseOp() && ltype->isBooleanType() && rtype->isBooleanType())
            add("frost-bitwise-bool", *e,
                "Bitwise boolean operation evaluates both operands; confirm logical short-circuiting was not "
                "intended.");
        if (e->isComparisonOp() && identical(lhs, rhs) &&
            (ltype->isIntegralOrEnumerationType() || ltype->isPointerType()))
            add("frost-identical-comparison", *e,
                "Comparison uses the same nonvolatile scalar declaration on both sides.");
        if (e->isLogicalOp() && identical(lhs, rhs) && ltype->isScalarType())
            add("frost-repeated-logical-operand", *e, "Logical operation repeats the same nonvolatile scalar operand.");
        if (e->isAdditiveOp() && ltype->isPointerType())
            if (auto value = integer(rhs, context); value && value->isZero())
                add("frost-pointer-zero-arithmetic", *e, "Pointer arithmetic adds or subtracts zero.");
        if (e->isEqualityOp() && ((e->getLHS()->getType()->isPointerType() && llvm::isa<clang::StringLiteral>(rhs)) ||
                                  (e->getRHS()->getType()->isPointerType() && llvm::isa<clang::StringLiteral>(lhs))))
            add("frost-literal-address-comparison", *e,
                "Built-in comparison tests the string literal address, not its characters.");
        if (e->getOpcode() == clang::BO_Div || e->getOpcode() == clang::BO_Rem) {
            if (e->getLHS()->getType()->isIntegerType())
                if (auto value = integer(rhs, context); value && value->isZero())
                    add("frost-divide-zero", *e, "Integral divisor is a constant zero.");
            const auto* a = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(lhs);
            const auto* b = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(rhs);
            if (e->getOpcode() == clang::BO_Div && a && b && a->getKind() == clang::UETT_SizeOf &&
                b->getKind() == clang::UETT_SizeOf && a->getTypeOfArgument()->isPointerType() &&
                context.hasSameUnqualifiedType(a->getTypeOfArgument()->getPointeeType(), b->getTypeOfArgument()))
                add("frost-sizeof-pointer-count", *e,
                    "sizeof(pointer) divided by sizeof(pointee) does not count array elements.");
        }
        if (e->isShiftOp())
            if (auto value = integer(rhs, context);
                value &&
                (value->isNegative() || value->getLimitedValue() >= context.getIntWidth(e->getLHS()->getType())))
                add("frost-shift-range", *e, "Shift count is negative or at least the promoted left operand width.");
        return true;
    }
    void condition(clang::Expr* e) {
        const auto* binary = llvm::dyn_cast_or_null<clang::BinaryOperator>(clean(e));
        if (binary && binary->isAssignmentOp())
            add("frost-assignment-condition", *binary,
                "Built-in assignment is used directly as a condition; confirm this is intentional.");
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitIfStmt(clang::IfStmt* e) {
        condition(e->getCond());
        if (!e->isConstexpr() && e->getCond() && !e->getCond()->isValueDependent() &&
            !e->getCond()->HasSideEffects(context)) {
            bool value = false;
            if (e->getCond()->EvaluateAsBooleanCondition(value, context))
                add("frost-constant-condition", *e->getCond(),
                    value ? "Runtime if condition is provably true." : "Runtime if condition is provably false.");
        }
        if (llvm::isa<clang::NullStmt>(e->getThen()))
            add("frost-empty-if", *e->getThen(),
                "If body is an empty statement; a following block is not controlled by this if.");
        if (const auto* next = llvm::dyn_cast_or_null<clang::IfStmt>(e->getElse());
            next && !next->getInit() && !next->getConditionVariable() && identical(e->getCond(), next->getCond()) &&
            clean(e->getCond())->getType()->isScalarType())
            add("frost-repeated-else-if", *next->getCond(),
                "Else-if repeats the scalar condition already known false on this edge.");
        const auto* a = singleReturn(e->getThen());
        const auto* b = singleReturn(e->getElse());
        const auto* av = a ? llvm::dyn_cast_or_null<clang::CXXBoolLiteralExpr>(clean(a->getRetValue())) : nullptr;
        const auto* bv = b ? llvm::dyn_cast_or_null<clang::CXXBoolLiteralExpr>(clean(b->getRetValue())) : nullptr;
        const auto* function = owner(context, e);
        if (av && bv && av->getValue() != bv->getValue() && function && function->getReturnType()->isBooleanType())
            add("frost-boolean-return-branch", *e,
                "Both branches return opposite boolean literals; the condition can express this result.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitWhileStmt(clang::WhileStmt* e) {
        condition(e->getCond());
        if (llvm::isa<clang::NullStmt>(e->getBody()))
            add("frost-empty-loop", *e->getBody(),
                "While loop has an empty statement body; confirm a deliberate spin loop.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitForStmt(clang::ForStmt* e) {
        if (llvm::isa<clang::NullStmt>(e->getBody()))
            add("frost-empty-loop", *e->getBody(),
                "For loop has an empty statement body; confirm this is intentional.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitCXXStaticCastExpr(clang::CXXStaticCastExpr* e) {
        const auto* operand = clean(e->getSubExpr());
        if (operand->isPRValue() && operand->getType()->isScalarType() &&
            context.hasSameType(e->getType(), operand->getType())) {
            auto bytes = text(operand->getSourceRange(), context);
            add("frost-identity-cast", *e, "Scalar prvalue already has the target canonical type.",
                bytes ? std::optional<std::string>("(" + *bytes + ")") : std::nullopt);
        }
        if (e->getCastKind() == clang::CK_BaseToDerived) {
            auto type = e->getSubExpr()->getType();
            if (type->isPointerType())
                type = type->getPointeeType();
            if (const auto* record = type->getAsCXXRecordDecl();
                record && record->hasDefinition() && record->isPolymorphic())
                add("frost-unchecked-downcast", *e, "Base-to-derived static_cast does not check the dynamic type.");
        }
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitImplicitCastExpr(clang::ImplicitCastExpr* e) {
        if (e->getCastKind() != clang::CK_IntegralCast || !e->getType()->isIntegerType() ||
            e->getType()->isBooleanType())
            return true;
        auto value = integer(e->getSubExpr(), context);
        if (!value)
            return true;
        const unsigned width = context.getIntWidth(e->getType());
        const bool fits = e->getType()->isUnsignedIntegerType()
                              ? (!value->isNegative() && value->getActiveBits() <= width)
                              : (value->isUnsigned() ? value->getActiveBits() < width : value->isSignedIntN(width));
        if (!fits)
            add("frost-out-of-range-conversion", *e,
                "Constant integral value lies outside the destination type range.");
        return true;
    }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): CRTP RecursiveASTVisitor hook
    bool VisitUnaryOperator(clang::UnaryOperator* e) {
        if (e->getOpcode() == clang::UO_Minus && e->getType()->isSignedIntegerType())
            if (auto value = integer(e->getSubExpr(), context); value && value->isMinSignedValue())
                add("frost-negation-overflow", *e, "Negating this signed minimum cannot be represented in its type.");
        return true;
    }
};
} // namespace
void analyzeSemantics(AnalyzerResults& output) {
    Expressions expressions(output);
    expressions.TraverseDecl(output.context.getTranslationUnitDecl());
    analysis::analyzeDeclarations(output);
}
} // namespace frost::clang_engine
