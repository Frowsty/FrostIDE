// SPDX-License-Identifier: Apache-2.0
//
// EditorInsightBinding (frost/ui/lang/editor_insight_binding.hpp): semantic tokens, inactive
// regions and diagnostics from the language server and the build into one EditorView.

#include "frost/ui/lang/editor_insight_binding.hpp"

#include "frost/lang/clangd.hpp"
#include "frost/lang/code_vision.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/lang/language_capabilities.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

using fl::SemanticModifier;
using fl::SemanticTokenType;

// Posts `fn` to the UI thread; it runs only while `self` is alive.
template<class Fn>
void post(QPointer<EditorInsightBinding> self, Fn fn) {
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(
        app,
        [self = std::move(self), fn = std::move(fn)]() mutable {
            if (self) {
                fn(*self);
            }
        },
        Qt::QueuedConnection);
}

// One token request's outcome: the new integer array and its decoded tokens.
struct TokensOutcome {
    fl::SemanticTokensData data;
    std::vector<fl::SemanticToken> tokens;
};

struct CodeVisionOutcome {
    std::vector<fl::DocumentSymbol> symbols;
    std::vector<CodeVisionCountEntry> counts;
};

// Executes Code Vision requests against clangd's index and maps their exact declarations to the
// document snapshot used for this request. Each metric is omitted if the server has no answer.
sched::Task<Result<CodeVisionOutcome>> codeVisionCountsTask(fl::ILspRequester& server, std::string path,
                                                            text::Snapshot snapshot, fl::PositionEncoding encoding,
                                                            bool cppCounts, std::stop_token token) {
    auto symbols = co_await fl::requestDocumentSymbols(server, path, token);
    if (!symbols) {
        co_return std::unexpected(std::move(symbols.error()));
    }
    if (!cppCounts) {
        co_return CodeVisionOutcome{std::move(*symbols), {}};
    }
    const auto rows = fl::flattenSymbols(*symbols);
    fl::CppCodeVisionProvider provider;
    std::vector<CodeVisionCountEntry> entries;
    for (const fl::SymbolRow& row : rows) {
        if (!row.symbol) {
            continue;
        }
        const fl::DocumentSymbol& symbol = *row.symbol;
        const bool type = symbol.kind == fl::SymbolKind::Class || symbol.kind == fl::SymbolKind::Struct ||
                          symbol.kind == fl::SymbolKind::Interface;
        const bool callable = symbol.kind == fl::SymbolKind::Function || symbol.kind == fl::SymbolKind::Method ||
                              symbol.kind == fl::SymbolKind::Constructor;
        if (!type && !callable) {
            continue;
        }
        auto offset = fl::fromLspPosition(snapshot, symbol.selectionRange.start, encoding);
        if (!offset) {
            continue;
        }
        auto counts = co_await provider.counts(server, path,
                                               fl::CodeVisionSymbol{symbol.selectionRange.start, symbol.kind}, token);
        if (!counts) {
            if (token.stop_requested()) {
                co_return std::unexpected(std::move(counts.error()));
            }
            continue; // Keep any Git author lenses; a failed index metric is simply unavailable.
        }
        entries.push_back(CodeVisionCountEntry{*offset, snapshot.lineAt(*offset), *counts});
    }
    co_return CodeVisionOutcome{std::move(*symbols), std::move(entries)};
}

// Requests and decodes off the UI thread (the snapshot is the one the request was made for).
sched::Task<Result<TokensOutcome>> tokensTask(fl::ILspRequester& server, std::string path,
                                              std::optional<fl::SemanticTokensData> previous, bool useDelta,
                                              fl::SemanticTokensLegend legend, text::Snapshot snapshot,
                                              fl::PositionEncoding encoding, std::stop_token token) {
    Result<fl::SemanticTokensData> data =
        co_await fl::requestSemanticTokens(server, std::move(path), std::move(previous), useDelta, token);
    if (!data) {
        co_return std::unexpected(std::move(data.error()));
    }
    Result<std::vector<fl::SemanticToken>> tokens = fl::decodeSemanticTokens(data->data, legend, snapshot, encoding);
    if (!tokens) {
        co_return std::unexpected(std::move(tokens.error()));
    }
    co_return TokensOutcome{std::move(*data), std::move(*tokens)};
}

} // namespace

