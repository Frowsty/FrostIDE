// SPDX-License-Identifier: Apache-2.0
//
// The theme blob fuzz harness (theme_blob_harness.hpp).

#include "theme_blob_harness.hpp"

#include "frost/ui/model/theme_blob.hpp"

#include <algorithm>
#include <format>

namespace frost::ui::fuzz {

Result<void> runThemeBlobInput(std::span<const std::uint8_t> data) {
    const std::span<const std::byte> bytes = std::as_bytes(data);
    auto theme = decodeThemeBlob(bytes);
    if (!theme) {
        const ErrorCode code = theme.error().code();
        if (code != ErrorCode::Corrupt && code != ErrorCode::Unsupported) {
            return fail(ErrorCode::Internal, std::format("decode failed with {}", toString(code)));
        }
        return {};
    }
    if (!Theme::fromData(theme->data())) {
        return fail(ErrorCode::Internal, "a decoded theme fails Theme::fromData");
    }
    const std::vector<std::byte> again = encodeThemeBlob(theme->data());
    if (!std::ranges::equal(again, bytes)) {
        return fail(ErrorCode::Internal, "an accepted blob does not re-encode to the same bytes");
    }
    return {};
}

} // namespace frost::ui::fuzz
