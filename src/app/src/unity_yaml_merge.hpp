// SPDX-License-Identifier: Apache-2.0
// Explicit local Unity tool over isolated immutable Git stages. No in-process substitute merge.
#pragma once
#include "frost/io/file_system.hpp"
#include "frost/vcs/git_runner.hpp"

#include <functional>
class QWidget;
namespace frost::app {
struct UnityMergeWork {
    io::IFileSystem& fs;
    io::IProcessLauncher& launcher;
    vcs::GitRunner git;
    std::string root, workTree, gitDir, absolute, relative, program;
    std::function<bool()> admitted; // Immutable trust revision and owner generation admission.
};
struct UnityMergePreview {
    std::string base, ours, theirs, output, diagnostics, index, stages, disk, physicalRoot, physicalTarget,
        physicalProgram;
    io::FileStatus programStatus;
    int exitCode = 0;
    bool clean = false;
};
// Worker only. Requires actual stage1/2/3 and regular UTF8 files (8MiB each), private temporary
// copies, trusted exact tool admission, immutable Git OIDs, bounded source receipts and output.
[[nodiscard]] Result<UnityMergePreview> prepareUnityMerge(const UnityMergeWork&, std::stop_token);
[[nodiscard]] Result<void> validateUnityMergeSources(const UnityMergeWork&, const UnityMergePreview&, std::stop_token);
// Owner-thread modal waits for worker completion even after owner destruction or cancellation.
[[nodiscard]] Result<UnityMergePreview> prepareUnityMergeInBackground(QWidget*, UnityMergeWork);
[[nodiscard]] Result<bool> showUnityMergePreview(QWidget*, const UnityMergePreview&,
                                                 std::function<Result<void>(std::string_view)> apply);
} // namespace frost::app
