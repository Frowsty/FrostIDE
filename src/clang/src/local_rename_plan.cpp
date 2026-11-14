// SPDX-License-Identifier: Apache-2.0
// REF-4 preparatory local rename plans: canonical AST identity, immutable input snapshots and
// a second semantic parse. No Rewriter, output action, WorkspaceEditApplier or disk application.

#include "local_rename_plan.hpp"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PreprocessingRecord.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace frost::clang_engine {
namespace {

constexpr std::size_t kMainBytes = 8 * 1024 * 1024;
constexpr std::size_t kDependencyBytes = 256 * 1024 * 1024;
constexpr std::size_t kFiles = 4096;
constexpr std::size_t kArguments = 4096;
constexpr std::size_t kArgumentBytes = 1024 * 1024;
constexpr std::size_t kEdits = 100000;
constexpr std::size_t kOutputBytes = 16 * 1024 * 1024;
constexpr std::size_t kDiagnostics = 1024;

// The same checked budget primitive is used for admission and the bounded test diagnostics.
bool addWithin(std::size_t& used, std::size_t amount, std::size_t limit) {
    if (used > limit || amount > limit - used) {
        return false;
    }
    used += amount;
    return true;
}

bool valueFits(std::size_t value, std::size_t limit) {
    std::size_t used = 0;
    return addWithin(used, value, limit);
}

bool slotFits(std::size_t existing, std::size_t limit) { return existing < limit; }

bool jsonCharge(std::size_t& used, std::size_t inputBytes) {
    return used <= kOutputBytes && inputBytes <= (kOutputBytes - used) / 6 &&
           addWithin(used, inputBytes * 6, kOutputBytes);
}

bool dependencyFits(std::size_t files, std::size_t bytes, std::size_t amount) {
    return slotFits(files, kFiles) && addWithin(bytes, amount, kDependencyBytes);
}

std::string sha256(llvm::StringRef bytes) {
    llvm::SHA256 hash;
    hash.update(bytes);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : hash.final()) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

std::string absolute(llvm::StringRef path, llvm::StringRef directory) {
    llvm::SmallString<256> value(path);
    if (!llvm::sys::path::is_absolute(value)) {
        llvm::SmallString<256> base(directory);
        llvm::sys::path::append(base, value);
        value = base;
    }
    llvm::sys::path::remove_dots(value, true);
    llvm::sys::path::native(value);
    return value.str().str();
}

class Diagnostics final : public clang::DiagnosticConsumer {
public:
    std::vector<std::string> messages;
    bool failed = false;
    void HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& diagnostic) override {
        failed |= level >= clang::DiagnosticsEngine::Error;
        if (!slotFits(messages.size(), kDiagnostics)) {
            failed = true;
            return;
        }
        llvm::SmallString<256> message;
        diagnostic.FormatDiagnostic(message);
        if (message.size() > 4096) {
            failed = true;
            messages.emplace_back("diagnostic exceeded4096-byte limit");
        } else {
            messages.push_back(message.str().str());
        }
    }
};

struct FrozenInput {
    llvm::vfs::Status status;
    std::shared_ptr<const std::string> bytes;
};

class FrozenFile final : public llvm::vfs::File {
public:
    explicit FrozenFile(FrozenInput input) : m_input(std::move(input)) {}
    llvm::ErrorOr<llvm::vfs::Status> status() override { return m_input.status; }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(const llvm::Twine& name, std::int64_t, bool,
                                                                 bool) override {
        return llvm::MemoryBuffer::getMemBufferCopy(*m_input.bytes, name.str());
    }
    std::error_code close() override { return {}; }

private:
    FrozenInput m_input;
};

/*
 * Freeze every opened dependency on its first read. Candidate parsing can only reopen that
 * snapshot set, never adopt newly appearing disk bytes. The VFS API has no write operation.
 * File size is admitted before reading; the explicit size bounds the backing buffer request.
 */
