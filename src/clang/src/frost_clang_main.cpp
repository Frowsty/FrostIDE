// SPDX-License-Identifier: Apache-2.0
//
// frost-clang (CLANG-0, PLAN.md Appendix C and ADR 0083): unified out-of-process engine binary
// linking the pinned /MT LLVM 23.1.4 SDK libraries.
//
// Modes:
//   1. LSP server mode (default): runs clang::clangd::clangdMain with FrostFeatureModule linked
//      into FeatureModuleRegistry, advertising "frostClangFeatureModule" in initialize's
//      ServerCapabilities and answering custom JSON-RPC requests "frost/clangInfo" and
//      "frost/spikePing".
//   2. LibTooling AST symbol-rewrite mode:
//        frost-clang --rewrite-symbol <old-symbol> <new-symbol> <file> [-- <compile-flags...>]
//      Parses <file> with ClangTool, matches declarations, references, member accesses, constructor
//      initializers and type locations naming <old-symbol> in the main file via ASTMatchers, and
//      rewrites only those C++ tokens via clang::Rewriter (leaving comments and string literals
//      untouched).
//   3. --plan-local-rename: read-only canonical local/parameter rename proposals (REF-4 preparation).

#include "ast_refactoring_plan.hpp"
#include "binding_map.hpp"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/TypeLoc.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "clangd_spi.hpp"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "local_rename_plan.hpp"

#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class FrostFeatureModule final : public clang::clangd::FeatureModule {
public:
    void initializeLSP(clang::clangd::LSPBinder& bind, const llvm::json::Object& /*clientCaps*/,
                       llvm::json::Object& serverCaps) override {
        serverCaps["frostClangFeatureModule"] = llvm::json::Object{
            {"engine", "frost-clang"},
            {"llvmVersion", LLVM_VERSION_STRING},
            {"methods", llvm::json::Array{"frost/clangInfo", "frost/spikePing"}},
        };
        bind.bindRawMethod("frost/clangInfo", this, &FrostFeatureModule::onClangInfo);
        bind.bindRawMethod("frost/spikePing", this, &FrostFeatureModule::onClangInfo);
    }

private:
    void onClangInfo(const llvm::json::Value& params, clang::clangd::Callback<llvm::json::Value> reply) {
        std::string echo = "pong";
        if (const llvm::json::Object* obj = params.getAsObject()) {
            if (const auto ping = obj->getString("ping")) {
                echo = ping->str();
            } else if (const auto query = obj->getString("query")) {
                echo = query->str();
            }
        }
#ifdef _WIN32
        constexpr llvm::StringLiteral kCrtLinkage = "MT";
#else
        constexpr llvm::StringLiteral kCrtLinkage = "static-llvm";
#endif
        llvm::json::Object response{
            {"engine", "frost-clang"},     {"llvmVersion", LLVM_VERSION_STRING}, {"featureModule", true},
            {"moduleName", "frost-clang"}, {"crtLinkage", kCrtLinkage},          {"echo", std::move(echo)},
        };
        reply(llvm::json::Value(std::move(response)));
    }
};

// Static registration into clangDaemon's FeatureModuleRegistry; ClangdMain.cpp populates
// ClangdServer::Options::FeatureModules via FeatureModuleSet::fromRegistry().
const clang::clangd::FeatureModuleRegistry::Add<FrostFeatureModule>
    kRegisterFrostFeatureModule("frost-clang", "FrostIDE CLANG-0 shared LLVM SDK feature module");

class SymbolRewriteMatchCallback final : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
    SymbolRewriteMatchCallback(std::string oldName, std::set<unsigned>& offsets)
        : m_oldName(std::move(oldName)), m_offsets(offsets) {}

    void run(const clang::ast_matchers::MatchFinder::MatchResult& result) override {
        const clang::SourceManager& sm = *result.SourceManager;
        const clang::LangOptions& langOpts = result.Context->getLangOpts();

        if (const auto* decl = result.Nodes.getNodeAs<clang::NamedDecl>("decl")) {
            if (const auto* dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(decl)) {
                recordIfSpelled(sm, langOpts,
                                dtor->getNameInfo().getNamedTypeInfo() != nullptr
                                    ? dtor->getNameInfo().getNamedTypeInfo()->getTypeLoc().getBeginLoc()
                                    : dtor->getLocation());
            } else {
                recordIfSpelled(sm, langOpts, decl->getLocation());
            }
        }
        if (const auto* ref = result.Nodes.getNodeAs<clang::DeclRefExpr>("ref")) {
            recordIfSpelled(sm, langOpts, ref->getNameInfo().getLoc());
        }
        if (const auto* mem = result.Nodes.getNodeAs<clang::MemberExpr>("mem")) {
            recordIfSpelled(sm, langOpts, mem->getMemberLoc());
        }
        if (const auto* init = result.Nodes.getNodeAs<clang::CXXCtorInitializer>("init")) {
            if (init->isWritten()) {
                recordIfSpelled(sm, langOpts, init->getMemberLocation());
            }
        }
        if (const auto* tl = result.Nodes.getNodeAs<clang::TypeLoc>("typeloc")) {
            recordIfSpelled(sm, langOpts, tl->getBeginLoc());
            recordIfSpelled(sm, langOpts, tl->getEndLoc());
        }
    }

