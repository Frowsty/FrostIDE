// SPDX-License-Identifier: Apache-2.0
// REF-4 preparatory local rename plans: canonical AST identity, immutable input snapshots and
// a second semantic parse. No Rewriter, output action, WorkspaceEditApplier or disk application.

#include "local_rename_plan.hpp"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PreprocessingRecord.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "clang/Tooling/Refactoring/Rename/RenamingAction.h"
#include "clang/Tooling/Refactoring/Rename/USRFindingAction.h"
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
#include <iostream>
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
    explicit SnapshotFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> base, std::size_t files = kFiles,
                                std::size_t bytes = kDependencyBytes, std::size_t fileBytes = kDependencyBytes)
        : ProxyFileSystem(std::move(base)), m_fileLimit(files), m_byteLimit(bytes), m_singleFileLimit(fileBytes) {}
    std::vector<std::string> missingInputs() const {
        std::vector<std::string> result;
        for (const auto& [path, status] : m_status)
            if (!status && (status.getError() == std::errc::no_such_file_or_directory ||
                            status.getError() == std::errc::not_a_directory))
                result.push_back(path);
        return result;
    }
    bool capturePositiveProbes() {
        std::vector<std::string> paths;
        for (const auto& [path, observed] : m_status) {
            if (!observed) {
                if (observed.getError() != std::errc::no_such_file_or_directory &&
                    observed.getError() != std::errc::not_a_directory) {
                    error = "A namespace probe has no proven file or absence result";
                    return false;
                }
            } else if (observed->isRegularFile()) {
                if (!inputs.contains(path))
                    paths.push_back(path);
            } else if (!observed->isDirectory()) {
                error = "A namespace probe observed an unsupported non-file/non-directory kind";
                return false;
            }
        }
        for (const auto& path : paths) {
            auto file = openFileForRead(path);
            if (!file) {
                error = "A positive file probe could not be captured: " + path;
                return false;
            }
        }
        return error.empty();
    }
    std::vector<std::string> directoryInputs() const {
        std::vector<std::string> result;
        for (const auto& [path, observed] : m_status)
            if (observed && observed->isDirectory())
                result.push_back(path);
        return result;
    }
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
        if (frozen || !slotFits(m_status.size(), m_fileLimit * 4) ||
            !addWithin(m_namespaceBytes, key.size(), kMainBytes)) {
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
        if (frozen || !slotFits(m_realPaths.size(), m_fileLimit * 4)) {
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
        if (!info->isRegularFile() || info->getSize() > m_singleFileLimit ||
            !(m_fileLimit == kFiles && m_byteLimit == kDependencyBytes
                  ? dependencyFits(inputs.size(), m_bytes, info->getSize())
                  : slotFits(inputs.size(), m_fileLimit) && m_bytes <= m_byteLimit &&
                        info->getSize() <= m_byteLimit - m_bytes)) {
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
    std::size_t m_fileLimit;
    std::size_t m_byteLimit;
    std::size_t m_singleFileLimit;
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

// Project rename streams ASTs one translation unit/configuration at a time. All opened
// inputs are frozen before replacement collection; no compiler output action or writes exist.
class ProjectCommand final : public clang::tooling::CompilationDatabase {
public:
    clang::tooling::CompileCommand command;
    explicit ProjectCommand(clang::tooling::CompileCommand value) : command(std::move(value)) {}
    std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef) const override { return {command}; }
    std::vector<std::string> getAllFiles() const override { return {command.Filename}; }
};

const clang::NamedDecl* projectCanonical(const clang::NamedDecl* declaration) {
    if (const auto* templ = llvm::dyn_cast<clang::ClassTemplateDecl>(declaration))
        declaration = templ->getTemplatedDecl();
    if (const auto* templ = llvm::dyn_cast<clang::FunctionTemplateDecl>(declaration))
        declaration = templ->getTemplatedDecl();
    if (const auto* specialization = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(declaration))
        declaration = specialization->getSpecializedTemplate()->getTemplatedDecl();
    return clang::tooling::getCanonicalSymbolDeclaration(declaration);
}

class ProjectSelection final : public clang::RecursiveASTVisitor<ProjectSelection> {
    clang::ASTContext& context;
    std::string file;
    unsigned offset;

public:
    const clang::NamedDecl* declaration = nullptr;
    bool ambiguous = false;
    ProjectSelection(clang::ASTContext& ctx, std::string path, unsigned position)
        : context(ctx), file(std::move(path)), offset(position) {}
    void consider(const clang::NamedDecl* candidate, clang::SourceLocation location) {
        auto& sm = context.getSourceManager();
        if (!candidate || !location.isFileID() || absolute(sm.getFilename(location), "") != file)
            return;
        const auto start = sm.getFileOffset(location);
        if (offset < start || offset - start >= clang::Lexer::MeasureTokenLength(location, sm, context.getLangOpts()))
            return;
        candidate = projectCanonical(candidate);
        if (!candidate || !candidate->getIdentifier())
            return;
        if (declaration && clang::tooling::getUSRsForDeclaration(declaration, context) !=
                               clang::tooling::getUSRsForDeclaration(candidate, context))
            ambiguous = true;
        declaration = candidate;
    }
    bool VisitNamedDecl(clang::NamedDecl* d) {
        consider(d, d->getLocation());
        return true;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* e) {
        consider(e->getDecl(), e->getLocation());
        return true;
    }
    bool VisitMemberExpr(clang::MemberExpr* e) {
        consider(e->getMemberDecl(), e->getMemberLoc());
        return true;
    }
    bool VisitTagTypeLoc(clang::TagTypeLoc t) {
        consider(t.getDecl(), t.getNameLoc());
        return true;
    }
    bool VisitTypedefTypeLoc(clang::TypedefTypeLoc t) {
        consider(t.getTypedefNameDecl(), t.getNameLoc());
        return true;
    }
    bool VisitTemplateSpecializationTypeLoc(clang::TemplateSpecializationTypeLoc t) {
        consider(t.getTypePtr()->getTemplateName().getAsTemplateDecl(), t.getTemplateNameLoc());
        return true;
    }
};

class ProjectMethods final : public clang::RecursiveASTVisitor<ProjectMethods> {
    clang::ASTContext& context;

public:
    std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> edges;
    explicit ProjectMethods(clang::ASTContext& ctx) : context(ctx) {}
    bool VisitCXXMethodDecl(clang::CXXMethodDecl* method) {
        for (const auto* base : method->overridden_methods())
            edges.emplace_back(clang::tooling::getUSRsForDeclaration(method, context),
                               clang::tooling::getUSRsForDeclaration(base, context));
        return true;
    }
};

class ProjectOccurrences final : public clang::RecursiveASTVisitor<ProjectOccurrences> {
    clang::ASTContext& context;
    clang::Preprocessor* preprocessor;
    const std::set<std::string>& family;
    std::string spelling;

public:
    std::map<std::string, std::set<unsigned>> tokens;
    bool unsupportedMacro = false;
    ProjectOccurrences(clang::ASTContext& ctx, clang::Preprocessor& pp, const std::set<std::string>& ids,
                       std::string name)
        : context(ctx), preprocessor(&pp), family(ids), spelling(std::move(name)) {}
    void consider(const clang::NamedDecl* declaration, clang::SourceLocation location) {
        if (!declaration || location.isInvalid())
            return;
        declaration = projectCanonical(declaration);
        if (!declaration || declaration->isImplicit())
            return;
        auto ids = clang::tooling::getUSRsForDeclaration(declaration, context);
        if (std::ranges::none_of(ids, [&](const auto& id) { return family.contains(id); }))
            return;
        auto& sm = context.getSourceManager();
        while (location.isMacroID()) {
            if (!sm.isMacroArgExpansion(location)) {
                unsupportedMacro = true;
                return;
            }
            const auto name = clang::Lexer::getImmediateMacroName(location, sm, context.getLangOpts());
            auto* identifier = preprocessor->getIdentifierInfo(name);
            if (!identifier) {
                unsupportedMacro = true;
                return;
            }
            const auto definition = preprocessor->getMacroDefinitionAtLoc(identifier, sm.getExpansionLoc(location));
            const auto* macro = definition.getMacroInfo();
            if (!macro) {
                unsupportedMacro = true;
                return;
            }
            for (const auto& token : macro->tokens())
                if (token.is(clang::tok::hash) || token.is(clang::tok::hashhash)) {
                    unsupportedMacro = true;
                    return;
                }
            const auto spelling = sm.getImmediateSpellingLoc(location);
            if (spelling == location) {
                unsupportedMacro = true;
                return;
            }
            location = spelling;
        }
        if (!location.isFileID()) {
            unsupportedMacro = true;
            return;
        }
        bool invalid = false;
        const char* data = sm.getCharacterData(location, &invalid);
        if (invalid) {
            unsupportedMacro = true;
            return;
        }
        if (*data == '~')
            location = location.getLocWithOffset(1);
        if (clang::Lexer::MeasureTokenLength(location, sm, context.getLangOpts()) != spelling.size())
            return;
        tokens[absolute(sm.getFilename(location), "")].insert(sm.getFileOffset(location));
    }
    bool VisitNamedDecl(clang::NamedDecl* d) {
        consider(d, d->getLocation());
        return true;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* e) {
        consider(e->getDecl(), e->getLocation());
        return true;
    }
    bool VisitMemberExpr(clang::MemberExpr* e) {
        consider(e->getMemberDecl(), e->getMemberLoc());
        return true;
    }
    bool VisitTagTypeLoc(clang::TagTypeLoc t) {
        consider(t.getDecl(), t.getNameLoc());
        return true;
    }
    bool VisitTypedefTypeLoc(clang::TypedefTypeLoc t) {
        consider(t.getTypedefNameDecl(), t.getNameLoc());
        return true;
    }
    bool VisitTemplateSpecializationTypeLoc(clang::TemplateSpecializationTypeLoc t) {
        consider(t.getTypePtr()->getTemplateName().getAsTemplateDecl(), t.getTemplateNameLoc());
        return true;
    }
};

std::unique_ptr<clang::ASTUnit> projectAST(const clang::tooling::CompileCommand& command,
                                           llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs,
                                           const std::map<std::string, std::string>& changes,
                                           Diagnostics& diagnostics) {
    ProjectCommand database(command);
    std::vector<std::string> files{command.Filename};
    clang::tooling::ClangTool tool(database, files, std::make_shared<clang::PCHContainerOperations>(), fs);
    tool.setDiagnosticConsumer(&diagnostics);
    tool.appendArgumentsAdjuster(clang::tooling::getClangStripDependencyFileAdjuster());
    for (const auto& [path, bytes] : changes)
        tool.mapVirtualFile(path, bytes);
    std::vector<std::unique_ptr<clang::ASTUnit>> units;
    if (tool.buildASTs(units) || diagnostics.failed || units.size() != 1)
        return {};
    return std::move(units[0]);
}

llvm::json::Object projectPlan(const llvm::json::Object& input, llvm::StringRef resource) {
    llvm::json::Object result{{"kind", "cpp-rename-plan"},
                              {"schemaVersion", 1},
                              {"status", "invalid-input"},
                              {"atomic", true},
                              {"files", llvm::json::Array{}}};
    auto refuse = [&](llvm::StringRef status, const std::string& why) {
        result["status"] = status;
        result["diagnostics"] = llvm::json::Array{why};
        result["files"] = llvm::json::Array{};
        return std::move(result);
    };
    if (resource.empty())
        return refuse("unsupported", "Pinned LLVM resource directory unavailable");
    const auto rootValue = input.getString("projectRoot"), seedValue = input.getString("seedTranslationUnit"),
               anchorValue = input.getString("path"), newValue = input.getString("newName"),
               databaseValue = input.getString("compilationDatabase");
    const auto offset = input.getInteger("offset");
    if (!rootValue || !seedValue || !anchorValue || !newValue || !databaseValue || !offset || *offset < 0 ||
        *offset > std::numeric_limits<unsigned>::max())
        return refuse("invalid-input", "Missing project, compile commands, seed or selection");
    const std::string root = absolute(*rootValue, ""),
                      seed = seedValue->empty() ? std::string{} : absolute(*seedValue, root),
                      anchor = absolute(*anchorValue, root), newName = newValue->str();
    if (root.find('\0') != std::string::npos || seed.find('\0') != std::string::npos ||
        anchor.find('\0') != std::string::npos)
        return refuse("invalid-input", "NUL in project path");
    if (!llvm::sys::path::is_absolute(root) || newName.empty() ||
        !((newName[0] >= 'A' && newName[0] <= 'Z') || (newName[0] >= 'a' && newName[0] <= 'z') || newName[0] == '_') ||
        std::ranges::any_of(newName, [](unsigned char ch) {
            return !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_');
        }))
        return refuse("invalid-input", "New name must be one C++ identifier");
    auto inside = [&](llvm::StringRef path) {
        return path == root ||
               (path.starts_with(root) && path.size() > root.size() &&
                (llvm::sys::path::is_separator(root.back()) || llvm::sys::path::is_separator(path[root.size()])));
    };
    if (!inside(anchor) || (!seed.empty() && !inside(seed)))
        return refuse("unsupported", "Selection must belong to the captured project");
    std::string error;
    auto database = clang::tooling::JSONCompilationDatabase::loadFromBuffer(
        *databaseValue, error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!database)
        return refuse("invalid-input", error);
    auto commands = database->getAllCompileCommands();
    if (commands.empty() || commands.size() > 16384)
        return refuse("unsupported", "Compilation database is empty or exceeds16384 configurations");
    bool hasSeed = seed.empty();
    llvm::json::Array pch;
    for (auto& command : commands) {
        command.Directory = absolute(command.Directory, root);
        command.Filename = absolute(command.Filename, command.Directory);
        if (!inside(command.Filename) || command.CommandLine.empty())
            return refuse("unsupported", "Compilation database contains an unowned source or empty command");
        if (command.Directory.find('\0') != std::string::npos || command.Filename.find('\0') != std::string::npos)
            return refuse("invalid-input", "NUL in compilation path");
        hasSeed |= command.Filename == seed;
        std::vector<std::string> args;
        const auto driverName = llvm::sys::path::filename(command.CommandLine[0]);
        const bool cl =
            driverName == "cl.exe" || driverName == "cl" || driverName == "clang-cl.exe" || driverName == "clang-cl";
        const bool c = driverName == "clang" || driverName == "clang.exe" || driverName == "gcc" ||
                       driverName == "gcc.exe" || driverName == "cc";
        if (!cl && !c && driverName != "clang++" && driverName != "clang++.exe" && driverName != "g++" &&
            driverName != "g++.exe" && driverName != "c++")
            return refuse("unsupported", "Compilation command uses an unsupported compiler wrapper");
        llvm::SmallString<256> driver(resource);
        for (int i = 0; i != 3; ++i)
            llvm::sys::path::remove_filename(driver);
        llvm::sys::path::append(driver, "bin", cl ? "clang-cl" : c ? "clang" : "clang++");
#ifdef _WIN32
        driver += ".exe";
#endif
        args.push_back(driver.str().str());
        for (std::size_t i = 1; i < command.CommandLine.size(); ++i) {
            llvm::StringRef arg(command.CommandLine[i]);
            if (arg.contains('\0'))
                return refuse("invalid-input", "NUL in compiler argument");
            if (arg.starts_with("@") || arg == "-Xclang" || arg.starts_with("-fplugin") ||
                arg.starts_with("-fmodule") || arg.starts_with("/clang:") || arg == "-include-pch")
                return refuse("unsupported", "Response/plugin/module or opaque PCH actions are not admitted");
            if (cl && (arg.starts_with("/Yu") || arg.starts_with("/Yc"))) {
                auto header = arg.drop_front(3).str();
                if (header.empty()) {
                    if (++i == command.CommandLine.size())
                        return refuse("unsupported", "PCH header missing");
                    header = command.CommandLine[i];
                }
                if (header.empty() || header[0] == '/' || header[0] == '-')
                    return refuse("unsupported", "Default opaque PCH context has no source header");
                args.push_back("/FI" + header);
                pch.push_back(llvm::json::Object{
                    {"translationUnit", command.Filename}, {"header", header}, {"mode", "textual-reparse"}});
                continue;
            }
            if (cl && arg.starts_with("/Fp")) {
                if (arg.size() == 3 && ++i == command.CommandLine.size())
                    return refuse("invalid-input", "Missing PCH output operand");
                continue;
            }
            args.push_back(arg.str());
        }
        args.push_back("-resource-dir=" + resource.str());
        args.push_back("-fno-implicit-modules");
        args.push_back("-fno-implicit-module-maps");
        command.CommandLine = std::move(args);
    }
    if (!hasSeed)
        return refuse("invalid-input", "Seed translation unit is absent from complete compilation database");
    std::stable_sort(commands.begin(), commands.end(),
                     [&](const auto& a, const auto& b) { return (a.Filename == seed) > (b.Filename == seed); });
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> physical(new llvm::vfs::OverlayFileSystem(
        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>(llvm::vfs::createPhysicalFileSystem().release())));
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> overlays(new llvm::vfs::InMemoryFileSystem);
    std::set<std::string> overlayPaths;
    if (const auto* supplied = input.getArray("overlays"))
        for (const auto& value : *supplied) {
            const auto* object = value.getAsObject();
            if (!object)
                return refuse("invalid-input", "Invalid overlay");
            auto path = object->getString("path"), bytes = object->getString("contents");
            if (!path || !bytes)
                return refuse("invalid-input", "Invalid overlay snapshot");
            auto name = absolute(*path, root);
            if (!inside(name) || !overlayPaths.insert(name).second || !valueFits(bytes->size(), kMainBytes) ||
                !overlays->addFile(name, 0, llvm::MemoryBuffer::getMemBufferCopy(*bytes)))
                return refuse("invalid-input", "Duplicate, external or oversized overlay");
        }
    physical->pushOverlay(overlays);
    llvm::IntrusiveRefCntPtr<SnapshotFileSystem> frozen(
        new SnapshotFileSystem(physical, 32768, 512ULL * 1024 * 1024, 64ULL * 1024 * 1024));
    std::set<std::string> family;
    std::string oldName;
    std::string chosenSeed = seed;
    std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> methodEdges;
    for (const auto& command : commands) {
        Diagnostics diagnostics;
        auto ast = projectAST(command, frozen, {}, diagnostics);
        if (!ast || !frozen->error.empty())
            return refuse("parse-error",
                          frozen->error.empty() ? "Original compilation configuration failed" : frozen->error);
        if (seed.empty() || command.Filename == seed) {
            ProjectSelection selected(ast->getASTContext(), anchor, static_cast<unsigned>(*offset));
            selected.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
            if (!selected.declaration || selected.ambiguous) {
                if (!seed.empty())
                    return refuse("unsupported", "Selection has no unique canonical AST declaration");
            } else {
                auto ids = clang::tooling::getUSRsForDeclaration(selected.declaration, ast->getASTContext());
                if (ids.empty())
                    return refuse("unsupported", "Selected symbol has no stable USR");
                if (oldName.empty()) {
                    oldName = selected.declaration->getNameAsString();
                    chosenSeed = command.Filename;
                    family.insert(ids.begin(), ids.end());
                } else if (oldName != selected.declaration->getNameAsString() ||
                           std::ranges::none_of(ids, [&](const auto& id) { return family.contains(id); }))
                    return refuse("conflict", "Selected symbol differs between compilation configurations");
            }
        }
        ProjectMethods methods(ast->getASTContext());
        methods.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
        methodEdges.insert(methodEdges.end(), methods.edges.begin(), methods.edges.end());
    }
    if (oldName.empty() || oldName == newName)
        return refuse("invalid-input", "No changed symbol selected");
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [left, right] : methodEdges) {
            if (std::ranges::any_of(left, [&](const auto& id) { return family.contains(id); }) ||
                std::ranges::any_of(right, [&](const auto& id) { return family.contains(id); })) {
                for (const auto& id : left)
                    changed |= family.insert(id).second;
                for (const auto& id : right)
                    changed |= family.insert(id).second;
            }
        }
    }
    std::stable_sort(commands.begin(), commands.end(), [&](const auto& a, const auto& b) {
        return (a.Filename == chosenSeed) > (b.Filename == chosenSeed);
    });
    if (!frozen->capturePositiveProbes())
        return refuse("unsupported", frozen->error);
    frozen->frozen = true;
    std::vector<std::string> newNames{newName}, oldNames{oldName};
    std::vector<std::vector<std::string>> ids{std::vector<std::string>(family.begin(), family.end())};
    std::map<std::string, clang::tooling::Replacements> replacements;
    std::map<std::string, std::set<unsigned>> requiredTokens;
    for (const auto& command : commands) {
        Diagnostics diagnostics;
        auto ast = projectAST(command, frozen, {}, diagnostics);
        if (!ast || !frozen->error.empty())
            return refuse("conflict", "Frozen original configuration cannot be replayed");
        ProjectOccurrences occurrences(ast->getASTContext(), ast->getPreprocessor(), family, oldName);
        occurrences.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
        if (occurrences.unsupportedMacro)
            return refuse("unsupported", "A target occurrence depends on a macro body, token paste or stringization");
        for (const auto& [path, tokens] : occurrences.tokens)
            requiredTokens[path].insert(tokens.begin(), tokens.end());
        clang::tooling::RenamingAction action(newNames, oldNames, ids, replacements);
        auto consumer = action.newASTConsumer();
        consumer->Initialize(ast->getASTContext());
        consumer->HandleTranslationUnit(ast->getASTContext());
    }
    std::map<std::string, std::string> candidates;
    std::size_t editCount = 0;
    for (const auto& [path, edits] : replacements) {
        auto name = absolute(path, root);
        auto captured = frozen->inputs.find(name);
        if (!inside(name) || captured == frozen->inputs.end())
            return refuse("unsupported", "Rename would edit an external/uncaptured file");
        for (const auto& edit : edits) {
            if (++editCount > kEdits || edit.getOffset() > captured->second.bytes->size() ||
                edit.getLength() > captured->second.bytes->size() - edit.getOffset() ||
                captured->second.bytes->substr(edit.getOffset(), edit.getLength()) != oldName)
                return refuse("unsupported", "Macro/token spelling or replacement budget cannot be proved");
        }
        auto after = clang::tooling::applyAllReplacements(*captured->second.bytes, edits);
        if (!after) {
            llvm::consumeError(after.takeError());
            return refuse("conflict", "Overlapping or invalid semantic replacements");
        }
        candidates.emplace(name, std::move(*after));
    }
    if (candidates.empty())
        return refuse("unsupported", "No semantic rename occurrences found");
    for (const auto& [path, tokens] : requiredTokens)
        for (const auto offset : tokens) {
            bool covered = false;
            for (const auto& [file, edits] : replacements)
                if (absolute(file, root) == path)
                    for (const auto& edit : edits)
                        covered |= edit.getOffset() == offset && edit.getLength() == oldName.size();
            if (!covered)
                return refuse("unsupported", "A semantic target occurrence has no proven token replacement");
        }

    auto mappedOffset = [&](unsigned original) {
        std::int64_t delta = 0;
        for (const auto& [path, edits] : replacements)
            if (absolute(path, root) == anchor) {
                for (const auto& edit : edits) {
                    if (original >= edit.getOffset() && original - edit.getOffset() < edit.getLength())
                        return static_cast<unsigned>(static_cast<std::int64_t>(edit.getOffset()) + delta);
                    if (edit.getOffset() + edit.getLength() <= original)
                        delta += static_cast<std::int64_t>(edit.getReplacementText().size()) - edit.getLength();
                }
            }
        return static_cast<unsigned>(static_cast<std::int64_t>(original) + delta);
    };
    std::set<std::string> reverseFamily;
    std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> candidateEdges;
    std::map<std::string, clang::tooling::Replacements> reverse;
    for (const auto& command : commands) {
        Diagnostics diagnostics;
        auto ast = projectAST(command, frozen, candidates, diagnostics);
        if (!ast || !frozen->error.empty())
            return refuse("conflict", "Candidate fails a captured compilation configuration");
        if (command.Filename == chosenSeed) {
            ProjectSelection selected(ast->getASTContext(), anchor, mappedOffset(static_cast<unsigned>(*offset)));
            selected.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
            if (!selected.declaration || selected.ambiguous || selected.declaration->getNameAsString() != newName)
                return refuse("conflict", "Renamed declaration identity was captured or lost");
            auto found = clang::tooling::getUSRsForDeclaration(selected.declaration, ast->getASTContext());
            reverseFamily.insert(found.begin(), found.end());
        }
        ProjectMethods methods(ast->getASTContext());
        methods.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
        candidateEdges.insert(candidateEdges.end(), methods.edges.begin(), methods.edges.end());
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [left, right] : candidateEdges)
            if (std::ranges::any_of(left, [&](const auto& id) { return reverseFamily.contains(id); }) ||
                std::ranges::any_of(right, [&](const auto& id) { return reverseFamily.contains(id); })) {
                for (const auto& id : left)
                    changed |= reverseFamily.insert(id).second;
                for (const auto& id : right)
                    changed |= reverseFamily.insert(id).second;
            }
    }
    std::vector<std::vector<std::string>> reverseIDs{
        std::vector<std::string>(reverseFamily.begin(), reverseFamily.end())};
    for (const auto& command : commands) {
        Diagnostics diagnostics;
        auto ast = projectAST(command, frozen, candidates, diagnostics);
        if (!ast)
            return refuse("conflict", "Candidate semantic replay failed");
        clang::tooling::RenamingAction action(oldNames, newNames, reverseIDs, reverse);
        auto consumer = action.newASTConsumer();
        consumer->Initialize(ast->getASTContext());
        consumer->HandleTranslationUnit(ast->getASTContext());
    }
    // A valid parse alone misses name capture/overload changes. Reverse AST-bound edits must
    // restore every edited buffer exactly, and must not capture any previously unrelated file.
    if (reverse.size() != candidates.size())
        return refuse("conflict", "Rename changes unrelated symbol bindings");
    for (const auto& [path, edits] : reverse) {
        auto name = absolute(path, root);
        auto candidate = candidates.find(name);
        auto original = frozen->inputs.find(name);
        if (candidate == candidates.end() || original == frozen->inputs.end())
            return refuse("conflict", "Candidate captures an unrelated symbol");
        auto restored = clang::tooling::applyAllReplacements(candidate->second, edits);
        if (!restored) {
            llvm::consumeError(restored.takeError());
            return refuse("conflict", "Reverse replacements overlap");
        }
        if (*restored != *original->second.bytes)
            return refuse("conflict", "Semantic round trip detects capture or changed overload resolution");
    }
    llvm::json::Array files, dependencies, missing, directories;
    std::size_t outputBytes = 0;
    auto charge = [&](std::size_t bytes) {
        constexpr std::size_t limit = 64 * 1024 * 1024;
        if (outputBytes > limit || bytes > (limit - outputBytes) / 6)
            return false;
        outputBytes += bytes * 6;
        return true;
    };
    for (const auto& [path, bytes] : candidates) {
        const auto& before = *frozen->inputs.at(path).bytes;
        if (!llvm::json::isUTF8(before) || !llvm::json::isUTF8(bytes) || !charge(before.size()) ||
            !charge(bytes.size()))
            return refuse("unsupported", "UTF-8/output budget exceeded");
        files.push_back(llvm::json::Object{{"path", path},
                                           {"before", before},
                                           {"after", bytes},
                                           {"beforeSha256", sha256(before)},
                                           {"afterSha256", sha256(bytes)},
                                           {"overlay", overlayPaths.contains(path)}});
    }
    for (const auto& [path, entry] : frozen->inputs) {
        if (!charge(path.size() + 160))
            return refuse("unsupported", "Dependency receipt output budget exceeded");
        dependencies.push_back(llvm::json::Object{{"path", path},
                                                  {"sha256", sha256(*entry.bytes)},
                                                  {"bytes", entry.bytes->size()},
                                                  {"overlay", overlayPaths.contains(path)}});
    }
    for (const auto& path : frozen->missingInputs()) {
        if (!charge(path.size()))
            return refuse("unsupported", "Namespace receipt budget exceeded");
        missing.push_back(path);
    }
    for (const auto& path : frozen->directoryInputs()) {
        if (!charge(path.size()))
            return refuse("unsupported", "Directory receipt output budget exceeded");
        directories.push_back(path);
    }
    if (dependencies.size() + missing.size() + directories.size() > 32768)
        return refuse("unsupported", "Transaction read-dependency budget exceeded");
    result["directoryDependencies"] = std::move(directories);
    result["status"] = "ready";
    result["files"] = std::move(files);
    result["dependencies"] = std::move(dependencies);
    result["missingDependencies"] = std::move(missing);
    result["oldName"] = oldName;
    result["newName"] = newName;
    result["translationUnitConfigurations"] = commands.size();
    result["pch"] = std::move(pch);
    result["compilationDatabaseSha256"] = sha256(*databaseValue);
    result["coverage"] = "all supplied compilation-database configurations; frozen dependencies; semantic round trip";
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

int runProjectRenamePlan(std::string_view resourceDirectory) {
    std::string request;
    char chunk[4096];
    while (std::cin) {
        std::cin.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        if (request.size() > 32 * 1024 * 1024 - count) {
            llvm::outs() << "{\"kind\":\"cpp-rename-plan\",\"status\":\"invalid-input\",\"files\":[],\"diagnostics\":["
                            "\"request exceeds32MiB\"]}\n";
            return 1;
        }
        request.append(chunk, count);
    }
    auto parsed = llvm::json::parse(request);
    llvm::json::Object result;
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        result = {{"kind", "cpp-rename-plan"}, {"status", "invalid-input"}, {"files", llvm::json::Array{}}};
    } else if (const auto* object = parsed->getAsObject())
        result = projectPlan(*object, resourceDirectory);
    else
        result = {{"kind", "cpp-rename-plan"}, {"status", "invalid-input"}, {"files", llvm::json::Array{}}};
    const bool ready = result.getString("status") == "ready";
    llvm::outs() << llvm::json::Value(std::move(result)) << '\n';
    return ready ? 0 : 1;
}

} // namespace frost::clang_engine