std::optional<SemanticKind> semanticKindFor(SemanticTokenType type, std::uint32_t modifiers) noexcept {
    const bool isStatic = fl::hasModifier(modifiers, SemanticModifier::Static);
    switch (type) {
    case SemanticTokenType::Namespace: return SemanticKind::Namespace;
    case SemanticTokenType::Type: return SemanticKind::TypeAlias;
    case SemanticTokenType::Class:
    case SemanticTokenType::Struct:
    case SemanticTokenType::Interface: return SemanticKind::Class;
    case SemanticTokenType::Enum: return SemanticKind::Enum;
    case SemanticTokenType::TypeParameter: return SemanticKind::TemplateParameter;
    case SemanticTokenType::Concept: return SemanticKind::Concept;
    case SemanticTokenType::Parameter: return SemanticKind::Parameter;
    case SemanticTokenType::Variable:
        if (fl::hasModifier(modifiers, SemanticModifier::FunctionScope)) {
            return SemanticKind::LocalVariable;
        }
        if (fl::hasModifier(modifiers, SemanticModifier::ClassScope)) {
            return SemanticKind::StaticField;
        }
        return SemanticKind::GlobalVariable;
    case SemanticTokenType::Property: return isStatic ? SemanticKind::StaticField : SemanticKind::Field;
    case SemanticTokenType::EnumMember: return SemanticKind::Enumerator;
    case SemanticTokenType::Function: return SemanticKind::Function;
    case SemanticTokenType::Method: return isStatic ? SemanticKind::StaticMethod : SemanticKind::Method;
    case SemanticTokenType::Macro: return SemanticKind::Macro;
    default: return std::nullopt;
    }
}

std::uint16_t semanticModifiersFor(std::uint32_t modifiers) noexcept {
    namespace m = semantic_modifiers;
    std::uint16_t out = 0;
    const auto add = [&](SemanticModifier modifier, std::uint16_t bit) {
        if (fl::hasModifier(modifiers, modifier)) {
            out = static_cast<std::uint16_t>(out | bit);
        }
    };
    add(SemanticModifier::Readonly, m::kReadonly);
    add(SemanticModifier::Static, m::kStatic);
    add(SemanticModifier::Deprecated, m::kDeprecated);
    add(SemanticModifier::Declaration, m::kDeclaration);
    add(SemanticModifier::Definition, m::kDeclaration);
    add(SemanticModifier::Virtual, m::kVirtual);
    add(SemanticModifier::DefaultLibrary, m::kDefaultLibrary);
    return out;
}

std::vector<SemanticSpan> toSemanticSpans(std::span<const fl::SemanticToken> tokens) {
    std::vector<SemanticSpan> out;
    out.reserve(tokens.size());
    for (const fl::SemanticToken& token : tokens) {
        if (token.range.empty()) {
            continue;
        }
        if (const std::optional<SemanticKind> kind = semanticKindFor(token.type, token.modifiers)) {
            out.push_back(SemanticSpan{token.range, *kind, semanticModifiersFor(token.modifiers)});
        }
    }
    return out;
}

std::vector<text::ByteRange> inactiveRangesFromTokens(std::span<const fl::SemanticToken> tokens) {
    std::vector<text::ByteRange> out;
    for (const fl::SemanticToken& token : tokens) {
        if (token.type != SemanticTokenType::Comment || token.range.empty()) {
            continue;
        }
        // Consecutive lines' tokens are one region when only a line break separates them.
        if (!out.empty() && token.range.begin >= out.back().end && token.range.begin - out.back().end <= 2) {
            out.back().end = std::max(out.back().end, token.range.end);
        } else {
            out.push_back(token.range);
        }
    }
    return out;
}

