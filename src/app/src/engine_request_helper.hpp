// SPDX-License-Identifier: Apache-2.0
// Helper entry points for trusted FrostEngine automation requests.
#pragma once
#include "frost/core/error.hpp"
#include "frost/lang/engine_automation.hpp"
#include "frost/lang/json.hpp"
namespace frost::app {
// Executes one bounded-by-parent automation request in the isolated helper process.
[[nodiscard]] Result<lang::Json> executeEngineHelper(const lang::Json& input);
// Explicit legacy/injected transport for deterministic discovery tests. Production uses the
// no-transport-argument HMAC session entry point; this seam still requires the schema policy.
[[nodiscard]] Result<lang::Json> executeEngineHelper(const lang::Json& input, const lang::EngineConnect& connect);
} // namespace frost::app
