// SPDX-License-Identifier: Apache-2.0
// FEC-13 derived read-only invocation: narrowly reconstruct proven textual PCH source context.
#pragma once

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <string>
#include <vector>

namespace frost::clang_engine {
struct BindingCompileContext {
    clang::tooling::CompileCommand original, sourceParse;
    std::vector<std::string> analysisArguments, textualHeaders, normalizations;
    std::string error;
};

// No process launches or writes. Keeps every define/include/language/target option in order.
// CL /Yu is reconstructed only when its existing workspace header is the first explicit /FI;
// only /Yu and /Fp transport options are removed. Textual forced includes disable implicit PCH.
// Unknown/missing/mismatching PCH source context is an explicit failure, never an inferred header.
// O(arguments * path lengths); metadata reads only, in the out-of-process compiler helper.
[[nodiscard]] BindingCompileContext bindingSourceCompileContext(const clang::tooling::CompileCommand& command,
                                                                llvm::StringRef workspaceRoot);
[[nodiscard]] llvm::json::Object bindingCompileContextJson(const BindingCompileContext& context);
} // namespace frost::clang_engine
