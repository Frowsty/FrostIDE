// SPDX-License-Identifier: Apache-2.0
//
// MergeView: the minimal compare/merge view for an external change to a dirty buffer (PLAN.md
// section 7 M1, ADR 0042). Three read-only panes, yours | base | disk, aligned row by row through
// display-map block decorations (nav::MergePanes), the list of changed chunks with per-chunk Accept
// Left (yours) and Accept Right (disk), and the three decisions: Apply (the chunk choices),
// Keep My Version and Take Disk Version, driven by the document's PendingMerge and
// SaveManager::resolveExternalChange.
#pragma once

#include "frost/core/error.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/text/text_shaper.hpp"
#include "frost/ui/nav/merge_layout.hpp"

#include <QDialog>
#include <QString>

#include <array>
#include <cstddef>
#include <optional>
#include <string>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace frost::ui {

/*
 * One merge. Built for a document with a PendingMerge (create() fails with NotFound otherwise).
 * Chunk choices change the merge model in place; Apply needs every conflict resolved. Each decision
 * that succeeds emits resolved() and closes the view; a failed one (the buffer moved on: Busy, the
 * merge was rebuilt) shows the error and rebuilds the panes. The manager and the document outlive
 * the view (the Workbench closes it when the document closes). GUI thread only.
 */
class MergeView final : public QDialog {
    Q_OBJECT

public:
    [[nodiscard]] static Result<MergeView*> create(document::SaveManager& manager, document::FileDocument& document,
                                                   QWidget* parent);

    // The non-stable chunks, in order (indices into the model's chunks).
    [[nodiscard]] const std::vector<std::size_t>& changes() const noexcept { return m_changes; }
    // Takes yours (left) or the disk's (right) side for model chunk `chunk`. Errors: the model's.
    [[nodiscard]] Result<void> acceptLeft(std::size_t chunk);
    [[nodiscard]] Result<void> acceptRight(std::size_t chunk);
    // The model's unresolved conflicts.
    [[nodiscard]] std::uint32_t unresolvedCount() const;

    // The decisions. Errors: resolveExternalChange's (InvalidArgument: a conflict is unresolved;
    // Busy: the buffer changed and the merge was rebuilt).
    [[nodiscard]] Result<void> apply();
    [[nodiscard]] Result<void> keepMine();
    [[nodiscard]] Result<void> takeDisk();

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }
    [[nodiscard]] QPlainTextEdit* pane(nav::MergePane pane) const { return m_panes[static_cast<std::size_t>(pane)]; }
    [[nodiscard]] QListWidget* chunkList() const noexcept { return m_chunks; }
    [[nodiscard]] const nav::MergePanes& mergePanes() const { return *m_layout; }

signals:
    // The merge was settled with `decision`; the view closes.
    void resolved(const QString& path, int decision);

private:
    MergeView(document::SaveManager& manager, document::FileDocument& document, QWidget* parent);
    [[nodiscard]] Result<void> rebuild();
    [[nodiscard]] Result<void> decide(document::MergeDecision decision);
    [[nodiscard]] Result<void> take(std::size_t chunk, bool left);
    void syncScroll(int value);
    void refreshChunkList();
    [[nodiscard]] std::optional<std::size_t> selectedChunk() const;

    document::SaveManager& m_manager;
    document::FileDocument& m_document;
    std::string m_path;
    text::MonospaceTextShaper m_shaper;
    std::optional<nav::MergePanes> m_layout;
    std::vector<std::size_t> m_changes;
    std::array<QPlainTextEdit*, 3> m_panes{};
    QListWidget* m_chunks = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_apply = nullptr;
    bool m_syncing = false;
};

} // namespace frost::ui
