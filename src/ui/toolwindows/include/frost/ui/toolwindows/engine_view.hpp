// SPDX-License-Identifier: Apache-2.0
//
// FEC-3 Engine view: immutable CLI-backed project content, with no filesystem scanning.
#pragma once

#include "frost/engine/asset_index.hpp"
#include "frost/engine/project_content.hpp"

#include <QWidget>

#include <optional>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

namespace frost::ui {

// GUI-thread-only; owns the displayed value. No worker, process, engine session or file ownership.
// The application loads content asynchronously and revalidates trust/project identity and asset
// containment before handling openAsset. Counts are from parsed engine listing entries only.
class EngineView final : public QWidget {
    Q_OBJECT
public:
    explicit EngineView(QWidget* parent = nullptr);
    // Replaces every row and clears old navigation metadata. O(assets), GUI thread.
    void setContent(engine::ProjectContent content);
    // Clears old results/navigation immediately and shows a plain-text reason, without zero counts.
    void setUnavailable(const QString& reason);
    void clear();
    void setAssetUsages(const engine::AssetUsages& usages);
    void clearAssetUsages(const QString& reason = {});
    [[nodiscard]] const std::optional<engine::ProjectContent>& content() const noexcept { return m_content; }
    [[nodiscard]] QTreeWidget* tree() const noexcept { return m_tree; }
    [[nodiscard]] QLabel* statusLabel() const noexcept { return m_status; }
    // False for categories, stale/detached items and unavailable content; otherwise emits openAsset.
    bool activate(QTreeWidgetItem* item);

signals:
    void refreshRequested();
    void findAssetUsages(const QString& relativePath, const QString& handle);
    void openAssetUsage(const QString& relativePath);
    // Project-relative engine-reported path and exact decimal handle. Receiver must validate
    // against its current project generation and resolveProjectAsset before opening a file.
    void openAsset(const QString& relativePath, const QString& handle);

private:
    std::optional<engine::ProjectContent> m_content;
    QTreeWidget* m_tree = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_refresh = nullptr;
    QTreeWidget* m_usages = nullptr;
    QLabel* m_usageStatus = nullptr;
};

} // namespace frost::ui