InsightSeverity toInsightSeverity(fl::DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case fl::DiagnosticSeverity::Error: return InsightSeverity::Error;
    case fl::DiagnosticSeverity::Warning: return InsightSeverity::Warning;
    case fl::DiagnosticSeverity::Information: return InsightSeverity::Info;
    case fl::DiagnosticSeverity::Hint: return InsightSeverity::WeakWarning;
    }
    return InsightSeverity::Error;
}

InsightSeverity toInsightSeverity(build::Severity severity) noexcept {
    switch (severity) {
    case build::Severity::Error: return InsightSeverity::Error;
    case build::Severity::Warning: return InsightSeverity::Warning;
    case build::Severity::Message: return InsightSeverity::Info;
    }
    return InsightSeverity::Error;
}

std::vector<EditorDiagnostic> toEditorDiagnostics(const fl::FileDiagnostics& diagnostics) {
    std::vector<EditorDiagnostic> out;
    out.reserve(diagnostics.diagnostics.size());
    for (const fl::LiveDiagnostic& live : diagnostics.diagnostics) {
        EditorDiagnostic diagnostic;
        diagnostic.severity = toInsightSeverity(live.severity);
        diagnostic.message = live.message;
        diagnostic.code = live.code;
        diagnostic.range = live.range;
        diagnostic.textVersion = live.textVersion ? live.textVersion : diagnostics.textVersion;
        diagnostic.line = live.line;
        diagnostic.column = live.column;
        out.push_back(std::move(diagnostic));
    }
    return out;
}

std::vector<EditorDiagnostic> toEditorDiagnostics(std::span<const build::BuildDiagnostic> diagnostics) {
    std::vector<EditorDiagnostic> out;
    for (const build::BuildDiagnostic& entry : diagnostics) {
        const build::Diagnostic& d = entry.diagnostic;
        if (d.line <= 0) {
            continue;
        }
        EditorDiagnostic diagnostic;
        diagnostic.severity = toInsightSeverity(d.severity);
        diagnostic.message = d.code.empty() ? d.message : d.code + ": " + d.message;
        diagnostic.code = d.code;
        diagnostic.line = d.line;
        diagnostic.column = d.column;
        out.push_back(std::move(diagnostic));
    }
    return out;
}

std::vector<UsageHighlight> toUsageHighlights(std::span<const fl::DocumentHighlight> highlights,
                                              const text::Snapshot& snapshot, fl::PositionEncoding encoding) {
    std::vector<UsageHighlight> out;
    for (const fl::DocumentHighlight& highlight : highlights) {
        const Result<ByteOffset> begin = fl::fromLspPosition(snapshot, highlight.range.start, encoding);
        const Result<ByteOffset> end = fl::fromLspPosition(snapshot, highlight.range.end, encoding);
        if (!begin || !end || *end < *begin) {
            continue;
        }
        out.push_back(UsageHighlight{text::ByteRange{*begin, *end}, highlight.kind == fl::HighlightKind::Write});
    }
    return out;
}

std::vector<EditorInlayHint> toEditorInlayHints(std::span<const fl::LspInlayHint> hints, const text::Snapshot& snapshot,
                                                fl::PositionEncoding encoding) {
    std::vector<EditorInlayHint> out;
    out.reserve(hints.size());
    for (const fl::LspInlayHint& hint : hints) {
        const Result<ByteOffset> offset = fl::fromLspPosition(snapshot, hint.position, encoding);
        if (!offset || hint.label.empty()) {
            continue;
        }
        out.push_back(EditorInlayHint{
            .offset = *offset,
            .label = hint.label,
            .kind = hint.kind == fl::LspInlayHintKind::Type ? InlayHintKind::Type : InlayHintKind::Parameter,
            .tooltip = hint.tooltip,
        });
    }
    return out;
}