private:
    void recordIfSpelled(const clang::SourceManager& sm, const clang::LangOptions& langOpts,
                         clang::SourceLocation rawLoc) {
        if (rawLoc.isInvalid()) {
            return;
        }
        const clang::SourceLocation spellingLoc = sm.getSpellingLoc(rawLoc);
        if (spellingLoc.isInvalid() || !sm.isInMainFile(spellingLoc)) {
            return;
        }
        bool invalid = false;
        const llvm::StringRef token = clang::Lexer::getSourceText(
            clang::CharSourceRange::getTokenRange(spellingLoc, spellingLoc), sm, langOpts, &invalid);
        if (!invalid && token == m_oldName) {
            m_offsets.insert(sm.getFileOffset(spellingLoc));
        }
    }

    std::string m_oldName;
    std::set<unsigned>& m_offsets;
};

class SymbolRewriteAction final : public clang::ASTFrontendAction {
public:
    SymbolRewriteAction(std::string oldName, std::string newName, unsigned& rewriteCount)
        : m_oldName(std::move(oldName)), m_newName(std::move(newName)), m_callback(m_oldName, m_offsets),
          m_rewriteCount(rewriteCount) {
        using namespace clang::ast_matchers;
        m_finder.addMatcher(namedDecl(hasName(m_oldName)).bind("decl"), &m_callback);
        m_finder.addMatcher(declRefExpr(to(namedDecl(hasName(m_oldName)))).bind("ref"), &m_callback);
        m_finder.addMatcher(memberExpr(member(hasName(m_oldName))).bind("mem"), &m_callback);
        m_finder.addMatcher(cxxCtorInitializer(forField(hasName(m_oldName))).bind("init"), &m_callback);
        m_finder.addMatcher(typeLoc(loc(qualType(hasDeclaration(namedDecl(hasName(m_oldName)))))).bind("typeloc"),
                            &m_callback);
    }

    bool BeginSourceFileAction(clang::CompilerInstance& ci) override {
        m_rewriter.setSourceMgr(ci.getSourceManager(), ci.getLangOpts());
        m_offsets.clear();
        return true;
    }

    void EndSourceFileAction() override {
        clang::SourceManager& sm = m_rewriter.getSourceMgr();
        const clang::FileID mainFileId = sm.getMainFileID();
        const clang::SourceLocation fileStart = sm.getLocForStartOfFile(mainFileId);
        for (const unsigned offset : m_offsets) {
            const clang::SourceLocation loc = fileStart.getLocWithOffset(static_cast<int>(offset));
            m_rewriter.ReplaceText(loc, static_cast<unsigned>(m_oldName.size()), m_newName);
            ++m_rewriteCount;
        }
        if (const auto* buffer = m_rewriter.getRewriteBufferFor(mainFileId)) {
            buffer->write(llvm::outs());
        } else {
            bool invalid = false;
            const llvm::StringRef original = sm.getBufferData(mainFileId, &invalid);
            if (!invalid) {
                llvm::outs() << original;
            }
        }
        (void)m_rewriter.overwriteChangedFiles();
    }

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& /*ci*/,
                                                          llvm::StringRef /*inFile*/) override {
        return m_finder.newASTConsumer();
    }

private:
    std::string m_oldName;
    std::string m_newName;
    std::set<unsigned> m_offsets;
    clang::Rewriter m_rewriter;
    clang::ast_matchers::MatchFinder m_finder;
    SymbolRewriteMatchCallback m_callback;
    unsigned& m_rewriteCount;
};

class SymbolRewriteActionFactory final : public clang::tooling::FrontendActionFactory {
public:
    SymbolRewriteActionFactory(std::string oldName, std::string newName, unsigned& rewriteCount)
        : m_oldName(std::move(oldName)), m_newName(std::move(newName)), m_rewriteCount(rewriteCount) {}

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<SymbolRewriteAction>(m_oldName, m_newName, m_rewriteCount);
    }

private:
    std::string m_oldName;
    std::string m_newName;
    unsigned& m_rewriteCount;
};

