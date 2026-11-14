// SPDX-License-Identifier: Apache-2.0
// REF-6/7 asynchronous AST operation client; proposals share R8 immutable transaction receipts.
#pragma once
#include "frost/lang/cpp_rename_plan.hpp"
namespace frost::lang {
struct CppRefactoringRequest {
    CppRenameRequest context;
    std::string operation; // Whitelisted signature/inline/extract/safe-delete and generate-* operations.
    std::uint64_t endOffset = 0;
    std::vector<unsigned> parameterOrder; // Complete existing-parameter permutation, not arbitrary source text.
};
[[nodiscard]] Json cppRefactoringRequestJson(const CppRefactoringRequest& request);
// Does not apply or write source. Caller captures/rechecks all original images, namespace probes,
// open-buffer classifications, trust and project generation before one indivisible transaction.
[[nodiscard]] sched::Task<Result<CppRenameProposal>>
requestCppRefactoringPlan(io::IProcessLauncher& launcher, io::AsyncContext context, std::string helper,
                          CppRefactoringRequest request, IClock::Duration timeout, std::stop_token stop = {});
} // namespace frost::lang