std::vector<EditorDocumentLink> toEditorDocumentLinks(std::span<const fl::DocumentLink> links,
                                                      const text::Snapshot& snapshot, fl::PositionEncoding encoding) {
    std::vector<EditorDocumentLink> out;
    out.reserve(links.size());
    for (const fl::DocumentLink& link : links) {
        if (link.path.empty()) {
            continue;
        }
        const Result<ByteOffset> begin = fl::fromLspPosition(snapshot, link.range.start, encoding);
        const Result<ByteOffset> end = fl::fromLspPosition(snapshot, link.range.end, encoding);
        if (!begin || !end || *end <= *begin) {
            continue;
        }
        out.push_back(EditorDocumentLink{text::ByteRange{*begin, *end}, link.path, link.tooltip});
    }
    return out;
}

std::vector<text::ByteRange> toFoldCandidates(std::span<const fl::FoldingRange> ranges,
                                              const text::Snapshot& snapshot) {
    std::vector<text::ByteRange> out;
    out.reserve(ranges.size());
    for (const fl::FoldingRange& range : ranges) {
        if (range.endLine <= range.startLine) {
            continue;
        }
        const auto first = snapshot.lineInfo(LineIndex{range.startLine});
        const auto last = snapshot.lineInfo(LineIndex{range.endLine});
        if (first && last && first->content.end < last->content.begin) {
            out.push_back(text::ByteRange{first->content.end, last->content.begin});
        }
    }
    return out;
}

/*
 * Code Vision (FND-3, ADR 0078): only lenses from real data. The Git author of the declaration line
 * (authorForLine, from git blame) and the usage, override and implementation counts from clangd's
 * workspace index are shown; no count is guessed from source text.
 */
std::vector<EditorCodeVisionLens> toCodeVisionLenses(std::span<const fl::DocumentSymbol> symbols,
                                                     const text::Snapshot& snapshot, fl::PositionEncoding encoding,
                                                     const std::function<std::string(int line)>& authorForLine) {
    std::vector<EditorCodeVisionLens> out;
    if (!authorForLine) {
        return out;
    }
    const std::vector<fl::DocumentSymbol> symbolList(symbols.begin(), symbols.end());
    const std::vector<fl::SymbolRow> rows = fl::flattenSymbols(symbolList);
    for (const fl::SymbolRow& row : rows) {
        if (row.symbol == nullptr || row.depth > 1) {
            continue;
        }
        const fl::DocumentSymbol& sym = *row.symbol;
        const bool isType = sym.kind == fl::SymbolKind::Class || sym.kind == fl::SymbolKind::Struct ||
                            sym.kind == fl::SymbolKind::Interface || sym.kind == fl::SymbolKind::Enum;
        const bool isCallable = sym.kind == fl::SymbolKind::Function || sym.kind == fl::SymbolKind::Method ||
                                sym.kind == fl::SymbolKind::Constructor;
        if (!isType && !isCallable) {
            continue;
        }
        const Result<ByteOffset> symOffset = fl::fromLspPosition(snapshot, sym.selectionRange.start, encoding);
        if (!symOffset) {
            continue;
        }
        const LineIndex declLine = snapshot.lineAt(*symOffset);
        const std::string author = authorForLine(static_cast<int>(declLine.value()) + 1);
        if (!author.empty()) {
            out.push_back(EditorCodeVisionLens{
                .line = declLine,
                .symbolOffset = *symOffset,
                .label = author,
                .actionId = "vcs.annotate",
                .tooltip = "Last modified by " + author + " (click to annotate)",
            });
        }
    }
    return out;
}

std::vector<EditorCodeVisionLens> toCodeVisionCountLenses(std::span<const CodeVisionCountEntry> entries) {
    std::vector<EditorCodeVisionLens> out;
    const auto append = [&](const CodeVisionCountEntry& entry, std::size_t count, std::string noun,
                            std::string action) {
        if (count != 1) {
            noun.push_back('s');
        }
        out.push_back(EditorCodeVisionLens{
            .line = entry.line,
            .symbolOffset = entry.offset,
            .label = std::to_string(count) + " " + noun,
            .actionId = std::move(action),
            .tooltip = "Count from clangd's workspace index",
        });
    };
    for (const CodeVisionCountEntry& entry : entries) {
        if (entry.counts.usages) {
            append(entry, *entry.counts.usages, "usage", "navigate.findUsages");
        }
        if (entry.counts.overrides) {
            append(entry, *entry.counts.overrides, "override", "navigate.implementation");
        }
        if (entry.counts.implementations) {
            append(entry, *entry.counts.implementations, "implementation", "navigate.typeImplementations");
        }
    }
    return out;
}

