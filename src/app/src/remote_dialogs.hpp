// SPDX-License-Identifier: Apache-2.0
// Application-owned endpoint and WSL-profile forms; editing never probes or executes a remote tool.
#pragma once
#include "frost/build/wsl_cmake.hpp"

#include <optional>
class QWidget;
namespace frost::app {
[[nodiscard]] Result<std::vector<build::RemoteTarget>> editRemoteTargetForms(QWidget* parent,
                                                                             std::vector<build::RemoteTarget> targets);
struct WslProfileChoice {
    build::WslCMakeProfile profile;
    std::optional<build::WslCMakeOperation> operation;
};
[[nodiscard]] Result<WslProfileChoice> editWslProfileForm(QWidget* parent, build::WslCMakeProfile profile,
                                                          const std::vector<build::RemoteTarget>& targets);
} // namespace frost::app
