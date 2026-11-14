// SPDX-License-Identifier: Apache-2.0
// Shared immutable semantic input snapshots; no disk writes, unchanged R8 capture policy.
#pragma once
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

namespace frost::clang_engine::semantic {
constexpr std::size_t kMainBytes = 8 * 1024 * 1024;
constexpr std::size_t kDependencyBytes = 256 * 1024 * 1024;
constexpr std::size_t kFiles = 4096;
constexpr std::size_t kArguments = 4096;
constexpr std::size_t kArgumentBytes = 1024 * 1024;
constexpr std::size_t kEdits = 100000;
constexpr std::size_t kOutputBytes = 16 * 1024 * 1024;
constexpr std::size_t kDiagnostics = 1024;

// The same checked budget primitive is used for admission and the bounded test diagnostics.
inline bool addWithin(std::size_t& used, std::size_t amount, std::size_t limit) {
    if (used > limit || amount > limit - used) {
        return false;
    }
    used += amount;
    return true;
}

inline bool valueFits(std::size_t value, std::size_t limit) {
    std::size_t used = 0;
    return addWithin(used, value, limit);
}

inline bool slotFits(std::size_t existing, std::size_t limit) { return existing < limit; }

inline bool jsonCharge(std::size_t& used, std::size_t inputBytes) {
    return used <= kOutputBytes && inputBytes <= (kOutputBytes - used) / 6 &&
           addWithin(used, inputBytes * 6, kOutputBytes);
}

inline bool dependencyFits(std::size_t files, std::size_t bytes, std::size_t amount) {
    return slotFits(files, kFiles) && addWithin(bytes, amount, kDependencyBytes);
}

inline std::string sha256(llvm::StringRef bytes) {
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

inline std::string absolute(llvm::StringRef path, llvm::StringRef directory) {
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

} // namespace frost::clang_engine::semantic
