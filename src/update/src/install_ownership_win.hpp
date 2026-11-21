// SPDX-License-Identifier: Apache-2.0
// The local per-user install receipt consumed by tools/install_user.py.
#pragma once

#include <frost/io/path.hpp>
#include <frost/update/safe_updater.hpp>
#include <frost/update/verification.hpp>

#include <array>
#include <cstdlib>
#include <map>
#include <memory>
#include <string_view>

#include <sodium.h>
#include <yyjson.h>

namespace frost::update::detail {
inline constexpr std::string_view kInstallReceipt = ".frost-install.json";
inline constexpr std::string_view kReleaseFloor = "installed-release.json";
inline constexpr std::size_t kInstallReceiptLimit = 4 * 1024 * 1024;
inline constexpr std::size_t kInstallEntryLimit = 32768;
using OwnedFiles = std::map<std::string, std::string, std::less<>>;
using InstallDocument = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;

struct InstallOwnership {
    std::string receipt;
    OwnedFiles files;
};

inline std::string_view receiptString(yyjson_val* value) noexcept {
    return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view{};
}

inline bool validOwnedName(std::string_view name) {
    if (name.empty() || name.find_first_of("\\:") != name.npos || name.find('\0') != name.npos)
        return false;
    while (true) {
        const auto slash = name.find('/');
        const auto part = name.substr(0, slash);
        if (part.empty() || part == "." || part == ".." || part == kInstallReceipt)
            return false;
        if (slash == name.npos)
            return true;
        name.remove_prefix(slash + 1);
    }
}

inline bool completeWindowsPayload(const OwnedFiles& files) {
    return files.contains("frost.exe") && files.contains("frost_app.dll") && files.contains("LICENSE") &&
           files.contains("NOTICE");
}

inline std::string ownedFileHash(std::string_view bytes) {
    Sha256 hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    std::array<char, 2 * crypto_hash_sha256_BYTES + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    return std::string(hex.data(), hex.size() - 1);
}

inline Result<InstallOwnership> parseInstallOwnership(std::string receipt) {
    if (receipt.size() > kInstallReceiptLimit)
        return fail(ErrorCode::Corrupt, "The per-user install receipt is too large");
    InstallDocument document(yyjson_read(receipt.data(), receipt.size(), 0), yyjson_doc_free);
    auto* root = yyjson_doc_get_root(document.get());
    auto* fields = yyjson_obj_get(root, "files");
    auto* association = yyjson_obj_get(root, "associationReceipt");
    auto* associated = yyjson_obj_get(root, "associated");
    auto* schema = yyjson_obj_get(root, "schema");
    if (!yyjson_is_obj(root) || !yyjson_is_uint(schema) || yyjson_get_uint(schema) != 1)
        return fail(ErrorCode::Corrupt, "Invalid per-user install receipt schema");
    if (!yyjson_is_obj(fields) || yyjson_obj_size(fields) > kInstallEntryLimit || !yyjson_is_bool(associated) ||
        (association && (!yyjson_get_bool(associated) || !yyjson_is_obj(association))) ||
        yyjson_obj_size(root) != (association ? 4U : 3U))
        return fail(ErrorCode::Corrupt, "Invalid per-user install receipt");
    InstallOwnership ownership{.receipt = std::move(receipt), .files = {}};
    auto cursor = yyjson_obj_iter_with(fields);
    while (auto* key = yyjson_obj_iter_next(&cursor)) {
        const auto name = receiptString(key);
        const auto hex = receiptString(yyjson_obj_iter_get_val(key));
        if (!validOwnedName(name) || hex.size() != 64 ||
            hex.find_first_not_of("0123456789abcdef") != hex.npos ||
            !ownership.files.emplace(name, hex).second)
            return fail(ErrorCode::Corrupt, "Invalid path or hash in the per-user install receipt");
    }
    if (!completeWindowsPayload(ownership.files))
        return fail(ErrorCode::Corrupt, "Incomplete per-user install receipt");
    return ownership;
}

inline Result<void> hashInstallTree(io::IFileSystem& fs, std::string_view root, std::string_view parent,
                                    std::size_t depth, std::size_t& remaining, OwnedFiles& result) {
    auto directory = parent.empty() ? Result<std::string>(std::string(root)) : io::joinPath(root, parent);
    if (!directory)
        return std::unexpected(directory.error());
    auto entries = fs.listDirectoryWithDiagnostics(*directory);
    if (!entries)
        return std::unexpected(entries.error());
    if (!entries->issues.empty())
        return fail(ErrorCode::Unsupported, "The per-user install tree contains unsupported names");
    if (depth == 64 || entries->entries.size() > remaining)
        return fail(ErrorCode::OutOfRange, "The per-user install tree exceeds its receipt budget");
    for (const auto& entry : entries->entries) {
        if (parent.empty() && entry.name == kInstallReceipt && entry.type == io::FileType::Regular)
            continue;
        std::string name = parent.empty() ? entry.name : std::string(parent) + "/" + entry.name;
        if (!validOwnedName(name) || remaining == 0)
            return fail(ErrorCode::Corrupt, "Invalid path or entry count in the per-user install tree");
        --remaining;
        if (entry.type == io::FileType::Directory) {
            const auto priorSize = result.size();
            if (auto scan = hashInstallTree(fs, root, name, depth + 1, remaining, result); !scan)
                return scan;
            if (result.size() == priorSize)
                return fail(ErrorCode::Busy, "The per-user install tree contains an empty directory: " + name);
            continue;
        }
        if (entry.type != io::FileType::Regular)
            return fail(ErrorCode::Unsupported, "The per-user install tree contains a link or unsupported entry: " + name);
        auto path = io::joinPath(root, name);
        if (!path)
            return std::unexpected(path.error());
        auto bytes = fs.readFile(*path);
        if (!bytes)
            return std::unexpected(bytes.error());
        result.emplace(std::move(name), ownedFileHash(*bytes));
    }
    return {};
}

inline Result<OwnedFiles> hashInstallTree(io::IFileSystem& fs, std::string_view root) {
    OwnedFiles files;
    std::size_t remaining = kInstallEntryLimit;
    if (auto scan = hashInstallTree(fs, root, {}, 0, remaining, files); !scan)
        return std::unexpected(scan.error());
    return files;
}

inline Result<void> validateInstallOwnership(io::IFileSystem& fs, std::string_view root,
                                             const InstallOwnership& ownership) {
    auto path = io::joinPath(root, kInstallReceipt);
    if (!path)
        return std::unexpected(path.error());
    auto receipt = fs.readFileSnapshot(*path, kInstallReceiptLimit);
    if (!receipt)
        return std::unexpected(receipt.error());
    if (*receipt != ownership.receipt)
        return fail(ErrorCode::Busy, "The per-user install receipt changed during the update");
    auto actual = hashInstallTree(fs, root);
    if (!actual)
        return std::unexpected(actual.error());
    if (*actual != ownership.files)
        return fail(ErrorCode::Busy, "The per-user install contains modified or foreign files. The update made no changes.");
    return {};
}

inline Result<InstallOwnership> writeInstallOwnership(io::IFileSystem& fs, std::string_view stage,
                                                       const InstallOwnership& previous, std::string_view newFloor) {
    auto path = io::joinPath(stage, kInstallReceipt);
    if (!path)
        return std::unexpected(path.error());
    auto exists = fs.exists(*path);
    if (!exists)
        return std::unexpected(exists.error());
    if (*exists)
        return fail(ErrorCode::Corrupt, "The update payload contains an installer receipt");
    auto files = hashInstallTree(fs, stage);
    if (!files)
        return std::unexpected(files.error());
    if (!completeWindowsPayload(*files) || files->contains(kReleaseFloor))
        return fail(ErrorCode::Corrupt, "The update is not a complete ordinary Windows payload");
    files->emplace(std::string(kReleaseFloor), ownedFileHash(newFloor));

    InstallDocument original(yyjson_read(previous.receipt.data(), previous.receipt.size(), 0), yyjson_doc_free);
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> document(yyjson_doc_mut_copy(original.get(), nullptr),
                                                                          yyjson_mut_doc_free);
    auto* root = yyjson_mut_doc_get_root(document.get());
    auto* owned = yyjson_mut_obj(document.get());
    if (!owned || !yyjson_mut_obj_put(root, yyjson_mut_str(document.get(), "files"), owned))
        return fail(ErrorCode::IoError, "Could not create an updated per-user install receipt");
    for (const auto& [name, hash] : *files) {
        if (!yyjson_mut_obj_add(owned, yyjson_mut_strncpy(document.get(), name.data(), name.size()),
                               yyjson_mut_strncpy(document.get(), hash.data(), hash.size())))
            return fail(ErrorCode::IoError, "Could not record an owned update file");
    }
    std::size_t len = 0;
    std::unique_ptr<char, decltype(&std::free)> encoded(yyjson_mut_write(document.get(), 0, &len), std::free);
    if (!encoded || len > kInstallReceiptLimit)
        return fail(ErrorCode::IoError, "Could not encode the per-user install receipt");
    auto floor = io::joinPath(stage, kReleaseFloor);
    if (!floor)
        return std::unexpected(floor.error());
    if (auto written = fs.writeFileAtomic(*floor, newFloor); !written)
        return std::unexpected(written.error());
    std::string receipt(encoded.get(), len);
    if (auto written = fs.writeFileAtomic(*path, receipt); !written)
        return std::unexpected(written.error());
    return InstallOwnership{.receipt = std::move(receipt), .files = std::move(*files)};
}
} // namespace frost::update::detail
