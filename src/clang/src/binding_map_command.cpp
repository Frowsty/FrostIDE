// SPDX-License-Identifier: Apache-2.0
// FEC-13 source parsing preserves explicit compilation semantics while rejecting opaque PCH provenance.
#include "binding_map_command.hpp"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <optional>
#include <utility>

namespace frost::clang_engine {
namespace {
bool same(llvm::StringRef a, llvm::StringRef b) {
#ifdef _WIN32
    return a.equals_insensitive(b);
#else
    return a == b;
#endif
}
std::optional<std::string> sourceHeader(const clang::tooling::CompileCommand& command, llvm::StringRef path,
                                        llvm::StringRef root) {
    if (path.empty())
        return std::nullopt;
    llvm::SmallString<256> input(path);
    if (!llvm::sys::path::is_absolute(input)) {
        input = command.Directory;
        llvm::sys::path::append(input, path);
    }
    llvm::SmallString<256> actual;
    llvm::sys::fs::file_status status;
    if (llvm::sys::fs::real_path(input, actual) || llvm::sys::fs::status(actual, status) ||
        !llvm::sys::fs::is_regular_file(status))
        return std::nullopt;
    llvm::sys::path::native(actual, llvm::sys::path::Style::posix);
    if (actual.size() <= root.size() || !same(llvm::StringRef(actual).take_front(root.size()), root) ||
        (!root.ends_with('/') && actual[root.size()] != '/'))
        return std::nullopt;
    return actual.str().str();
}
llvm::json::Array array(const std::vector<std::string>& strings) {
    llvm::json::Array result;
    for (const auto& value : strings)
        result.push_back(value);
    return result;
}
} // namespace

BindingCompileContext bindingSourceCompileContext(const clang::tooling::CompileCommand& command, llvm::StringRef root) {
    BindingCompileContext result;
    result.original = result.sourceParse = command;
    if (command.CommandLine.empty()) {
        result.error = "compile-command-empty";
        return result;
    }
    const auto driver = llvm::sys::path::filename(command.CommandLine.front());
    bool cl = driver.equals_insensitive("cl.exe") || driver.equals_insensitive("cl") ||
              driver.equals_insensitive("clang-cl.exe") || driver.equals_insensitive("clang-cl");
    for (llvm::StringRef arg : command.CommandLine)
        if (arg.consume_front("--driver-mode="))
            cl = arg == "cl";
    std::optional<std::string> usedHeader;
    std::vector<std::string> forced;
    bool unknownForced = false, explicitSerialized = false;
    unsigned yu = 0;
    for (std::size_t i = 1; i < command.CommandLine.size(); ++i) {
        llvm::StringRef arg(command.CommandLine[i]);
        if (arg.starts_with("-include-pch"))
            explicitSerialized = true;
        if (cl && (arg.starts_with("/Yu") || arg.starts_with("-Yu"))) {
            ++yu;
            usedHeader = sourceHeader(command, arg.drop_front(3), root);
        } else if ((cl && (arg.starts_with("/FI") || arg.starts_with("-FI"))) || (!cl && arg == "-include")) {
            auto value = cl ? arg.drop_front(3) : llvm::StringRef{};
            if (value.empty() && i + 1 < command.CommandLine.size())
                value = command.CommandLine[++i];
            const auto header = sourceHeader(command, value, root);
            if (!header)
                unknownForced = true;
            else
                forced.push_back(*header);
        }
    }
    if (yu && (yu != 1 || !usedHeader || unknownForced || forced.empty() || !same(*usedHeader, forced.front()) ||
               explicitSerialized)) {
        result.error = "unproven-msvc-pch-source-context";
        return result;
    }
    if (yu) {
        result.sourceParse.CommandLine.clear();
        for (const auto& value : command.CommandLine) {
            const llvm::StringRef arg(value);
            if (arg.starts_with("/Yu") || arg.starts_with("-Yu") || arg.starts_with("/Fp") || arg.starts_with("-Fp")) {
                if (arg.size() == 3) {
                    result.error = "unproven-msvc-pch-source-context";
                    return result;
                }
                continue;
            }
            result.sourceParse.CommandLine.push_back(value);
        }
        result.normalizations.push_back("msvc-yu-fp-transport-to-existing-first-fi-source");
    }
    if (!forced.empty() && !unknownForced && !explicitSerialized) {
        // Pinned LLVM's driver documents -ignore-pch: source headers stay explicitly included,
        // but no implicit neighboring GCC/Clang cache is consumed. /clang: forwards it in CL mode.
        result.sourceParse.CommandLine.push_back(cl ? "/clang:-ignore-pch" : "-ignore-pch");
        result.textualHeaders = std::move(forced);
        result.normalizations.push_back("textual-forced-include-disable-implicit-pch");
    }
    return result;
}

llvm::json::Object bindingCompileContextJson(const BindingCompileContext& context) {
    return {{"directory", context.original.Directory},
            {"source", context.original.Filename},
            {"originalArguments", array(context.original.CommandLine)},
            {"analysisArguments", array(context.analysisArguments)},
            {"normalizations", array(context.normalizations)},
            {"textualPchHeaders", array(context.textualHeaders)},
            {"error", context.error}};
}
} // namespace frost::clang_engine
