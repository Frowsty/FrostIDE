// SPDX-License-Identifier: Apache-2.0
// Standalone Qt-free /MT frost-analyzer. Only reads JSON/AST inputs and writes actual reports.
#include "analyzer.hpp"
#include "llvm/ADT/SmallString.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <string_view>
namespace {
// Same staged-layout and environment policy as the reviewed frost-clang host; no PATH probe.
std::string resource(char* argv0) {
    llvm::SmallString<256> bundled(llvm::sys::fs::getMainExecutable(argv0, reinterpret_cast<void*>(&resource)));
    llvm::sys::path::remove_filename(bundled);
    llvm::sys::path::append(bundled, std::string("llvm-") + LLVM_VERSION_STRING, "lib", "clang",
                            std::to_string(LLVM_VERSION_MAJOR));
    if (llvm::sys::fs::is_directory(bundled))
        return bundled.str().str();
    if (const auto* root = std::getenv("FROST_LLVM_ROOT"); root && *root) {
        llvm::SmallString<256> candidate(root);
        llvm::sys::path::append(candidate, "lib", "clang", std::to_string(LLVM_VERSION_MAJOR));
        if (llvm::sys::fs::is_directory(candidate))
            return candidate.str().str();
    }
    if (const auto* root = std::getenv("FROST_SDK"); root && *root) {
        llvm::SmallString<256> candidate(root);
        llvm::sys::path::append(candidate, std::string("llvm-") + LLVM_VERSION_STRING, "lib", "clang",
                                std::to_string(LLVM_VERSION_MAJOR));
        if (llvm::sys::fs::is_directory(candidate))
            return candidate.str().str();
    }
    return {};
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--list-rules") {
        llvm::outs() << llvm::json::Value(llvm::json::Object{{"kind", "frost-analyzer-rules"},
                                                             {"schema", 1},
                                                             {"llvmVersion", LLVM_VERSION_STRING},
                                                             {"rules", frost::clang_engine::analyzerRegistry()}})
                     << '\n';
        return 0;
    }
    if (argc != 2 || (std::string_view(argv[1]) != "--analyze-unit" && std::string_view(argv[1]) != "--sarif")) {
        llvm::errs() << "usage: frost-analyzer --analyze-unit|--sarif|--list-rules; captured command JSON on stdin\n";
        return 1;
    }
    return frost::clang_engine::runAnalyzerUnit(resource(argv[0]), std::string_view(argv[1]) == "--sarif");
}
