// SPDX-License-Identifier: Apache-2.0
// Frozen actual Git sources and engine tool admission. No QObject crosses into worker calls.
#pragma once
#include "frost/engine/scene_merge.hpp"
#include "frost/vcs/git_runner.hpp"

#include "engine_unit_tests_window.hpp"

#include <functional>
class QWidget;
namespace frost::app {
struct SceneMergeSnapshot {
    engine::SceneMergeRequest request;
    engine::SceneMergeReport report;
    std::string index, stages, target;
    std::optional<std::string> disk;
};
struct SceneMergeWork {
    io::IFileSystem& fs;
    io::IProcessLauncher& launcher;
    vcs::GitRunner runner;
    EngineUnitTestsSource tool;
    std::string workTree, gitDir, absolute, relative;
};
// Worker-only: exact stage OIDs, UTF8 lossless images, source receipts, real engine validation.
[[nodiscard]] Result<SceneMergeSnapshot> prepareSceneMerge(const SceneMergeWork&, const std::stop_token&);
[[nodiscard]] Result<void> validateSceneMergeSources(const SceneMergeWork&, const SceneMergeSnapshot&,
                                                     const std::stop_token&);
[[nodiscard]] Result<SceneMergeSnapshot> refreshSceneMerge(const SceneMergeWork&, SceneMergeSnapshot,
                                                           const std::stop_token&);
// Parentless owned modal waits for its no-write worker even when owner is destroyed/cancelled.
[[nodiscard]] Result<SceneMergeSnapshot>
sceneMergeInBackground(QWidget*, std::function<Result<SceneMergeSnapshot>(std::stop_token)>);
// Returns true only after Apply succeeds. All choices are revalidated by the actual engine.
[[nodiscard]] Result<bool>
showSceneMerge(QWidget*, SceneMergeSnapshot,
               std::function<Result<SceneMergeSnapshot>(SceneMergeSnapshot, std::stop_token)> refresh,
               std::function<Result<void>(const SceneMergeSnapshot&)> apply);
} // namespace frost::app