// --- the binding ---

EditorInsightBinding::EditorInsightBinding(EditorView& view, InsightServices services, QObject* parent)
    : QObject(parent != nullptr ? parent : &view), m_view(&view), m_services(services), m_path(view.path()) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &EditorInsightBinding::refreshSemanticTokens);
    connect(&view, &EditorView::documentEdited, this, [this](quint64) { edited(); });
    view.setInsightEnabled(true);
}

EditorInsightBinding::~EditorInsightBinding() { detach(); }

bool EditorInsightBinding::forThisFile(std::string_view file) const {
    return fl::documentKey(file) == fl::documentKey(m_path);
}

void EditorInsightBinding::attach(fl::ClangdService& clangd) {
    detach();
    m_clangd = &clangd;
    const QPointer<EditorInsightBinding> self(this);
    const std::string key = fl::documentKey(m_path);
    m_diagnosticsListener = clangd.diagnostics().addListener([self, key](const fl::FileDiagnostics& diagnostics) {
        if (fl::documentKey(diagnostics.file) != key) {
            return;
        }
        post(self, [diagnostics](EditorInsightBinding& binding) { binding.setLiveDiagnostics(diagnostics); });
    });
    m_inactiveListener = clangd.addInactiveRegionsListener([self, key](const fl::InactiveRegions& regions) {
        if (fl::documentKey(regions.file) != key) {
            return;
        }
        post(self, [regions](EditorInsightBinding& binding) { binding.setInactiveRegions(regions); });
    });
    if (const std::optional<fl::FileDiagnostics> current = clangd.diagnostics().forFile(m_path)) {
        setLiveDiagnostics(*current);
    } else if (m_view) {
        m_view->setAnalyzing(true);
    }
    if (const std::optional<fl::InactiveRegions> current = clangd.inactiveRegions(m_path)) {
        setInactiveRegions(*current);
    }
    refreshSemanticTokens();
}

void EditorInsightBinding::detach() {
    m_debounce.stop();
    m_tokensSlot.cancel(); // Cancel in-flight work even if no clangd was attached.
    m_inlaySlot.cancel();
    m_linksSlot.cancel();
    m_foldSlot.cancel();
    m_codeVisionSlot.cancel();
    m_previous.reset();
    m_haveInactiveRegions = false;
    m_inactiveFromTokens = false;
    m_liveSeen = false;
    if (m_clangd == nullptr) {
        return;
    }
    m_clangd->diagnostics().removeListener(m_diagnosticsListener);
    m_clangd->removeInactiveRegionsListener(m_inactiveListener);
    m_clangd = nullptr;
}

void EditorInsightBinding::setLiveDiagnostics(const fl::FileDiagnostics& diagnostics) {
    if (!m_view || !forThisFile(diagnostics.file)) {
        return;
    }
    m_liveSeen = true;
    m_view->setDiagnostics(EditorDiagnosticSource::Live, toEditorDiagnostics(diagnostics));
    // Still analysing while the diagnostics describe an older text than the view's.
    const std::uint64_t current = m_view->editor().text().version();
    const bool behind = diagnostics.stale || (diagnostics.textVersion && *diagnostics.textVersion != current);
    m_view->setAnalyzing(m_serverAvailable && behind);
}

void EditorInsightBinding::setInactiveRegions(const fl::InactiveRegions& regions) {
    if (!m_view || !forThisFile(regions.file) || !regions.textVersion) {
        return;
    }
    m_haveInactiveRegions = true;
    m_inactiveFromTokens = false;
    (void)m_view->setInactiveRanges(*regions.textVersion, regions.ranges);
}

