// SPDX-License-Identifier: Apache-2.0
// Bounded, no-link discovery of a trusted Unity generated-context descriptor.
#pragma once
#include <string>
namespace frost::app {
// Call only after project trust has been established. Returns the generation id when its
// owner, descriptor and solution are regular no-link files; otherwise returns empty.
[[nodiscard]] std::string trustedUnityRevision(const std::string& root);
} // namespace frost::app