class SnapshotFileSystem final : public llvm::vfs::ProxyFileSystem {
public:
    explicit SnapshotFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> base)
        : ProxyFileSystem(std::move(base)) {}
    std::map<std::string, FrozenInput> inputs;
    std::string error;
    bool frozen = false;

    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine& path) override {
        const auto key = keyFor(path);
        if (const auto it = inputs.find(key); it != inputs.end()) {
            return it->second.status;
        }
        if (const auto it = m_status.find(key); it != m_status.end()) {
            return it->second;
        }
        if (frozen || !slotFits(m_status.size(), kFiles * 4) || !addWithin(m_namespaceBytes, key.size(), kMainBytes)) {
            error = "new candidate namespace observation or namespace limit exceeded";
            return std::make_error_code(std::errc::permission_denied);
        }
        auto value = ProxyFileSystem::status(path);
        m_status.emplace(key, value);
        return value;
    }
    bool exists(const llvm::Twine& path) override { return bool(status(path)); }
    std::error_code getRealPath(const llvm::Twine& path, llvm::SmallVectorImpl<char>& output) override {
        const auto key = keyFor(path);
        if (const auto it = m_realPaths.find(key); it != m_realPaths.end()) {
            output.assign(it->second.second.begin(), it->second.second.end());
            return it->second.first;
        }
        if (frozen || !slotFits(m_realPaths.size(), kFiles * 4)) {
            error = "new candidate realpath observation or namespace limit exceeded";
            return std::make_error_code(std::errc::permission_denied);
        }
        const auto code = ProxyFileSystem::getRealPath(path, output);
        if (!addWithin(m_namespaceBytes, key.size(), kMainBytes) ||
            !addWithin(m_namespaceBytes, output.size(), kMainBytes)) {
            error = "namespace path-byte limit exceeded";
            return std::make_error_code(std::errc::file_too_large);
        }
        m_realPaths.emplace(key, std::make_pair(code, std::string(output.begin(), output.end())));
        return code;
    }
    llvm::vfs::directory_iterator dir_begin(const llvm::Twine&, std::error_code& code) override {
        error = "directory enumeration is unsupported by local rename snapshot";
        code = std::make_error_code(std::errc::operation_not_supported);
        return {};
    }
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine& path) override {
        const auto key = keyFor(path);
        if (const auto it = inputs.find(key); it != inputs.end()) {
            return std::make_unique<FrozenFile>(it->second);
        }
        if (frozen) {
            error = "candidate requested a dependency outside the frozen snapshot";
            return std::make_error_code(std::errc::permission_denied);
        }
        const auto observed = status(path);
        if (!observed) {
            return observed.getError();
        }
        auto file = ProxyFileSystem::openFileForRead(path);
        if (!file) {
            return file.getError();
        }
        auto info = (*file)->status();
        if (!info) {
            return info.getError();
        }
        if (info->getUniqueID() != observed->getUniqueID() || info->getSize() != observed->getSize() ||
            info->getLastModificationTime() != observed->getLastModificationTime()) {
            error = "dependency identity changed after namespace observation";
            return std::make_error_code(std::errc::io_error);
        }
        if (!info->isRegularFile() || !dependencyFits(inputs.size(), m_bytes, info->getSize())) {
            error = "dependency resource/type limit exceeded";
            return std::make_error_code(std::errc::file_too_large);
        }
        auto buffer = (*file)->getBuffer(key, static_cast<std::int64_t>(info->getSize()), true, true);
        if (!buffer) {
            return buffer.getError();
        }
        if ((*buffer)->getBufferSize() != info->getSize()) {
            error = "dependency size changed during snapshot admission";
            return std::make_error_code(std::errc::io_error);
        }
        const auto content = (*buffer)->getBuffer();
        const auto extension = llvm::sys::path::extension(key);
        if (content.starts_with("CPCH") || extension == ".pch" || extension == ".gch" || extension == ".pcm") {
            error = "precompiled dependencies are unsupported";
            return std::make_error_code(std::errc::operation_not_supported);
        }
        m_bytes += (*buffer)->getBufferSize();
        FrozenInput input{*info, std::make_shared<const std::string>((*buffer)->getBuffer().str())};
        inputs.emplace(key, input);
        return std::make_unique<FrozenFile>(std::move(input));
    }

private:
    std::size_t m_bytes = 0;
    std::size_t m_namespaceBytes = 0;
    std::map<std::string, llvm::ErrorOr<llvm::vfs::Status>> m_status;
    std::map<std::string, std::pair<std::error_code, std::string>> m_realPaths;
    std::string keyFor(const llvm::Twine& path) {
        // Keep lookup spelling: collapsing symlink/.. could merge distinct native observations.
        llvm::SmallString<256> value(path.str());
        auto cwd = getCurrentWorkingDirectory();
        if (!llvm::sys::path::is_absolute(value) && cwd) {
            llvm::SmallString<256> joined(*cwd);
            llvm::sys::path::append(joined, value);
            value = joined;
        }
        llvm::sys::path::native(value);
        return value.str().str();
    }
};

struct Edit {
    unsigned offset;
    unsigned length;
};

class OffsetMap {
public:
    OffsetMap() = default;
    OffsetMap(const std::vector<Edit>& edits, std::size_t replacementBytes) {
        for (const auto& edit : edits) {
            m_offsets.push_back(edit.offset);
            m_delta.push_back(m_delta.back() + static_cast<std::int64_t>(replacementBytes) - edit.length);
        }
    }
    unsigned operator()(unsigned offset) const {
        const auto before = std::lower_bound(m_offsets.begin(), m_offsets.end(), offset) - m_offsets.begin();
        return static_cast<unsigned>(static_cast<std::int64_t>(offset) + m_delta[static_cast<std::size_t>(before)]);
    }

private:
    std::vector<unsigned> m_offsets;
    std::vector<std::int64_t> m_delta{0};
};

class Selection final : public clang::RecursiveASTVisitor<Selection> {
public:
    Selection(clang::SourceManager& sm, const clang::LangOptions& language, unsigned offset)
        : m_sm(sm), m_language(language), m_offset(offset) {}
    const clang::VarDecl* declaration = nullptr;
    bool ambiguous = false;
    bool VisitVarDecl(clang::VarDecl* declaration) {
        consider(declaration, declaration->getLocation());
        return true;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* reference) {
        if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl())) {
            consider(variable, reference->getLocation());
        }
        return true;
    }

private:
    clang::SourceManager& m_sm;
    const clang::LangOptions& m_language;
    unsigned m_offset;
    void consider(const clang::VarDecl* candidate, clang::SourceLocation location) {
        if (!location.isFileID() || !m_sm.isWrittenInMainFile(location)) {
            return;
        }
        const unsigned start = m_sm.getFileOffset(location);
        const unsigned size = clang::Lexer::MeasureTokenLength(location, m_sm, m_language);
        if (m_offset < start || m_offset - start >= size) {
            return;
        }
        const auto* canonical = candidate->getCanonicalDecl();
        ambiguous |= declaration != nullptr && declaration != canonical;
        declaration = canonical;
    }
};

