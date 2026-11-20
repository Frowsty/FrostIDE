// SPDX-License-Identifier: Apache-2.0
// Actual Clang file/region emission with bounded diagnostics and optional exact-token fixes.
#include "analyzer.hpp"
#include "semantic_snapshot.hpp"
namespace frost::clang_engine {
AnalyzerResults::AnalyzerResults(clang::ASTContext& c, std::string r, std::set<std::string> rules)
    : context(c), root(std::move(r)), enabled(std::move(rules)) {}
void AnalyzerResults::add(llvm::StringRef id, clang::SourceRange range, llvm::StringRef message,
                          std::optional<std::string> replacement) {
    if (!enabled.contains(id.str()))
        return;
    if (findings.size() >= 8192) {
        if (limitations.empty())
            limitations.push_back(llvm::json::Object{{"reason", "Finding budget exceeds8192; report is incomplete"}});
        return;
    }
    auto& sm = context.getSourceManager();
    if (!range.getBegin().isFileID() || !range.getEnd().isFileID() ||
        sm.getFileID(range.getBegin()) != sm.getFileID(range.getEnd()) || sm.isInSystemHeader(range.getBegin()))
        return;
    const auto path = semantic::absolute(sm.getFilename(range.getBegin()), root);
    if (!(path == root || (llvm::StringRef(path).starts_with(root) && path.size() > root.size() &&
                           llvm::sys::path::is_separator(path[root.size()]))))
        return;
    const auto end = clang::Lexer::getLocForEndOfToken(range.getEnd(), 0, sm, context.getLangOpts());
    if (!end.isFileID())
        return;
    const auto offset = sm.getFileOffset(range.getBegin()), finish = sm.getFileOffset(end);
    if (finish < offset || finish - offset > 8U * 1024U * 1024U)
        return;
    const auto key = id.str() + "|" + path + "|" + std::to_string(offset);
    if (!emitted.insert(key).second)
        return;
    const auto column = [&](clang::SourceLocation location) {
        const auto bytes = sm.getBufferData(sm.getFileID(location));
        const auto at = sm.getFileOffset(location);
        const auto newline = at ? bytes.rfind('\n', at - 1) : llvm::StringRef::npos;
        const auto start = newline == llvm::StringRef::npos ? 0 : newline + 1;
        unsigned count = 1;
        for (auto i = start; i < at; ++i)
            if ((static_cast<unsigned char>(bytes[i]) & 0xc0) != 0x80)
                ++count;
        return count;
    };
    llvm::json::Object finding{{"ruleId", id},
                               {"message", message},
                               {"path", path},
                               {"line", sm.getSpellingLineNumber(range.getBegin())},
                               {"column", column(range.getBegin())},
                               {"endLine", sm.getSpellingLineNumber(end)},
                               {"endColumn", column(end)},
                               {"offset", offset},
                               {"length", finish - offset}};
    if (replacement)
        finding["replacement"] = *replacement;
    findings.push_back(std::move(finding));
}
void AnalyzerResults::limited(const clang::FunctionDecl& function, llvm::StringRef why) {
    if (limitations.size() < 1024)
        limitations.push_back(llvm::json::Object{{"function", function.getQualifiedNameAsString()}, {"reason", why}});
}
} // namespace frost::clang_engine
