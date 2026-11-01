// SPDX-License-Identifier: Apache-2.0
//
// The .theme.json fuzz harness (theme_json_harness.hpp).

#include "theme_json_harness.hpp"

#include "frost/ui/model/theme_blob.hpp"
#include "frost/ui/theme_compiler/theme_compiler.hpp"

#include <algorithm>
#include <tuple>

namespace frost::ui::fuzz {

namespace {

Result<void> checkShape(const CompileResult& result) {
    const bool sorted = std::ranges::is_sorted(result.diagnostics, [](const Diagnostic& a, const Diagnostic& b) {
        return std::tie(a.jsonPath, a.message) < std::tie(b.jsonPath, b.message);
    });
    if (!sorted) {
        return fail(ErrorCode::Internal, "diagnostics are not sorted");
    }
    const bool hasError =
        std::ranges::any_of(result.diagnostics, [](const Diagnostic& d) { return d.severity == Diagnostic::Error; });
    if (hasError == result.ok()) {
        return fail(ErrorCode::Internal, "data is present exactly when there is no error diagnostic, but not here");
    }
    return {};
}

} // namespace

Result<void> runThemeJsonInput(std::span<const std::uint8_t> data) {
    const std::span<const std::byte> json = std::as_bytes(data);
    const CompileResult strict = compileTheme("fuzz", json, CompileOptions{true});
    if (auto shape = checkShape(strict); !shape) {
        return shape;
    }
    const CompileResult lenient = compileTheme("fuzz", json, CompileOptions{false});
    if (auto shape = checkShape(lenient); !shape) {
        return shape;
    }
    if (strict.ok() && !lenient.ok()) {
        return fail(ErrorCode::Internal, "a strict success fails in lenient mode");
    }
    if (!strict.ok()) {
        return {};
    }
    if (!Theme::fromData(*strict.data)) {
        return fail(ErrorCode::Internal, "compiled data fails Theme::fromData");
    }
    const auto decoded = decodeThemeBlob(encodeThemeBlob(*strict.data));
    if (!decoded || decoded->data() != *strict.data) {
        return fail(ErrorCode::Internal, "compiled data does not survive the blob round trip");
    }
    const CompileResult again = compileTheme("fuzz", json, CompileOptions{true});
    if (again.data != strict.data || again.diagnostics != strict.diagnostics) {
        return fail(ErrorCode::Internal, "compiling the same input twice differs");
    }
    return {};
}

} // namespace frost::ui::fuzz