// Compare referenced canonical declaration anchors, translating main-file offsets through edits.
// This guards both the changed local's references and unedited references against rebinding.
class FunctionFacts final : public clang::RecursiveASTVisitor<FunctionFacts> {
public:
    FunctionFacts(clang::ASTUnit& ast, const clang::VarDecl* target, const OffsetMap& mapping)
        : m_sm(ast.getSourceManager()), m_target(target), m_mapping(mapping) {}
    bool unsupported = false;
    std::set<unsigned> occurrences;
    std::map<unsigned, std::string> references;
    bool VisitDecl(clang::Decl* declaration) {
        unsupported |= declaration->getLocation().isMacroID();
        if (llvm::isa<clang::BindingDecl>(declaration) || llvm::isa<clang::FunctionTemplateDecl>(declaration)) {
            unsupported = true;
        }
        return !unsupported;
    }
    bool VisitStmt(clang::Stmt* statement) {
        unsupported |= statement->getBeginLoc().isMacroID() || statement->getEndLoc().isMacroID() ||
                       llvm::isa<clang::LambdaExpr>(statement) || llvm::isa<clang::CoroutineBodyStmt>(statement);
        return !unsupported;
    }
    bool VisitExpr(clang::Expr* expression) {
        unsupported |= expression->isTypeDependent() || expression->isValueDependent() ||
                       expression->containsUnexpandedParameterPack();
        return !unsupported;
    }
    bool VisitTypeLoc(clang::TypeLoc type) {
        unsupported |= type.getBeginLoc().isMacroID() || type.getEndLoc().isMacroID();
        return !unsupported;
    }
    bool VisitVarDecl(clang::VarDecl* variable) {
        if (variable->getCanonicalDecl() == m_target) {
            recordOccurrence(variable->getLocation());
        }
        return !unsupported;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* reference) {
        if (reference->getDecl()->getCanonicalDecl() == m_target) {
            recordOccurrence(reference->getLocation());
        }
        recordReference(reference->getLocation(), reference->getDecl());
        return !unsupported;
    }
    bool VisitMemberExpr(clang::MemberExpr* reference) {
        recordReference(reference->getMemberLoc(), reference->getMemberDecl());
        return !unsupported;
    }

private:
    clang::SourceManager& m_sm;
    const clang::VarDecl* m_target;
    const OffsetMap& m_mapping;
    void recordOccurrence(clang::SourceLocation location) {
        if (!location.isFileID() || !m_sm.isWrittenInMainFile(location)) {
            unsupported = true;
            return;
        }
        const auto offset = m_sm.getFileOffset(location);
        if (!slotFits(occurrences.size(), kEdits) && !occurrences.contains(offset)) {
            unsupported = true;
        } else {
            occurrences.insert(offset);
        }
    }
    void recordReference(clang::SourceLocation location, const clang::NamedDecl* declaration) {
        if (!location.isFileID() || !m_sm.isWrittenInMainFile(location)) {
            unsupported = true;
            return;
        }
        const auto canonical = declaration->getCanonicalDecl()->getLocation();
        if (!canonical.isFileID()) {
            unsupported = true;
            return;
        }
        unsigned offset = m_sm.getFileOffset(canonical);
        if (m_sm.isWrittenInMainFile(canonical)) {
            offset = m_mapping(offset);
        }
        const std::string identity =
            m_sm.getFilename(canonical).str() + ':' + std::to_string(offset) + ':' + declaration->getDeclKindName();
        const auto at = m_mapping(m_sm.getFileOffset(location));
        if ((!slotFits(references.size(), kEdits) && !references.contains(at)) ||
            (references.contains(at) && references[at] != identity)) {
            unsupported = true;
        } else {
            references[at] = identity;
        }
    }
};

struct Request {
    std::string source, snapshot, directory, driver, name;
    unsigned offset = 0;
    std::vector<std::string> supplied, arguments;
};

// Deliberately small flag allowlist: this preparatory mode does not guess at arbitrary driver
// actions. In particular all forwarding/response/plugin/PCH/module flags are unsupported.
bool argumentsFor(Request& request, llvm::StringRef resource, std::string& error) {
    if (std::ranges::any_of(request.supplied,
                            [](const std::string& argument) { return llvm::StringRef(argument).starts_with("@"); })) {
        error = "response files are unsupported, including compile flag operands";
        return false;
    }
    const auto filename = llvm::sys::path::filename(request.driver);
    const bool cl = filename == "clang-cl" || filename == "clang-cl.exe";
    if (!cl && filename != "clang++" && filename != "clang++.exe") {
        error = "unsupported compile-command driver";
        return false;
    }
    llvm::SmallString<256> sdk(resource);
    for (int i = 0; i != 3; ++i) {
        llvm::sys::path::remove_filename(sdk);
    }
    llvm::sys::path::append(sdk, "bin", filename);
    if (resource.empty() || absolute(sdk, request.directory) != request.driver) {
        error = "compile-command argv0 is not the pinned SDK Clang driver";
        return false;
    }
    request.arguments = {cl ? "--driver-mode=cl" : "--driver-mode=g++"};
    for (std::size_t i = 0; i < request.supplied.size(); ++i) {
        llvm::StringRef arg(request.supplied[i]);
        if (absolute(arg, request.directory) == request.source || arg == "-c" || (cl && arg == "/c")) {
            continue;
        }
        if (arg == "-o") {
            if (++i == request.supplied.size()) {
                error = "missing output argument";
                return false;
            }
            continue;
        }
        if (cl && (arg.starts_with("/Fo") || arg.starts_with("/Fd") || arg.starts_with("/Fe"))) {
            if (arg.size() == 3 && ++i == request.supplied.size()) {
                error = "missing output argument";
                return false;
            }
            continue;
        }
        const bool pair = arg == "-I" || arg == "-isystem" || arg == "-iquote" || arg == "-D" || arg == "-U" ||
                          arg == "-include" || (cl && (arg == "/I" || arg == "/D" || arg == "/U" || arg == "/FI"));
        const bool flag = arg.starts_with("-std=") || arg.starts_with("-I") || arg.starts_with("-D") ||
                          arg.starts_with("-U") || arg == "-fms-extensions" || arg == "-fms-compatibility" ||
                          arg == "-fno-exceptions" || arg == "-fno-rtti" || arg == "-m64" ||
                          (cl && (arg.starts_with("/std:") || arg.starts_with("/I") || arg.starts_with("/D") ||
                                  arg.starts_with("/U") || arg.starts_with("/FI") || arg == "/TP" || arg == "/EHsc" ||
                                  arg == "/nologo" || arg == "/W4" || arg == "/WX"));
        if (!pair && !flag) {
            error = "unsupported compile flag/action: " + arg.str();
            return false;
        }
        request.arguments.push_back(arg.str());
        if (pair) {
            if (++i == request.supplied.size()) {
                error = "missing compile flag operand";
                return false;
            }
            request.arguments.push_back(request.supplied[i]);
        }
    }
    request.arguments.push_back("-resource-dir=" + resource.str());
    request.arguments.push_back("-Xclang");
    request.arguments.push_back("-detailed-preprocessing-record");
    request.arguments.push_back(cl ? "/Zs" : "-fsyntax-only");
    return true;
}

