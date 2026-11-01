// SPDX-License-Identifier: Apache-2.0
//
// The .theme.json fuzz harness: compiles one input with compileTheme (strict and lenient) and
// checks the compiler's contract. Replayed over tests/fuzz/corpus/theme_json by
// ui_theme_compiler_tests (UiFuzzReplay.ThemeJson). It needs Qt Core, so it stays replay-only
// until the compiler is Qt-free (yyjson) or a Qt-capable fuzz tree exists (ADR 0015).
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <span>

namespace frost::ui::fuzz {

/*
 * Runs one input as theme "fuzz". The compiler must not crash; diagnostics must be sorted with
 * data present exactly when there is no Error; a success must pass Theme::fromData, encode and
 * decode back to the same data, and compile identically a second time; a strict success must
 * also succeed in lenient mode. Returns Internal describing the first broken property.
 */
[[nodiscard]] Result<void> runThemeJsonInput(std::span<const std::uint8_t> data);

} // namespace frost::ui::fuzz
