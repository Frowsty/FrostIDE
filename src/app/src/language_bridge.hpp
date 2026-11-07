// SPDX-License-Identifier: Apache-2.0
//
// LanguageBridge: the composition root's side of the language features (ADR 0041, ADR 0044), one
// per Workbench. It is the LanguageController's server (an ILspRequester over whichever clangd the
// open project runs; Busy without one), its editor host (IEditorLanguageHost over the active
// EditorView) and the open documents of its WorkspaceEditApplier (IWorkspaceDocuments over the
// workbench's editor views). Internal to frost_app.
/*
 * Versions. documentVersion() is the version clangd holds for the document (the host keeps it
 * current: every edit through an editor reaches ClangdService::changeDocument first), so results
 * computed for older text are dropped. Edits (completion commits, rename, quick fixes, reformat)
 * are applied through the EditorView as one undo step, so they reach clangd the same way.
 *
 * GUI thread only; the workbench outlives the bridge.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/ui/lang/editor_language_host.hpp"

#include "stable_lsp_requester.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui {
class EditorView;
} // namespace frost::ui

namespace frost::app {

class Workbench;

class LanguageBridge final : public lang::ILspRequester,
                             public ui::lang::IEditorLanguageHost,
                             public lang::IWorkspaceDocuments,
                             public lang::IWorkspaceEditApplier {
public:
    explicit LanguageBridge(Workbench& workbench);
    ~LanguageBridge() override;
    LanguageBridge(const LanguageBridge&) = delete ("the controller refers to it");
    LanguageBridge& operator=(const LanguageBridge&) = delete ("the controller refers to it");

    // The applier the controller's rename and quick fixes use (the encoding follows clangd's).
    [[nodiscard]] lang::IWorkspaceEditApplier& applier() noexcept;
    [[nodiscard]] Result<void> apply(const lang::WorkspaceEdit& edit, std::string_view label) override;
    // The UI publishes and withdraws servers; requests retain the server they captured.
    void setServer(std::shared_ptr<lang::ILspRequester> server) { m_server.setServer(std::move(server)); }

    void setLuaServer(std::shared_ptr<lang::ILspRequester> server) { m_server.setLuaServer(std::move(server)); }
    void setManagedServer(std::shared_ptr<lang::ILspRequester> server) { m_server.setManagedServer(std::move(server)); }
    void setActivePath(std::string_view path) { m_server.setActivePath(path); }

    // ILspRequester: the server for the document.
    [[nodiscard]] sched::Task<Result<lang::Json>> request(std::string method, lang::Json params,
                                                          std::stop_token token = {}) override;
    [[nodiscard]] lang::PositionEncoding encoding() const override;
    [[nodiscard]] std::optional<lang::ServerCapabilities> serverCapabilities() const override;
    void setApplyEditHandler(ApplyEditHandler handler) override { m_server.setApplyEditHandler(std::move(handler)); }

    // IEditorLanguageHost: the active editor.
    [[nodiscard]] std::string documentPath() const override;
    [[nodiscard]] std::optional<text::Snapshot> snapshot() const override;
    [[nodiscard]] std::int64_t documentVersion() const override;
    [[nodiscard]] std::string structureIdentity() const override;
    [[nodiscard]] ByteOffset caretOffset() const override;
    [[nodiscard]] std::optional<text::ByteRange> selection() const override;
    [[nodiscard]] ui::lang::FormattingSettings formattingOptions() const override;
    [[nodiscard]] QRect caretScreenRect() const override;
    [[nodiscard]] QRect screenRectAt(ByteOffset offset) const override;
    [[nodiscard]] Result<void> applyEdits(std::span<const lang::ByteEdit> edits, std::string_view label,
                                          std::optional<ByteOffset> caret) override;
    void select(text::ByteRange range) override;
    void openLocation(std::string_view path, lang::LspPosition position) override;
    // openLocation, then the view of `path` stays read-only (DocumentEditorView::setForcedReadOnly):
    // a metadata-cache source is shown, never edited or saved (LTR-1).
    void openReadOnlyLocation(std::string_view path, lang::LspPosition position) override;
    void setUsageHighlights(std::vector<lang::DocumentHighlight> highlights) override;
    [[nodiscard]] std::vector<std::pair<std::string, text::Snapshot>> openDocuments() const override;
    [[nodiscard]] QWidget* editorWidget() const override;

    // IWorkspaceDocuments: the documents open in editors.
    [[nodiscard]] std::optional<lang::OpenDocumentState> find(std::string_view path) override;
    [[nodiscard]] Result<void> applyEdits(std::string_view path, std::span<const lang::ByteEdit> edits,
                                          std::string_view label) override;

private:
    [[nodiscard]] ui::EditorView* activeEditor() const;
    // Applies `edits` to `editor` as one undo step, the caret at `caret` (default: where the old
    // caret maps). Errors: InvalidArgument (read-only, or the editor refused the edits).
    [[nodiscard]] static Result<void> applyTo(ui::EditorView& editor, std::span<const lang::ByteEdit> edits,
                                              std::optional<ByteOffset> caret);

    Workbench& m_workbench;
    StableLspRequester m_server;
    lang::WorkspaceEditApplier m_applier;
};

} // namespace frost::app
