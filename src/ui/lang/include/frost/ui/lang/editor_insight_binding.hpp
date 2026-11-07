// SPDX-License-Identifier: Apache-2.0
//
// EditorInsightBinding (ADR 0047): connects one EditorView to the language server and the build,
// so the workbench only creates it per editor and hands it ClangdService and the build's
// diagnostics. Plus the pure translations from frost::lang's and frost::build's vocabulary to the
// editor's (frost/ui/editor/editor_insight.hpp), which tests and other hosts reuse.
/*
 * What it does for its view:
 *   - Semantic tokens: on attach and after each edit (debounced, 300 ms by default) it requests
 *     textDocument/semanticTokens/full, or /full/delta with the previous result id when the server
 *     offers deltas, decodes them off the UI thread against the snapshot the request saw, and
 *     hands the spans to EditorView::setSemanticTokens with that snapshot's version (the view maps
 *     them through any edits made meanwhile). A newer request cancels an older one (LatestWins).
 *   - Inactive regions: clangd's textDocument/inactiveRegions through ClangdService's listener;
 *     until the first one arrives for the file, the "comment" tokens clangd uses for disabled
 *     code without that extension stand in.
 *   - Live diagnostics: ClangdService's DiagnosticsStore listener, for this file only. The view
 *     shows "analysing" from an edit until diagnostics of the current text arrive.
 *   - Build diagnostics: setBuildDiagnostics with the build's diagnostics for the file
 *     (BuildService::diagnosticsForDocument), resolved by line and column.
 *
 * Threads and lifetime: UI thread only. Listener callbacks arrive on the server's threads and are
 * posted to the UI thread, where they are dropped once the binding is gone. The requester,
 * scheduler and attached ClangdService must outlive the binding and every request it started;
 * the binding is a child of its view by default, so it dies with it.
 */
#pragma once

#include "frost/build/build_diagnostics.hpp"
#include "frost/core/error.hpp"
#include "frost/lang/diagnostics.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/semantic_tokens.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/sched/latest_wins.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/text/positions.hpp"
#include "frost/ui/editor/editor_insight.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frost::lang {
class ClangdService;
}

namespace frost::ui {
class EditorView;
}

namespace frost::ui::lang {

// --- Translations (pure, thread-safe) ---

// The editor's kind of a server token, or nullopt for kinds the tree-sitter colours already cover
// (keywords, strings, numbers, operators, comments, ...). Variables split by scope: function
// scope is a local, class scope a static field, anything else a global; properties and methods
// with the static modifier are static fields and methods. O(1).
[[nodiscard]] std::optional<SemanticKind> semanticKindFor(::frost::lang::SemanticTokenType type,
                                                          std::uint32_t modifiers) noexcept;
// The editor's modifier bits (semantic_modifiers) of a server token's. O(1).
[[nodiscard]] std::uint16_t semanticModifiersFor(std::uint32_t modifiers) noexcept;
// Every token with an editor kind, as spans. O(tokens).
[[nodiscard]] std::vector<SemanticSpan> toSemanticSpans(std::span<const ::frost::lang::SemanticToken> tokens);
// The "comment" tokens (clangd's disabled code) as ranges, adjacent lines merged. O(tokens).
[[nodiscard]] std::vector<text::ByteRange>
inactiveRangesFromTokens(std::span<const ::frost::lang::SemanticToken> tokens);
// Error, Warning, Information (Info) and Hint (WeakWarning).
[[nodiscard]] InsightSeverity toInsightSeverity(::frost::lang::DiagnosticSeverity severity) noexcept;
// Error, Warning and Message (Info).
[[nodiscard]] InsightSeverity toInsightSeverity(build::Severity severity) noexcept;
// A file's live diagnostics with their mapped ranges (else line and column). O(diagnostics).
[[nodiscard]] std::vector<EditorDiagnostic> toEditorDiagnostics(const ::frost::lang::FileDiagnostics& diagnostics);
// Build diagnostics, by line and column (the ones without a line are skipped). O(diagnostics).
[[nodiscard]] std::vector<EditorDiagnostic> toEditorDiagnostics(std::span<const build::BuildDiagnostic> diagnostics);
// documentHighlight results in `snapshot` (write highlights are write usages); ranges outside it
// are dropped. O(highlights × log n).
[[nodiscard]] std::vector<UsageHighlight>
toUsageHighlights(std::span<const ::frost::lang::DocumentHighlight> highlights, const text::Snapshot& snapshot,
                  ::frost::lang::PositionEncoding encoding);
// LSP inlayHint results mapped to byte offsets of `snapshot`.
[[nodiscard]] std::vector<EditorInlayHint> toEditorInlayHints(std::span<const ::frost::lang::LspInlayHint> hints,
                                                              const text::Snapshot& snapshot,
                                                              ::frost::lang::PositionEncoding encoding);
// Code Vision lenses (usages, implementations, and optional Git author) for classes/structs/methods.
[[nodiscard]] std::vector<EditorCodeVisionLens>
toCodeVisionLenses(std::span<const ::frost::lang::DocumentSymbol> symbols, const text::Snapshot& snapshot,
                   ::frost::lang::PositionEncoding encoding,
                   const std::function<std::string(int line)>& authorForLine = {});

// What the binding talks to (see the lifetime rules above).
struct InsightServices {
    ::frost::lang::ILspRequester& server; // ClangdService (or a test's LspClient).
    sched::IScheduler& scheduler;
};

class EditorInsightBinding final : public QObject {
    Q_OBJECT

public:
    // Binds `view` (its path is the document's). `parent` null: the view is the parent.
    EditorInsightBinding(EditorView& view, InsightServices services, QObject* parent = nullptr);
    ~EditorInsightBinding() override;
    EditorInsightBinding(const EditorInsightBinding&) = delete ("listens on a server and a view");
    EditorInsightBinding& operator=(const EditorInsightBinding&) = delete ("listens on a server and a view");