bool readRequest(int argc, char* argv[], Request& request, std::string& error) {
    if (argc < 13 || !valueFits(static_cast<std::size_t>(argc), kArguments)) {
        error = "expected source offset name --snapshot file --working-directory dir --driver argv0 -- flags";
        return false;
    }
    request.source = argv[2];
    request.name = argv[4];
    if (llvm::StringRef(argv[3]).getAsInteger(10, request.offset) || std::string_view(argv[5]) != "--snapshot" ||
        std::string_view(argv[7]) != "--working-directory" || std::string_view(argv[9]) != "--driver" ||
        std::string_view(argv[11]) != "--") {
        error = "invalid request syntax or byte offset";
        return false;
    }
    request.snapshot = argv[6];
    request.directory = argv[8];
    request.driver = argv[10];
    if (!llvm::sys::path::is_absolute(request.source) || !llvm::sys::path::is_absolute(request.snapshot) ||
        !llvm::sys::path::is_absolute(request.directory) || !llvm::sys::path::is_absolute(request.driver)) {
        error = "source/snapshot/directory/driver paths must be absolute";
        return false;
    }
    std::size_t bytes = 0;
    for (int i = 0; i < argc; ++i) {
        const auto size = llvm::StringRef(argv[i]).size();
        if (!addWithin(bytes, size, kArgumentBytes)) {
            error = "argument byte limit exceeded";
            return false;
        }
        if (i >= 12) {
            request.supplied.emplace_back(argv[i]);
        }
    }
    request.source = absolute(request.source, request.directory);
    request.driver = absolute(request.driver, request.directory);
    return true;
}

llvm::json::Array strings(const std::vector<std::string>& values) {
    llvm::json::Array array;
    for (const auto& value : values) {
        array.push_back(value);
    }
    return array;
}

const clang::FunctionDecl* supportedFunction(const clang::VarDecl* target) {
    if (!target || (!llvm::isa<clang::ParmVarDecl>(target) && !target->hasLocalStorage()) || target->isStaticLocal() ||
        target->getStorageClass() == clang::SC_Extern) {
        return nullptr;
    }
    const auto* function = llvm::dyn_cast<clang::FunctionDecl>(target->getDeclContext());
    if (!function || !function->doesThisDeclarationHaveABody() || llvm::isa<clang::CXXMethodDecl>(function) ||
        function->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate || function->isVariadic()) {
        return nullptr;
    }
    return function;
}

bool hasMacroExpansion(clang::ASTUnit& ast, const clang::FunctionDecl& function) {
    auto* record = ast.getPreprocessor().getPreprocessingRecord();
    if (!record) {
        return true;
    }
    auto& sm = ast.getSourceManager();
    const auto first = sm.getFileOffset(function.getBeginLoc());
    const auto last = sm.getFileOffset(function.getEndLoc());
    std::size_t count = 0;
    for (auto* entity : *record) {
        if (++count > kEdits) {
            return true;
        }
        if (llvm::isa<clang::MacroExpansion>(entity)) {
            const auto start = sm.getExpansionLoc(entity->getSourceRange().getBegin());
            const auto end = sm.getExpansionLoc(entity->getSourceRange().getEnd());
            if ((start.isFileID() && sm.isWrittenInMainFile(start) && sm.getFileOffset(start) >= first &&
                 sm.getFileOffset(start) <= last) ||
                (end.isFileID() && sm.isWrittenInMainFile(end) && sm.getFileOffset(end) >= first &&
                 sm.getFileOffset(end) <= last)) {
                return true;
            }
        }
    }
    return false;
}

bool nameConflicts(clang::ASTUnit& ast, const clang::FunctionDecl& function, llvm::StringRef name,
                   const std::set<unsigned>& occurrences) {
    auto& sm = ast.getSourceManager();
    auto& pp = ast.getPreprocessor();
    auto* identifier = pp.getIdentifierInfo(name);
    if (!identifier || identifier->getTokenID() != clang::tok::identifier) {
        return true;
    }
    if (pp.getMacroDefinitionAtLoc(identifier, function.getBeginLoc())) {
        return true;
    }
    const auto start = sm.getLocForStartOfFile(sm.getMainFileID());
    for (auto offset : occurrences) {
        if (pp.getMacroDefinitionAtLoc(identifier, start.getLocWithOffset(static_cast<int>(offset)))) {
            return true;
        }
    }
    auto location = function.getBeginLoc();
    const auto end = sm.getFileOffset(function.getEndLoc());
    while (location.isValid() && sm.getFileOffset(location) <= end) {
        clang::Token token;
        if (clang::Lexer::getRawToken(location, token, sm, ast.getLangOpts(), true)) {
            return true;
        }
        if (clang::Lexer::getSpelling(token, sm, ast.getLangOpts()) == name) {
            return true;
        }
        const auto next = clang::Lexer::getLocForEndOfToken(token.getLocation(), 0, sm, ast.getLangOpts());
        if (!next.isValid() || next == location) {
            return true;
        }
        location = next;
    }
    return false;
}