void EditorInsightBinding::setBuildDiagnostics(std::span<const build::BuildDiagnostic> diagnostics) {
    if (m_view) {
        m_view->setDiagnostics(EditorDiagnosticSource::Build, toEditorDiagnostics(diagnostics));
    }
}

void EditorInsightBinding::setServerAvailable(bool available) {
    m_serverAvailable = available;
    if (!available) {
        m_debounce.stop();
        (void)m_tokensSlot.next(); // Drops the result of a request in flight.
        (void)m_inlaySlot.next();
        (void)m_linksSlot.next();
        (void)m_foldSlot.next();
        (void)m_codeVisionSlot.next();
        if (m_view) {
            m_view->setAnalyzing(false);
            m_view->setCodeVisionLenses({});
        }
    }
}

void EditorInsightBinding::setCodeVisionEnabled(bool enabled) {
    m_codeVisionEnabled = enabled;
    if (!enabled && m_view) {
        (void)m_codeVisionSlot.next();
        m_view->setCodeVisionLenses({});
    }
}

void EditorInsightBinding::setAuthorProvider(std::function<std::string(int line)> provider) {
    m_authorProvider = std::move(provider);
}

void EditorInsightBinding::setDebounce(std::chrono::milliseconds delay) {
    m_debounce.setInterval(static_cast<int>(delay.count()));
}

void EditorInsightBinding::edited() {
    if (!m_serverAvailable) {
        return;
    }
    if (m_view && m_codeVisionEnabled) {
        (void)m_codeVisionSlot.next();
        m_view->setCodeVisionLenses({});
    }
    if (m_view && (m_clangd != nullptr || m_liveSeen)) {
        m_view->setAnalyzing(true); // Until the server's diagnostics of the new text arrive.
    }
    m_debounce.start();
}

void EditorInsightBinding::refreshSemanticTokens() {
    m_debounce.stop();
    if (!m_view || !m_serverAvailable) {
        return;
    }
    const std::optional<fl::ServerCapabilities> capabilities = m_services.server.serverCapabilities();
    if (!capabilities) {
        // The language server initializes asynchronously; retry until its capabilities are available.
        m_debounce.start();
        return;
    }
    if (capabilities->inlayHint) {
        refreshInlayHints();
    }
    if (capabilities->documentLink) {
        refreshDocumentLinks();
    }
    if (capabilities->foldingRange) {
        refreshFoldingRanges();
    }
    if (m_codeVisionEnabled && capabilities->documentSymbol) {
        refreshCodeVision();
    }
    const std::optional<fl::SemanticTokensSupport> support = fl::semanticTokensSupport(*capabilities);
    if (!support || !support->full) {
        return;
    }
    const text::Snapshot snapshot = m_view->editor().text();
    const std::uint64_t version = snapshot.version();
    const sched::LatestWins::Ticket ticket = m_tokensSlot.next();
    sched::Task<Result<TokensOutcome>> task =
        tokensTask(m_services.server, m_path, m_previous, support->delta, support->legend, snapshot,
                   m_services.server.encoding(), ticket.token);
    const QPointer<EditorInsightBinding> self(this);
    auto deliver = [](sched::Task<Result<TokensOutcome>> work, QPointer<EditorInsightBinding> binding,
                      sched::LatestWins* slot, sched::LatestWins::Ticket current,
                      std::uint64_t textVersion) -> sched::Task<void> {
        auto result = std::make_shared<Result<TokensOutcome>>(co_await std::move(work));
        post(binding, [slot, current, textVersion, result](EditorInsightBinding& target) {
            if (!slot->isCurrent(current) || !target.m_view) {
                return;
            }
            if (!*result) {
                emit target.semanticTokensFailed(QString::fromStdString(result->error().message()));
                return;
            }
            TokensOutcome& outcome = **result;
            target.m_previous = std::move(outcome.data);
            if (!target.m_view->setSemanticTokens(textVersion, toSemanticSpans(outcome.tokens))) {
                target.m_debounce.start(); // Too many edits since: ask again.
                return;
            }
            if (languageIdForPath(target.m_path) == "cpp") {
                std::vector<text::ByteRange> inactive = inactiveRangesFromTokens(outcome.tokens);
                if (!target.m_haveInactiveRegions && (!inactive.empty() || target.m_inactiveFromTokens)) {
                    target.m_inactiveFromTokens = !inactive.empty();
                    (void)target.m_view->setInactiveRanges(textVersion, std::move(inactive));
                }
            }
            emit target.semanticTokensApplied();
        });
    };
    m_services.scheduler.spawn(sched::Lane::Interactive, deliver(std::move(task), self, &m_tokensSlot, ticket, version))
        .detach();
}

