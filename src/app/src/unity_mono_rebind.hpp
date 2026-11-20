// SPDX-License-Identifier: Apache-2.0
// Ephemeral explicit Unity Mono reload rebind consent; old frames/values are never retained.
#pragma once
#include "frost/app/workbench.hpp"

#include "unity_control.hpp"
namespace frost::app {
struct UnityMonoDebugBinding {
    UnityMonoDebugSelection selection;
    std::string root, instance, session, image;
    std::uint64_t trustRevision = 0, processToken = 0;
};
// GUI-thread policy only. Actual held OS process and fresh PE/PDB/source admission remain required.
[[nodiscard]] inline bool canRebindUnityMono(const UnityMonoDebugBinding& binding, const UnityEditorIdentity& editor,
                                             std::string_view root, bool trusted, std::uint64_t revision) {
    return !binding.selection.player && trusted && revision == binding.trustRevision && root == binding.root &&
           editor.root == binding.root && editor.instance == binding.instance && editor.session != binding.session &&
           editor.reportedPid > 0 && static_cast<std::uint32_t>(editor.reportedPid) == binding.selection.pid &&
           editor.authenticated && !editor.compiling && editor.epoch && editor.epoch->load() == editor.revision;
}
[[nodiscard]] inline bool sameUnityMonoProcess(const UnityMonoDebugBinding& binding, std::uint32_t pid,
                                               std::uint64_t token, std::string_view image) {
    return pid == binding.selection.pid && token == binding.processToken && image == binding.image;
}
} // namespace frost::app
