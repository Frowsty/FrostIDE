// SPDX-License-Identifier: Apache-2.0
// Admission and staging of the private Windows updater helper.
#pragma once

#include <frost/core/error.hpp>
#include <frost/io/file_system.hpp>

#include <string>
#include <string_view>

namespace frost::update {
inline constexpr std::string_view kPerUserUpdateHelperName = "frost-update-apply.exe";

struct PreparedUpdateHelper {
    std::string executable;
    std::string installReceiptDigest;
};

// Windows only. Workspace must be an exclusively created directory outside installDir.
// Stages only the exact receipt-owned helper and its receipt-owned local runtime files.
[[nodiscard]] Result<PreparedUpdateHelper> stagePerUserUpdateHelper(io::IFileSystem& fs, std::string_view installDir,
                                                                    std::string_view workspace);
} // namespace frost::update
