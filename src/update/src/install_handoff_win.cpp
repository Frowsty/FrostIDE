// SPDX-License-Identifier: Apache-2.0
// Copy only receipt-owned Windows updater binaries into the private handoff directory.
#include "install_ownership_win.hpp"
#include <frost/update/install_handoff.hpp>

#include <array>
#include <utility>
#include <vector>

namespace frost::update {
Result<PreparedUpdateHelper> stagePerUserUpdateHelper(io::IFileSystem& fs, std::string_view installDir,
                                                      std::string_view workspace) {
    const auto receiptPath = io::joinPath(installDir, detail::kInstallReceipt);
    if (!receiptPath)
        return std::unexpected(receiptPath.error());
    auto receipt = fs.readFileSnapshot(*receiptPath, detail::kInstallReceiptLimit);
    if (!receipt)
        return std::unexpected(receipt.error());
    auto ownership = detail::parseInstallOwnership(std::move(*receipt));
    if (!ownership)
        return std::unexpected(ownership.error());
    constexpr std::array names{
        kPerUserUpdateHelperName,
        std::string_view{"msvcp140.dll"},
        std::string_view{"msvcp140_atomic_wait.dll"},
        std::string_view{"vcruntime140.dll"},
        std::string_view{"vcruntime140_1.dll"},
        std::string_view{"msvcp140d.dll"},
        std::string_view{"msvcp140d_atomic_wait.dll"},
        std::string_view{"vcruntime140d.dll"},
        std::string_view{"vcruntime140_1d.dll"},
        std::string_view{"ucrtbased.dll"},
        std::string_view{"clang_rt.asan_dynamic-x86_64.dll"},
    };
    std::vector<std::pair<std::string, std::string>> copies;
    for (const auto name : names) {
        const auto source = io::joinPath(installDir, name);
        const auto destination = io::joinPath(workspace, name);
        if (!source)
            return std::unexpected(source.error());
        if (!destination)
            return std::unexpected(destination.error());
        const auto found = ownership->files.find(name);
        if (found == ownership->files.end()) {
            if (name == kPerUserUpdateHelperName)
                return fail(ErrorCode::Unsupported, "The per-user installation does not own its update helper");
            auto exists = fs.exists(*source);
            if (!exists)
                return std::unexpected(exists.error());
            if (*exists)
                return fail(ErrorCode::PermissionDenied,
                            "The installation contains an unowned updater runtime: " + std::string(name));
            continue;
        }
        auto bytes = fs.readFileSnapshot(*source, 64 * 1024 * 1024);
        if (!bytes)
            return std::unexpected(bytes.error());
        if (detail::ownedFileHash(*bytes) != found->second)
            return fail(ErrorCode::PermissionDenied,
                        "An installed updater file differs from its receipt: " + std::string(name));
        copies.emplace_back(*destination, std::move(*bytes));
    }
    auto executable = copies.front().first;
    for (auto& [destination, bytes] : copies) {
        if (auto saved = fs.writeFileAtomic(destination, bytes); !saved)
            return std::unexpected(saved.error());
    }
    return PreparedUpdateHelper{std::move(executable), detail::ownedFileHash(ownership->receipt)};
}
} // namespace frost::update