#ifdef FROST_CLANG_PLAN_TESTS
struct PlanTestHooks {
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> base;
    std::string snapshot;
    std::function<void()> betweenParses;
    std::function<void(clang::ASTUnit&, bool)> observeParse;
};
#endif

llvm::json::Object makePlan(Request& request, llvm::StringRef resource
#ifdef FROST_CLANG_PLAN_TESTS
                            ,
                            PlanTestHooks* hooks = nullptr
#endif
) {
    llvm::json::Object result{{"kind", "local-rename-preview"},
                              {"status", "invalid-input"},
                              {"coverage", "one main snapshot, selected function only"},
                              {"edits", llvm::json::Array{}}};
    auto fail = [&](llvm::StringRef status, const std::string& message) {
        result["status"] = status.str();
        result["diagnostics"] = llvm::json::Array{message};
        return std::move(result);
    };
    std::string error;
    if (!argumentsFor(request, resource, error)) {
        return fail("unsupported", error);
    }
    std::string capturedSnapshot;
#ifdef FROST_CLANG_PLAN_TESTS
    if (hooks) {
        if (!valueFits(hooks->snapshot.size(), kMainBytes)) {
            return fail("invalid-input", "test snapshot exceeds8MiB");
        }
        capturedSnapshot = hooks->snapshot;
    } else
#endif
    {
        llvm::sys::fs::file_status info;
        if (llvm::sys::fs::status(request.snapshot, info) || !llvm::sys::fs::is_regular_file(info) ||
            !valueFits(info.getSize(), kMainBytes)) {
            return fail("invalid-input", "snapshot missing, non-regular or exceeds8MiB");
        }
        auto snapshot = llvm::MemoryBuffer::getFileSlice(request.snapshot, info.getSize(), 0, true);
        if (!snapshot || (*snapshot)->getBufferSize() != info.getSize()) {
            return fail("invalid-input", "snapshot read failed/resource limit");
        }
        capturedSnapshot = (*snapshot)->getBuffer().str();
    }
    const llvm::StringRef bytes(capturedSnapshot);
    const auto* utf8 = reinterpret_cast<const llvm::UTF8*>(bytes.data());
    if (!llvm::isLegalUTF8String(&utf8, utf8 + bytes.size())) {
        return fail("invalid-input", "snapshot must be valid UTF8");
    }
    if (request.offset >= bytes.size()) {
        return fail("invalid-input", "selection byte offset is outside snapshot");
    }
    // Identifier spelling is deliberately ASCII in this first slice; source bytes may be UTF8.
    if (request.name.empty() || request.name.size() > 1024 ||
        !((request.name[0] >= 'a' && request.name[0] <= 'z') || (request.name[0] >= 'A' && request.name[0] <= 'Z') ||
          request.name[0] == '_') ||
        request.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
            std::string::npos) {
        return fail("invalid-input", "new name must be an ordinary ASCII identifier");
    }
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> physical;
#ifdef FROST_CLANG_PLAN_TESTS
    if (hooks) {
        physical = hooks->base;
    } else
#endif
    {
        physical = llvm::vfs::createPhysicalFileSystem().release();
    }
    if (physical->setCurrentWorkingDirectory(request.directory)) {
        return fail("invalid-input", "compile-command working directory is unavailable");
    }
    llvm::IntrusiveRefCntPtr<SnapshotFileSystem> fs(new SnapshotFileSystem(physical));
    const auto recordedArguments = [](const std::vector<std::string>& arguments, llvm::StringRef) { return arguments; };
    Diagnostics originalDiagnostics;
    auto original = clang::tooling::buildASTFromCodeWithArgs(bytes, request.arguments, request.source, request.driver,
                                                             std::make_shared<clang::PCHContainerOperations>(),
                                                             recordedArguments, {}, &originalDiagnostics, fs);
    if (!original || originalDiagnostics.failed || !fs->error.empty()) {
        return fail("parse-error", fs->error.empty() ? "original snapshot did not parse" : fs->error);
    }
#ifdef FROST_CLANG_PLAN_TESTS
    if (hooks && hooks->observeParse) {
        hooks->observeParse(*original, false);
    }
#endif
    auto& sm = original->getSourceManager();
    Selection selection(sm, original->getLangOpts(), request.offset);
    selection.TraverseDecl(original->getASTContext().getTranslationUnitDecl());
    const auto* function = supportedFunction(selection.declaration);
    if (selection.ambiguous || !function || function->getBeginLoc().isMacroID() || function->getEndLoc().isMacroID()) {
        return fail("unsupported", "selection is not a supported ordinary local/parameter declaration");
    }
    // llvm::json::Value(StringRef) borrows storage: own this spelling before the AST dies.
    const std::string oldName = selection.declaration->getName().str();
    const OffsetMap noMapping;
    FunctionFacts facts(*original, selection.declaration, noMapping);
    facts.TraverseDecl(const_cast<clang::FunctionDecl*>(function));
    if (facts.unsupported || facts.occurrences.empty() || hasMacroExpansion(*original, *function)) {
        return fail("unsupported",
                    "selected function contains unsupported macro/dependent/capture syntax or resources");
    }
    if (nameConflicts(*original, *function, request.name, facts.occurrences)) {
        return fail("conflict",
                    "new identifier is reserved, a macro at an edit, or already spelled in selected function");
    }
    std::vector<Edit> edits;
    for (auto offset : facts.occurrences) {
        const auto location = sm.getLocForStartOfFile(sm.getMainFileID()).getLocWithOffset(static_cast<int>(offset));
        const auto length = clang::Lexer::MeasureTokenLength(location, sm, original->getLangOpts());
        if (offset > bytes.size() || length > bytes.size() - offset || bytes.substr(offset, length) != oldName) {
            return fail("conflict", "AST occurrence does not match exact snapshot token bytes");
        }
        edits.push_back({offset, length});
    }
    std::string candidate;
    candidate.reserve(bytes.size());
    std::size_t cursor = 0;
    std::size_t candidateBytes = 0;
    for (const auto& edit : edits) {
        if (!addWithin(candidateBytes, edit.offset - cursor, kMainBytes) ||
            !addWithin(candidateBytes, request.name.size(), kMainBytes)) {
            return fail("invalid-input", "candidate snapshot exceeds8MiB");
        }
        candidate.append(bytes.data() + cursor, edit.offset - cursor);
        candidate += request.name;
        cursor = edit.offset + edit.length;
    }
    if (!addWithin(candidateBytes, bytes.size() - cursor, kMainBytes)) {
        return fail("invalid-input", "candidate snapshot exceeds8MiB");
    }
    candidate.append(bytes.data() + cursor, bytes.size() - cursor);
    const OffsetMap offsetMapping(edits, request.name.size());
#ifdef FROST_CLANG_PLAN_TESTS
    if (hooks && hooks->betweenParses) {
        hooks->betweenParses();
    }
#endif
    fs->frozen = true;
    Diagnostics candidateDiagnostics;
    auto changed = clang::tooling::buildASTFromCodeWithArgs(
        candidate, request.arguments, request.source, request.driver, std::make_shared<clang::PCHContainerOperations>(),
        recordedArguments, {}, &candidateDiagnostics, fs);
    if (!changed || candidateDiagnostics.failed || !fs->error.empty()) {
        return fail("conflict", "candidate failed identical-command/frozen-dependency parse: " + fs->error);
    }
#ifdef FROST_CLANG_PLAN_TESTS
    if (hooks && hooks->observeParse) {
        hooks->observeParse(*changed, true);
    }
#endif
    Selection renamed(changed->getSourceManager(), changed->getLangOpts(),
                      offsetMapping(sm.getFileOffset(selection.declaration->getLocation())));
    renamed.TraverseDecl(changed->getASTContext().getTranslationUnitDecl());
    const auto* newFunction = supportedFunction(renamed.declaration);
    if (!newFunction || renamed.ambiguous) {
        return fail("conflict", "candidate declaration identity is unavailable");
    }
    FunctionFacts before(*original, selection.declaration, offsetMapping);
    before.TraverseDecl(const_cast<clang::FunctionDecl*>(function));
    FunctionFacts after(*changed, renamed.declaration, noMapping);
    after.TraverseDecl(const_cast<clang::FunctionDecl*>(newFunction));
    std::set<unsigned> expected;
    for (auto offset : facts.occurrences) {
        expected.insert(offsetMapping(offset));
    }
    if (before.unsupported || after.unsupported || hasMacroExpansion(*changed, *newFunction) ||
        before.references != after.references || expected != after.occurrences) {
        return fail("conflict", "candidate changed reference binding or occurrence coverage");
    }
    // Bound JSON expansion before constructing repeated edit/path/argument strings. Six bytes
    // per input byte covers JSON control-character escaping; fixed margins cover field syntax.
    std::size_t outputBudget = 4096;
    auto admit = [&](std::size_t size) { return jsonCharge(outputBudget, size); };
    if (!admit(request.source.size() + request.driver.size() + request.directory.size() + resource.size())) {
        return fail("invalid-input", "proposal output resource limit");
    }
    for (const auto* arguments : {&request.supplied, &request.arguments}) {
        for (const auto& argument : *arguments) {
            if (!admit(argument.size() + 16)) {
                return fail("invalid-input", "proposal argument output resource limit");
            }
        }
    }
    for (const auto& [path, input] : fs->inputs) {
        (void)input;
        if (!admit(path.size() + 160)) {
            return fail("invalid-input", "proposal dependency output resource limit");
        }
    }
    for (const auto& message : originalDiagnostics.messages) {
        if (!admit(message.size() + 16)) {
            return fail("invalid-input", "proposal diagnostic output resource limit");
        }
    }
    llvm::json::Array proposed;
    for (const auto& edit : edits) {
        if (!admit(oldName.size() + request.name.size() + 160)) {
            return fail("invalid-input", "proposal edit output resource limit");
        }
        proposed.push_back(llvm::json::Object{{"offset", edit.offset},
                                              {"length", edit.length},
                                              {"expectedText", oldName},
                                              {"replacement", request.name}});
    }
    llvm::json::Array dependencies;
    for (const auto& [path, input] : fs->inputs) {
        dependencies.push_back(
            llvm::json::Object{{"path", path}, {"bytes", input.bytes->size()}, {"sha256", sha256(*input.bytes)}});
    }
    result["status"] = "ready";
    result["source"] = llvm::json::Object{
        {"logicalPath", request.source}, {"snapshotSha256", sha256(bytes)}, {"snapshotBytes", bytes.size()}};
    result["compileCommand"] = llvm::json::Object{{"directory", request.directory},
                                                  {"driver", request.driver},
                                                  {"suppliedArguments", strings(request.supplied)},
                                                  {"parseArguments", strings(request.arguments)},
                                                  {"llvmVersion", "23.1.4"},
                                                  {"resourceDirectory", resource.str()}};
    result["selection"] =
        llvm::json::Object{{"requestedByteOffset", request.offset},
                           {"declarationByteOffset", sm.getFileOffset(selection.declaration->getLocation())},
                           {"oldSpelling", oldName},
                           {"identityKind", "canonical-decl-in-current-AST"}};
    result["dependencies"] = std::move(dependencies);
    result["diagnostics"] = strings(originalDiagnostics.messages);
    result["edits"] = std::move(proposed);
    return result;
}

} // namespace