void EditorInsightBinding::refreshInlayHints() {
    if (!m_view || !m_serverAvailable) {
        return;
    }
    const auto capabilities = m_services.server.serverCapabilities();
    if (!capabilities || !capabilities->inlayHint) return;
    const text::Snapshot snapshot = m_view->editor().text();
    const std::uint64_t version = snapshot.version();
    const auto encoding = m_services.server.encoding();
    const fl::LspRange fullRange{fl::LspPosition{0, 0}, fl::toLspPosition(snapshot, snapshot.size(), encoding)};
    const sched::LatestWins::Ticket ticket = m_inlaySlot.next();
    auto work = fl::requestInlayHints(m_services.server, m_path, fullRange, ticket.token);
    const QPointer<EditorInsightBinding> self(this);
    auto deliver = [](sched::Task<Result<std::vector<fl::LspInlayHint>>> task, QPointer<EditorInsightBinding> binding,
                      sched::LatestWins* slot, sched::LatestWins::Ticket current, text::Snapshot snap,
                      fl::PositionEncoding enc, std::uint64_t textVersion) -> sched::Task<void> {
        auto result = std::make_shared<Result<std::vector<fl::LspInlayHint>>>(co_await std::move(task));
        if (!*result) {
            co_return;
        }
        auto mapped = std::make_shared<std::vector<EditorInlayHint>>(toEditorInlayHints(**result, snap, enc));
        post(binding, [slot, current, textVersion, mapped](EditorInsightBinding& target) {
            if (!slot->isCurrent(current) || !target.m_view) {
                return;
            }
            if (target.m_view->setInlayHints(textVersion, std::move(*mapped))) {
                emit target.inlayHintsApplied();
            }
        });
    };
    m_services.scheduler
        .spawn(sched::Lane::Interactive,
               deliver(std::move(work), self, &m_inlaySlot, ticket, snapshot, encoding, version))
        .detach();
}

void EditorInsightBinding::refreshDocumentLinks() {
    if (!m_view || !m_serverAvailable) {
        return;
    }
    const text::Snapshot snapshot = m_view->editor().text();
    const std::uint64_t version = snapshot.version();
    const sched::LatestWins::Ticket ticket = m_linksSlot.next();
    const fl::PositionEncoding encoding = m_services.server.encoding();
    auto work = fl::requestDocumentLinks(m_services.server, m_path, ticket.token);
    const QPointer<EditorInsightBinding> self(this);
    auto deliver = [](sched::Task<Result<std::vector<fl::DocumentLink>>> task, QPointer<EditorInsightBinding> binding,
                      sched::LatestWins* slot, sched::LatestWins::Ticket current, text::Snapshot snap,
                      fl::PositionEncoding enc, std::uint64_t textVersion) -> sched::Task<void> {
        auto result = co_await std::move(task);
        if (!result) {
            co_return; // The old links stay; the next edit asks again.
        }
        auto mapped = std::make_shared<std::vector<EditorDocumentLink>>(toEditorDocumentLinks(*result, snap, enc));
        post(binding, [slot, current, textVersion, mapped](EditorInsightBinding& target) {
            if (!slot->isCurrent(current) || !target.m_view) {
                return;
            }
            if (target.m_view->setDocumentLinks(textVersion, std::move(*mapped))) {
                emit target.documentLinksApplied();
            }
        });
    };
    m_services.scheduler
        .spawn(sched::Lane::Interactive,
               deliver(std::move(work), self, &m_linksSlot, ticket, snapshot, encoding, version))
        .detach();
}