    // Follows `clangd`'s diagnostics and inactive regions for the view's file (the current ones
    // are applied at once) and requests semantic tokens. Replaces an earlier attach.
    void attach(::frost::lang::ClangdService& clangd);
    // Stops following the attached ClangdService (the destructor also does).
    void detach();

    // The live diagnostics of the view's file (attach's listener calls it too).
    void setLiveDiagnostics(const ::frost::lang::FileDiagnostics& diagnostics);
    // The inactive regions of the view's file (attach's listener calls it too).
    void setInactiveRegions(const ::frost::lang::InactiveRegions& regions);
    // The build's diagnostics for the view's file (BuildService::diagnosticsForDocument); an
    // empty list clears them.
    void setBuildDiagnostics(std::span<const build::BuildDiagnostic> diagnostics);
    // Whether the server runs: false clears the analysing state and stops requests.
    void setServerAvailable(bool available);

    // Requests semantic tokens now (cancels a pending debounce). No-op when the server offers none.
    void refreshSemanticTokens();
    // Requests LSP inlay hints now for the document.
    void refreshInlayHints();
    // Requests document symbols and refreshes Code Vision lenses.
    void refreshCodeVision();
    void setCodeVisionEnabled(bool enabled);
    void setAuthorProvider(std::function<std::string(int line)> provider);
    // The delay between the last edit and the token request (default 300 ms).
    void setDebounce(std::chrono::milliseconds delay);
    // The last result applied (its id is the next delta's base).
    [[nodiscard]] const std::optional<::frost::lang::SemanticTokensData>& lastTokens() const noexcept {
        return m_previous;
    }

signals:
    // Semantic tokens reached the view (tests wait on it).
    void semanticTokensApplied();
    // A token request failed (the old tokens stay).
    void semanticTokensFailed(QString message);
    void inlayHintsApplied();
    void codeVisionApplied();

private:
    void edited();
    [[nodiscard]] bool forThisFile(std::string_view file) const;

    QPointer<EditorView> m_view;
    InsightServices m_services;
    std::string m_path;
    QTimer m_debounce;
    sched::LatestWins m_tokensSlot;
    sched::LatestWins m_inlaySlot;
    sched::LatestWins m_codeVisionSlot;
    std::optional<::frost::lang::SemanticTokensData> m_previous;
    bool m_serverAvailable = true;
    bool m_codeVisionEnabled = false;
    std::function<std::string(int line)> m_authorProvider;
    bool m_haveInactiveRegions = false; // The extension's notification arrived for this file.
    bool m_inactiveFromTokens = false;  // The view's inactive ranges came from comment tokens.
    bool m_liveSeen = false;            // Live diagnostics arrived (a server that diagnoses).
    ::frost::lang::ClangdService* m_clangd = nullptr;
    std::uint64_t m_diagnosticsListener = 0;
    std::uint64_t m_inactiveListener = 0;
};

} // namespace frost::ui::lang
