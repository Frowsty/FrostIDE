// SPDX-License-Identifier: Apache-2.0
// Workbench composition and lifecycle for one project's Git repository.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/vcs/git_service.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace frost::ui {
class GitController;
class BranchStatusWidget;
class CommitToolWindow;
class DiffViewer;
class TrustDialog;
} // namespace frost::ui
namespace frost::app {
class Workbench;
class DocumentEditorView;
class GitIntegration final : public QObject {
public:
    explicit GitIntegration(Workbench& workbench);
    ~GitIntegration() override;
    void openProject(const std::string& root);
    void closeProject();
    void shutdown();
    void attachEditor(DocumentEditorView& view);
    void documentChanged();
    void editorChanged();
    void filesChanged(const io::FileChangeBatch& batch);
    [[nodiscard]] ui::GitController* controller() const noexcept { return m_controller.get(); }

private:
    void installRepository(vcs::RepositoryInfo info, const std::string& executable);
    void clearRepository();
    void poll();
    void refreshMarkers();
    using ContentDone = std::function<void(Result<std::optional<std::string>>)>;
    void readBaseText(const std::string& path, bool index, ContentDone done);
    void showDiff(const std::string& path, bool index = false);
    void requestTrust();
    [[nodiscard]] Result<void> allow(vcs::GitOperationKind kind, const std::vector<std::string>& paths);
    [[nodiscard]] Result<void> allowWrite(std::string_view path) const;
    Workbench& m_workbench;
    std::string m_root;
    std::string m_executable;
    std::string m_configuredGit;
    std::vector<std::string> m_dirtyPaths;
    std::unordered_set<std::string> m_dirtyKeys;
    std::jthread m_discovery;
    std::unique_ptr<ui::GitController> m_controller;
    QPointer<ui::BranchStatusWidget> m_branch;
    QPointer<ui::CommitToolWindow> m_commit;
    QPointer<ui::DiffViewer> m_diff;
    QPointer<ui::TrustDialog> m_trustDialog;
    QTimer m_poll;
    QTimer m_diffRefresh;
    std::string m_diffPath;
    bool m_diffIndex = false;
    bool m_trusted = false;
    bool m_shutdown = false;
    bool m_destructiveOperation = false;
    unsigned m_pollCount = 0;
    std::uint64_t m_diffGeneration = 0;
    std::uint64_t m_projectGeneration = 0;
};
} // namespace frost::app