void EditorInsightBinding::refreshFoldingRanges() {
    if (!m_view || !m_serverAvailable) {
        return;
    }
    const text::Snapshot snapshot = m_view->editor().text();
    const std::uint64_t version = snapshot.version();
    const sched::LatestWins::Ticket ticket = m_foldSlot.next();
    auto work = fl::requestFoldingRanges(m_services.server, m_path, ticket.token);
    const QPointer<EditorInsightBinding> self(this);
    auto deliver = [](sched::Task<Result<std::vector<fl::FoldingRange>>> task, QPointer<EditorInsightBinding> binding,
                      sched::LatestWins* slot, sched::LatestWins::Ticket current, text::Snapshot snap,
                      std::uint64_t textVersion) -> sched::Task<void> {
        auto result = co_await std::move(task);
        if (!result) {
            co_return;
        }
        auto mapped = std::make_shared<std::vector<text::ByteRange>>(toFoldCandidates(*result, snap));
        post(binding, [slot, current, textVersion, mapped](EditorInsightBinding& target) {
            if (!slot->isCurrent(current) || !target.m_view) {
                return;
            }
            // Refused after an edit: the debounced refresh that edit started asks again.
            if (target.m_view->setLanguageFoldRanges(textVersion, std::move(*mapped))) {
                emit target.foldingRangesApplied();
            }
        });
    };
    m_services.scheduler
        .spawn(sched::Lane::Interactive, deliver(std::move(work), self, &m_foldSlot, ticket, snapshot, version))
        .detach();
}

void EditorInsightBinding::refreshCodeVision() {
    if (!m_view || !m_serverAvailable || !m_codeVisionEnabled || !capabilitiesForPath(m_path).codeVision) {
        return;
    }
    const text::Snapshot snapshot = m_view->editor().text();
    const sched::LatestWins::Ticket ticket = m_codeVisionSlot.next();
    const fl::PositionEncoding encoding = m_services.server.encoding();
    const auto authorFn = m_authorProvider;
    auto work = codeVisionCountsTask(m_services.server, m_path, snapshot, encoding, languageIdForPath(m_path) == "cpp",
                                     ticket.token);
    const QPointer<EditorInsightBinding> self(this);
    auto deliver = [](sched::Task<Result<CodeVisionOutcome>> task, QPointer<EditorInsightBinding> binding,
                      sched::LatestWins* slot, sched::LatestWins::Ticket current, text::Snapshot snap,
                      fl::PositionEncoding enc,
                      std::function<std::string(int line)> authorProvider) -> sched::Task<void> {
        auto result = std::make_shared<Result<CodeVisionOutcome>>(co_await std::move(task));
        if (!*result) {
            co_return;
        }
        auto lenses = std::make_shared<std::vector<EditorCodeVisionLens>>(
            toCodeVisionLenses((*result)->symbols, snap, enc, authorProvider));
        std::vector<EditorCodeVisionLens> counts = toCodeVisionCountLenses((*result)->counts);
        lenses->insert(lenses->end(), std::make_move_iterator(counts.begin()), std::make_move_iterator(counts.end()));
        post(binding, [slot, current, lenses, textVersion = snap.version()](EditorInsightBinding& target) {
            if (!slot->isCurrent(current) || !target.m_view) {
                return;
            }
            if (target.m_view->editor().text().version() != textVersion) {
                return;
            }
            target.m_view->setCodeVisionLenses(std::move(*lenses));
            emit target.codeVisionApplied();
        });
    };
    m_services.scheduler
        .spawn(sched::Lane::Interactive,
               deliver(std::move(work), self, &m_codeVisionSlot, ticket, snapshot, encoding, authorFn))
        .detach();
}

} // namespace frost::ui::lang