#ifdef FROST_CLANG_PLAN_TESTS
int checkLocalRenamePlanner(std::string_view resourceDirectory) {
    llvm::json::Array checks;
    bool all = true;
    auto record = [&](llvm::StringRef name, llvm::StringRef kind, bool passed, llvm::json::Object observations) {
        all &= passed;
        checks.push_back(llvm::json::Object{
            {"name", name.str()}, {"kind", kind.str()}, {"passed", passed}, {"observations", std::move(observations)}});
    };
    struct Limit {
        const char* name;
        std::size_t value;
        bool slots;
    };
    for (const auto limit : {Limit{"main-bytes", kMainBytes, false},
                             {"dependency-files", kFiles, true},
                             {"argument-count", kArguments, false},
                             {"argument-bytes", kArgumentBytes, false},
                             {"edit-count", kEdits, true},
                             {"diagnostic-count", kDiagnostics, true},
                             {"namespace-path-bytes", kMainBytes, false},
                             {"namespace-entries", kFiles * 4, true}}) {
        const bool admitted =
            limit.slots ? slotFits(limit.value - 1, limit.value) : valueFits(limit.value, limit.value);
        const bool refused =
            limit.slots ? !slotFits(limit.value, limit.value) : !valueFits(limit.value + 1, limit.value);
        record(limit.name, "pure-production-budget", admitted && refused,
               {{"limit", limit.value}, {"atBoundaryAdmitted", admitted}, {"aboveBoundaryRefused", refused}});
    }
    const bool dependencyAt = dependencyFits(kFiles - 1, kDependencyBytes - 1, 1);
    const bool dependencyOver = !dependencyFits(kFiles - 1, kDependencyBytes, 1);
    record(
        "dependency-bytes", "pure-production-budget", dependencyAt && dependencyOver,
        {{"limit", kDependencyBytes}, {"atBoundaryAdmitted", dependencyAt}, {"aboveBoundaryRefused", dependencyOver}});
    std::size_t output = kOutputBytes - 6;
    const bool outputAt = jsonCharge(output, 1);
    const bool outputOver = !jsonCharge(output, 1);
    const bool overflowRefused = !jsonCharge(output, std::numeric_limits<std::size_t>::max());
    record("output-bytes", "pure-production-budget", outputAt && outputOver && overflowRefused,
           {{"limit", kOutputBytes},
            {"atBoundaryAdmitted", outputAt},
            {"aboveBoundaryRefused", outputOver},
            {"overflowRefused", overflowRefused}});
    const OffsetMap growth({{3, 2}, {9, 2}}, 4);
    const OffsetMap shrink({{3, 2}, {9, 2}}, 1);
    record("offset-prefix-map", "pure-production-offsets",
           growth(0) == 0 && growth(3) == 3 && growth(5) == 7 && growth(9) == 11 && growth(12) == 16 &&
               shrink(5) == 4 && shrink(12) == 10,
           {{"growthAfterTwoEdits", growth(12)}, {"shrinkAfterTwoEdits", shrink(12)}});

    // No physical FS underneath this adapter: mutations are controlled in-memory switches.
    class SwitchFileSystem final : public llvm::vfs::FileSystem {
    public:
        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> current;
        explicit SwitchFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs) : current(std::move(fs)) {}
        llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine& path) override { return current->status(path); }
        llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine& path) override {
            return current->openFileForRead(path);
        }
        llvm::vfs::directory_iterator dir_begin(const llvm::Twine& path, std::error_code& code) override {
            return current->dir_begin(path, code);
        }
        llvm::ErrorOr<std::string> getCurrentWorkingDirectory() const override {
            return current->getCurrentWorkingDirectory();
        }
        std::error_code setCurrentWorkingDirectory(const llvm::Twine& path) override {
            return current->setCurrentWorkingDirectory(path);
        }
        std::error_code getRealPath(const llvm::Twine& path, llvm::SmallVectorImpl<char>& output) override {
            return current->getRealPath(path, output);
        }
    };
