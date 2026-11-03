// SPDX-License-Identifier: Apache-2.0
// Shared per-user release floor, separate from editable application/project settings.
#pragma once
#include <QString>
#include <frost/update/manifest.hpp>

namespace frost::app {
// The directory must identify one local per-user state directory shared by all
// application instances/channels. Blocking bounded I/O; no network or dialogs.
// Cooperative writers serialize through QLockFile. Same-user hostile mutation and
// power-loss durability are outside this contract. Never silently resets bad state.
[[nodiscard]] Result<update::ReleaseFloor> readUpdateFloor(const QString& directory);
// Reloads the floor, verifies exact signed bytes under the lock, and atomically
// publishes the new floor before returning a parsed Release. Exact retries avoid
// rewriting. policy's accepted fields are replaced with the stored floor.
[[nodiscard]] Result<update::Release> acceptUpdate(const QString& directory,
                                                   const update::AuthenticatedRelease& envelope,
                                                   const update::PublicKey& key, update::ReleasePolicy policy);
} // namespace frost::app
