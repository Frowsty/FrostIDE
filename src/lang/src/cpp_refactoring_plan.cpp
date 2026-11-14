// SPDX-License-Identifier: Apache-2.0
// Typed operation dispatch using the existing contained semantic helper and receipt validation.
#include "frost/lang/cpp_refactoring_plan.hpp"

#include <algorithm>
#include <array>
namespace frost::lang {
Json cppRefactoringRequestJson(const CppRefactoringRequest& request) {
    std::vector<Json> order;
    for (const auto index : request.parameterOrder)
        order.emplace_back(static_cast<std::uint64_t>(index));
    return cppRenameRequestJson(request.context)
        .set("operation", request.operation)
        .set("usageReturnType", request.usageReturnType)
        .set("usageBody", request.usageBody)
        .set("endOffset", request.endOffset)
        .set("parameterOrder", Json::array(std::move(order)));
}
sched::Task<Result<CppRenameProposal>> requestCppRefactoringPlan(io::IProcessLauncher& launcher,
                                                                 io::AsyncContext context, std::string helper,
                                                                 CppRefactoringRequest request,
                                                                 IClock::Duration timeout, std::stop_token stop) {
    constexpr std::array operations{"change-signature",  "inline-variable",     "inline-function",
                                    "extract-variable",  "extract-constant",    "extract-method",
                                    "extract-field",     "safe-delete",         "generate-accessors",
                                    "generate-equality", "generate-formatting", "generate-overrides", "create-from-usage"};
    if (std::ranges::find(operations, request.operation) == operations.end() ||
        request.endOffset < request.context.offset ||
        (!request.operation.starts_with("generate-") && request.context.path != request.context.seedTranslationUnit))
        co_return fail(ErrorCode::Unsupported, "Unknown AST operation or non-source selection");
    auto payload = cppRefactoringRequestJson(request);
    co_return co_await requestCppSemanticPlan(launcher, context, std::move(helper), std::move(request.context),
                                              std::move(payload), "--plan-ast-refactoring", "cpp-refactoring-plan",
                                              timeout, stop);
}
} // namespace frost::lang
