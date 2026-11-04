// SPDX-License-Identifier: Apache-2.0
//
// DocumentEditorFactory: the shell's editor factory over documents (ADR 0042). It opens every tab
// through the DocumentRegistry (canonical path, SaveManager, the LoadService's large-file guard) and
// wraps the inner editor (the placeholder until the W1 workbench binds the real editor to the
// FileDocument) in a view with banners for the document's state: ReadOnlyReason::LongLine,
// DiskState::OutOfDate (Reload, Overwrite), DiskState::Changed (Compare...) and Deleted.
#pragma once

#include "frost/document/file_document.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/shell/editor_host.hpp"

#include <QFrame>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QHBoxLayout;
class QLabel;

namespace frost::ui {

// What a banner reports.
enum class BannerKind : std::uint8_t { LongLine, OutOfDate, MergePending, Deleted };

// The banner actions the shell implements (the Workbench); each gets the document's canonical path.
struct BannerActions {
    std::function<void(const std::string&)> reload;    // OutOfDate: read the disk again.
    std::function<void(const std::string&)> overwrite; // OutOfDate: save the buffer over the disk.
    std::function<void(const std::string&)> compare;   // MergePending: open the merge view.
};

// The banners `document` needs now, in display order. Pure.
[[nodiscard]] std::vector<BannerKind> bannersFor(const document::FileDocument& document);

/*
 * One banner above an editor: an icon-less strip with the message and link buttons. objectName
 * "editorBanner", property "bannerKind" (the BannerKind as int). GUI thread only.
 */
class EditorBanner final : public QFrame {
    Q_OBJECT

public:
    EditorBanner(BannerKind kind, const QString& message, QWidget* parent = nullptr);

    [[nodiscard]] BannerKind kind() const noexcept { return m_kind; }
    // Adds a link button labelled `label` (objectName "bannerAction") that runs `run`.
    void addAction(const QString& label, std::function<void()> run);
    [[nodiscard]] QLabel* message() const noexcept { return m_message; }

private:
    BannerKind m_kind;
    QLabel* m_message;
    QHBoxLayout* m_layout;
};

class DocumentEditorView;

/*
 * The factory. open() canonicalizes nothing itself (EditorTabs passes canonicalPath()'s result),
 * opens the document through the registry, then the inner editor; a refused document (the
 * large-file guard's OutOfRange) fails before the inner factory runs. The registry and the inner
 * factory outlive the factory and its views. GUI thread only.
 */
class DocumentEditorFactory final : public IEditorFactory {
public:
    DocumentEditorFactory(IEditorFactory& inner, nav::DocumentRegistry& documents) noexcept;
    ~DocumentEditorFactory();
    DocumentEditorFactory(const DocumentEditorFactory&) = delete ("views refer to their factory");
    DocumentEditorFactory& operator=(const DocumentEditorFactory&) = delete ("views refer to their factory");

    [[nodiscard]] Result<std::unique_ptr<IEditorView>> open(const std::string& path, IEditorHost& host) override;
    [[nodiscard]] std::string canonicalPath(const std::string& path) const override;

    void setActions(BannerActions actions) { m_actions = std::move(actions); }
    // Rebuilds the banners of every open view of `path` (canonical) from its document's state.
    void refreshBanners(const std::string& path);
    // Updates the canonical path of every open view of `oldPath` to `newPath` and refreshes banners.
    void renamePath(const std::string& oldPath, const std::string& newPath);
    // Updates the 1-based bookmarked lines on every open view of `path`.
    void setBookmarks(const std::string& path, std::span<const int> oneBasedLines);
    // The banners the view of `path` shows now (empty when it is not open).
    [[nodiscard]] std::vector<EditorBanner*> banners(const std::string& path) const;
    [[nodiscard]] nav::DocumentRegistry& documents() const noexcept { return *m_documents; }

private:
    friend class DocumentEditorView;
    void attach(DocumentEditorView* view);
    void detach(DocumentEditorView* view);

    IEditorFactory* m_inner;
    nav::DocumentRegistry* m_documents;
    BannerActions m_actions;
    std::vector<DocumentEditorView*> m_views;
};

} // namespace frost::ui