#ifdef _WIN32
    const std::string directory = "C:/frost-local-rename-private";
    constexpr auto driverName = "clang-cl.exe";
    const std::vector<std::string> flags{"/std:c++20"};
#else
    const std::string directory = "/frost-local-rename-private";
    constexpr auto driverName = "clang++";
    const std::vector<std::string> flags{"-std=c++20"};
#endif
    llvm::SmallString<256> driver(resourceDirectory);
    for (int i = 0; i != 3; ++i) {
        llvm::sys::path::remove_filename(driver);
    }
    llvm::sys::path::append(driver, "bin", driverName);
    const auto source = absolute("input.cpp", directory);
    const auto optional = absolute("optional.h", directory);
    const std::string conditional =
        "constexpr int setting=\n#if __has_include(\"optional.h\")\n7\n#else\n3\n#endif\n;\n"
        "int f(){int local=1;return local+setting;}\n";
    for (const int scenario : {0, 1, 2}) {
        llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> before(new llvm::vfs::InMemoryFileSystem);
        llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> after(new llvm::vfs::InMemoryFileSystem);
        const std::string contents =
            scenario == 2 ? "#include \"optional.h\"\nint f(){int local=1;return local+setting;}\n" : conditional;
        const bool sourceAdded = before->addFile(source, 0, llvm::MemoryBuffer::getMemBufferCopy(contents)) &&
                                 after->addFile(source, 0, llvm::MemoryBuffer::getMemBufferCopy(contents));
        bool headersAdded = true;
        if (scenario != 0) {
            headersAdded &=
                before->addFile(optional, 0, llvm::MemoryBuffer::getMemBufferCopy("constexpr int setting=7;\n"));
        }
        if (scenario != 1) {
            headersAdded &=
                after->addFile(optional, 0, llvm::MemoryBuffer::getMemBufferCopy("constexpr int setting=9;\n"));
        }
        const bool directoriesSet =
            !before->setCurrentWorkingDirectory(directory) && !after->setCurrentWorkingDirectory(directory);
        llvm::IntrusiveRefCntPtr<SwitchFileSystem> base(new SwitchFileSystem(before));
        int originalSetting = -1;
        int candidateSetting = -1;
        PlanTestHooks hooks{base, contents, [&] { base->current = after; },
                            [&](clang::ASTUnit& ast, bool candidate) {
                                int value = -1;
                                for (auto* declaration : ast.getASTContext().getTranslationUnitDecl()->decls()) {
                                    const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
                                    if (variable && variable->getName() == "setting" && variable->hasInit()) {
                                        clang::Expr::EvalResult evaluated;
                                        if (variable->getInit()->EvaluateAsInt(evaluated, ast.getASTContext())) {
                                            value = static_cast<int>(evaluated.Val.getInt().getSExtValue());
                                        }
                                    }
                                }
                                (candidate ? candidateSetting : originalSetting) = value;
                            }};
        Request request;
        request.source = source;
        request.snapshot = absolute("snapshot.cpp", directory);
        request.directory = directory;
        request.driver = absolute(driver, directory);
        request.name = "renamed";
        request.offset = static_cast<unsigned>(contents.find("local"));
        request.supplied = flags;
        auto result = makePlan(request, resourceDirectory, &hooks);
        const bool ready = result.getString("status") == "ready";
        const bool baseHasHeader = bool(base->status(optional));
        const int expected = scenario == 0 ? 3 : 7;
        const char* name = scenario == 0   ? "negative-lookup-appearance"
                           : scenario == 1 ? "positive-lookup-removal"
                                           : "dependency-content-change";
        record(name, "actual-two-parse-private-VFS",
               sourceAdded && headersAdded && directoriesSet && ready && originalSetting == expected &&
                   candidateSetting == expected && baseHasHeader == (scenario != 1),
               {{"plannerStatus", result.getString("status").value_or("missing").str()},
                {"originalSetting", originalSetting},
                {"candidateSetting", candidateSetting},
                {"mutatedBaseHasHeader", baseHasHeader},
                {"plannerResult", std::move(result)}});
    }
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> privateBase(new llvm::vfs::InMemoryFileSystem);
    const bool privateSourceAdded = privateBase->addFile(source, 0, llvm::MemoryBuffer::getMemBufferCopy("int f();"));
    const bool privateDirectorySet = !privateBase->setCurrentWorkingDirectory(directory);
    SnapshotFileSystem namespaceSnapshot(privateBase);
    const bool initiallyMissing = !namespaceSnapshot.status(optional);
    namespaceSnapshot.frozen = true;
    const bool added = privateBase->addFile(optional, 0, llvm::MemoryBuffer::getMemBufferCopy("new bytes"));
    const bool stillMissing = !namespaceSnapshot.status(optional);
    const bool unknownRefused = !namespaceSnapshot.status(absolute("unobserved.h", directory));
    record("frozen-new-namespace-refusal", "actual-private-VFS",
           privateSourceAdded && privateDirectorySet && initiallyMissing && added && stillMissing && unknownRefused &&
               !namespaceSnapshot.error.empty(),
           {{"initiallyMissing", initiallyMissing},
            {"addedToBase", added},
            {"snapshotStillMissing", stillMissing},
            {"unknownRefused", unknownRefused},
            {"error", namespaceSnapshot.error}});
    llvm::outs() << llvm::json::Value(llvm::json::Object{
                        {"kind", "internal-local-rename-checks"}, {"passed", all}, {"checks", std::move(checks)}})
                 << '\n';
    return all ? 0 : 1;
}
#endif

int runLocalRenamePlan(int argc, char* argv[], std::string_view resourceDirectory) {
    Request request;
    std::string error;
    llvm::json::Object result;
    if (!readRequest(argc, argv, request, error)) {
        result = llvm::json::Object{{"kind", "local-rename-preview"},
                                    {"status", "invalid-input"},
                                    {"diagnostics", llvm::json::Array{error}},
                                    {"edits", llvm::json::Array{}}};
    } else {
        result = makePlan(request, resourceDirectory);
    }
    const bool ready = result.getString("status") == "ready";
    std::string serialized;
    llvm::raw_string_ostream output(serialized);
    output << llvm::json::Value(std::move(result));
    if (serialized.size() > kOutputBytes) {
        llvm::outs() << "{\"kind\":\"local-rename-preview\",\"status\":\"invalid-input\",\"edits\":[],"
                        "\"diagnostics\":[\"serialized proposal exceeds16MiB\"]}\n";
        return 1;
    }
    llvm::outs() << serialized << '\n';
    return ready ? 0 : 1;
}

} // namespace frost::clang_engine
