// SPDX-License-Identifier: Apache-2.0
//
// EditorInsightBinding (frost/ui/lang/editor_insight_binding.hpp): semantic tokens, inactive
// regions and diagnostics from the language server and the build into one EditorView.

#include "frost/ui/lang/editor_insight_binding.hpp"

#include "frost/lang/clangd.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
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
        [self, fn = std::move(fn)]() mutable {
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
    m_view->setDiagnostics(DiagnosticSource::Live, toEditorDiagnostics(diagnostics));
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
        m_view->setDiagnostics(DiagnosticSource::Build, toEditorDiagnostics(diagnostics));
    }
}

void EditorInsightBinding::setServerAvailable(bool available) {
    m_serverAvailable = available;
    if (!available) {
        m_debounce.stop();
        (void)m_tokensSlot.next(); // Drops the result of a request in flight.
        if (m_view) {
            m_view->setAnalyzing(false);
        }
    }
}

void EditorInsightBinding::setDebounce(std::chrono::milliseconds delay) {
    m_debounce.setInterval(static_cast<int>(delay.count()));
}

void EditorInsightBinding::edited() {
    if (!m_serverAvailable) {
        return;
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
        return;
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
            std::vector<text::ByteRange> inactive = inactiveRangesFromTokens(outcome.tokens);
            if (!target.m_haveInactiveRegions && (!inactive.empty() || target.m_inactiveFromTokens)) {
                target.m_inactiveFromTokens = !inactive.empty();
                (void)target.m_view->setInactiveRanges(textVersion, std::move(inactive));
            }
            emit target.semanticTokensApplied();
        });
    };
    m_services.scheduler.spawn(sched::Lane::Interactive, deliver(std::move(task), self, &m_tokensSlot, ticket, version))
        .detach();
}

} // namespace frost::ui::lang