std::string detectClangResourceDir() {
    if (const char* llvmRoot = std::getenv("FROST_LLVM_ROOT"); llvmRoot != nullptr && *llvmRoot != '\0') {
        llvm::SmallString<256> candidate(llvmRoot);
        llvm::sys::path::append(candidate, "lib", "clang", std::to_string(LLVM_VERSION_MAJOR));
        if (llvm::sys::fs::is_directory(candidate)) {
            return std::string(candidate.str());
        }
    }
    if (const char* sdkRoot = std::getenv("FROST_SDK"); sdkRoot != nullptr && *sdkRoot != '\0') {
        llvm::SmallString<256> candidate(sdkRoot);
        llvm::sys::path::append(candidate, std::string("llvm-") + LLVM_VERSION_STRING, "lib", "clang",
                                std::to_string(LLVM_VERSION_MAJOR));
        if (llvm::sys::fs::is_directory(candidate)) {
            return std::string(candidate.str());
        }
    }
    return {};
}

int runSymbolRewriter(int argc, char* argv[]) {
    // Expected: frost-clang --rewrite-symbol <old> <new> <file> [-- <compile-flags...>]
    if (argc < 5) {
        llvm::errs()
            << "usage: frost-clang --rewrite-symbol <old-symbol> <new-symbol> <file> [-- <compile-flags...>]\n";
        return 2;
    }
    const std::string oldName = argv[2];
    const std::string newName = argv[3];
    if (oldName.empty() || newName.empty()) {
        llvm::errs() << "frost-clang: symbol names must not be empty\n";
        return 2;
    }

    std::string sourceFile;
    int dashDashIndex = argc;
    for (int i = 4; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--") {
            dashDashIndex = i;
            break;
        }
        if (arg == "--in-place") {
            continue;
        }
        if (sourceFile.empty()) {
            sourceFile = std::string(arg);
        }
    }
    if (sourceFile.empty()) {
        llvm::errs() << "frost-clang: missing <file> argument for --rewrite-symbol\n";
        return 2;
    }

    std::vector<std::string> compileFlags;
    for (int i = dashDashIndex + 1; i < argc; ++i) {
        compileFlags.emplace_back(argv[i]);
    }
    if (compileFlags.empty()) {
        compileFlags.emplace_back("-std=c++20");
    }

    llvm::SmallString<256> absSource(sourceFile);
    if (const std::error_code ec = llvm::sys::fs::make_absolute(absSource)) {
        llvm::errs() << "frost-clang: failed to resolve path '" << sourceFile << "': " << ec.message() << "\n";
        return 1;
    }
    const llvm::StringRef directory = llvm::sys::path::parent_path(absSource);
    clang::tooling::FixedCompilationDatabase compilations(directory.empty() ? "." : directory, compileFlags);

    std::vector<std::string> sources{std::string(absSource.str())};
    clang::tooling::ClangTool tool(compilations, sources);
    tool.appendArgumentsAdjuster(clang::tooling::getClangStripOutputAdjuster());
    tool.appendArgumentsAdjuster(clang::tooling::getClangSyntaxOnlyAdjuster());

    const std::string resourceDir = detectClangResourceDir();
    if (!resourceDir.empty()) {
        tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
            ("-resource-dir=" + resourceDir).c_str(), clang::tooling::ArgumentInsertPosition::BEGIN));
    }

    unsigned rewriteCount = 0;
    SymbolRewriteActionFactory factory(oldName, newName, rewriteCount);
    const int status = tool.run(&factory);
    if (status != 0) {
        return status;
    }
    if (rewriteCount == 0) {
        llvm::errs() << "frost-clang: symbol '" << oldName << "' not found in " << absSource << "\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::string_view(argv[1]) == "--binding-map") {
        return frost::clang_engine::runBindingMap(argc, argv, detectClangResourceDir());
    }
#ifdef FROST_CLANG_PLAN_TESTS
    if (argc == 2 && std::string_view(argv[1]) == "--check-local-rename-planner") {
        return frost::clang_engine::checkLocalRenamePlanner(detectClangResourceDir());
    }
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--plan-project-rename") {
        return frost::clang_engine::runProjectRenamePlan(detectClangResourceDir());
    }
    if (argc == 2 && std::string_view(argv[1]) == "--plan-ast-refactoring") {
        return frost::clang_engine::runAstRefactoringPlan(detectClangResourceDir());
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--plan-local-rename") {
        return frost::clang_engine::runLocalRenamePlan(argc, argv, detectClangResourceDir());
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--rewrite-symbol") {
        return runSymbolRewriter(argc, argv);
    }
    return clang::clangd::clangdMain(argc, argv);
}
