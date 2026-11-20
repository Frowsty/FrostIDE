// SPDX-License-Identifier: Apache-2.0
// CPP-13 actual Clang -E output and callback events for one exact captured compilation command.
#pragma once
#include "frost/lang/cpp_index.hpp"
namespace frost::lang {
struct CppPreprocessorResult {
    CppIndexUnit receipt; // Immutable dependency receipt; never an indexed symbol unit.
    std::string text;
    Json includes, macros;
};
// Bounded decoder; unavailable/malformed producer results are errors. No tools or files are written.
[[nodiscard]] Result<CppPreprocessorResult> parseCppPreprocessorResult(std::string_view bytes);
// Owns one cancellable helper process on the caller's async context. Caller owns trust admission.
[[nodiscard]] sched::Task<Result<CppPreprocessorResult>>
requestCppPreprocessor(io::IProcessLauncher& launcher, io::AsyncContext context, std::string helper,
                       CppIndexRequest request, IClock::Duration timeout, std::stop_token stop = {});
} // namespace frost::lang
