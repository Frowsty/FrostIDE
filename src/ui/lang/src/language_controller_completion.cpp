// SPDX-License-Identifier: Apache-2.0
//
// LanguageController (frost/ui/lang/language_controller.hpp): code completion and its lookup, the
// editor hooks (keys, typed text, caret moves, usage highlighting delay), parameter info and quick
// documentation (split from language_controller.cpp, FND-12a).

#include "frost/lang/code_assist.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

using detail::fileName;
using detail::lowerAscii;
using detail::qs;

bool allIdentifierBytes(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return fl::isIdentifierByte(c); });
}

} // namespace

// --- Completion ---

void LanguageController::completeCode() {
    if (capabilities().postfix && tryExpandPostfixCompletion()) {
        return;
    }
    requestCompletion(fl::CompletionTrigger{});
}

void LanguageController::requestCompletion(fl::CompletionTrigger trigger) {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const std::string line = lineBeforeCaret();
    const ByteOffset caret = m_services.host.caretOffset();
    const std::size_t typed = line.size() - fl::identifierStart(line);
    m_lookupPath = path;
    m_lookupWordStart = ByteOffset{caret.value() - typed};
    const fl::LspPosition position = positionAt(caret);
    start<fl::CompletionList>(
        m_completionSlot,
        [&server = m_services.server, path, position, trigger](std::stop_token token) {
            return fl::requestCompletion(server, path, position, trigger, std::move(token));
        },
        [this, path](Result<fl::CompletionList> list) {
            if (!list) {
                report(list.error(), "Completion");
                return;
            }
            if (!m_lookupWordStart || m_services.host.documentPath() != path) {
                return;
            }
            m_resolveSlot.cancel();
            m_completion->setList(std::move(*list));
            updateLookupPrefix();
        });
}

void LanguageController::updateLookupPrefix() {
    if (!m_lookupWordStart) {
        return;
    }
    const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
    const ByteOffset caret = m_services.host.caretOffset();
    if (!snapshot || m_services.host.documentPath() != m_lookupPath || caret < *m_lookupWordStart) {
        closeLookup();
        return;
    }
    const std::string prefix = snapshot->text(text::ByteRange{.begin = *m_lookupWordStart, .end = caret});
    if (!prefix.empty() && !allIdentifierBytes(prefix)) {
        closeLookup();
        return;
    }
    m_completion->setPrefix(prefix);
    if (m_completion->model().empty()) {
        m_completion->hide(); // Nothing matches; typing on may match again or close the lookup.
        return;
    }
    m_completion->showAt(m_services.host.screenRectAt(*m_lookupWordStart));
}

void LanguageController::closeLookup() {
    m_completionSlot.cancel();
    m_resolveSlot.cancel();
    m_lookupWordStart.reset();
    m_completion->hide();
}

void LanguageController::commitCompletion(std::size_t itemIndex, bool replaceWord) {
    const auto items = m_completion->model().items();
    const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
    if (itemIndex >= items.size() || !snapshot || !m_lookupWordStart) {
        closeLookup();
        return;
    }
    const fl::CompletionItem item = items[itemIndex];
    const ByteOffset wordStart = *m_lookupWordStart;
    ByteOffset end = snapshot->clip(m_services.host.caretOffset());
    if (replaceWord) {
        // Tab replaces the rest of the identifier after the caret, as in Rider.
        while (end < snapshot->size() && fl::isIdentifierByte(snapshot->byteAt(end))) {
            end = ByteOffset{end.value() + 1};
        }
    }
    closeLookup();
    auto commit = fl::commitCompletion(item, *snapshot, wordStart, end, m_services.server.encoding());
    if (!commit) {
        report(commit.error(), "Completion");
        return;
    }
    if (auto applied = m_services.host.applyEdits(commit->edits, "Complete Code", commit->caret); !applied) {
        report(applied.error(), "Completion");
        return;
    }
    // LuaLS and other servers use this editor-local command to request signature help after
    // inserting a call snippet. Do not execute arbitrary commands supplied by a server.
    const fl::Json* command = item.raw.find("command");
    const bool triggerParameterHints = command && command->stringAt("command") == "editor.action.triggerParameterHints";
    m_tabStops = std::move(commit->tabStops);
    if (!m_tabStops.empty()) {
        const std::optional<text::Snapshot> after = m_services.host.snapshot();
        m_tabStopsDocumentSize = after ? after->size().value() : 0;
        // Only an empty final stop: the caret is already there.
        if (m_tabStops.size() == 1 && m_tabStops.front().empty()) {
            m_tabStops.clear();
        } else {
            static_cast<void>(nextTabStop());
        }
    }
    if (triggerParameterHints)
        requestSignatureHelp(std::nullopt);
}

bool LanguageController::nextTabStop() {
    const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
    if (m_tabStops.empty() || !snapshot) {
        m_tabStops.clear();
        return false;
    }
    // Typing inside the current placeholder shifts every later stop by the size change.
    const std::uint64_t size = snapshot->size().value();
    const auto shift = [&](ByteOffset offset) {
        const std::int64_t moved =
            static_cast<std::int64_t>(offset.value()) +
            (static_cast<std::int64_t>(size) - static_cast<std::int64_t>(m_tabStopsDocumentSize));
        return snapshot->clip(ByteOffset{static_cast<std::uint64_t>(std::max<std::int64_t>(0, moved))});
    };
    for (text::ByteRange& stop : m_tabStops) {
        stop = text::ByteRange{.begin = shift(stop.begin), .end = shift(stop.end)};
    }
    m_tabStopsDocumentSize = size;
    const text::ByteRange next = m_tabStops.front();
    m_tabStops.erase(m_tabStops.begin());
    m_services.host.select(next);
    return true;
}

void LanguageController::resolveSelected(std::size_t itemIndex) {
    const auto capabilities = m_services.server.serverCapabilities();
    const auto items = m_completion->model().items();
    if (!capabilities || !capabilities->completionResolve || itemIndex >= items.size() || items[itemIndex].resolved) {
        return;
    }
    start<fl::CompletionItem>(
        m_resolveSlot,
        [&server = m_services.server, item = items[itemIndex]](std::stop_token token) {
            return fl::resolveCompletionItem(server, item, std::move(token));
        },
        [this, itemIndex](Result<fl::CompletionItem> resolved) {
            if (resolved && m_lookupWordStart && itemIndex < m_completion->model().items().size()) {
                m_completion->replaceItem(itemIndex, std::move(*resolved));
            }
        });
}

// --- Editor hooks ---

bool LanguageController::keyPress(int key, Qt::KeyboardModifiers modifiers) {
    if (m_completion->isVisible() && m_completion->handleKey(key, modifiers)) {
        return true;
    }
    const bool plain =
        (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier)) == 0;
    if (key == Qt::Key_Escape) {
        bool consumed = false;
        for (QWidget* popup : {static_cast<QWidget*>(m_parameterInfo.get()), static_cast<QWidget*>(m_quickDoc.get()),
                               static_cast<QWidget*>(m_quickDef.get()), static_cast<QWidget*>(m_hierarchy.get())}) {
            if (popup->isVisible()) {
                popup->hide();
                consumed = true;
            }
        }
        if (!m_tabStops.empty()) {
            m_tabStops.clear();
            consumed = true;
        }
        return consumed;
    }
    if (key == Qt::Key_Tab && plain && !m_tabStops.empty()) {
        return nextTabStop();
    }
    if (key == Qt::Key_Tab && plain && hasDocument() && capabilities().postfix && tryExpandPostfixCompletion()) {
        return true;
    }
    return false;
}

void LanguageController::textTyped(std::string_view text) {
    if (!hasDocument() || text.empty()) {
        return;
    }
    if (m_lookupWordStart) {
        if (allIdentifierBytes(text)) {
            if (m_completion->model().isIncomplete()) {
                requestCompletion(fl::CompletionTrigger{
                    .kind = fl::CompletionTriggerKind::TriggerForIncompleteCompletions, .character = {}});
            } else {
                updateLookupPrefix();
            }
            return;
        }
        closeLookup();
    }
    if (auto trigger = fl::autoCompletionTrigger(lineBeforeCaret())) {
        requestCompletion(*trigger);
    }
    const char last = text.back();
    if (fl::isSignatureHelpTrigger(last)) {
        requestSignatureHelp(last);
    } else if (last == ')' && m_parameterInfo->isVisible()) {
        requestSignatureHelp(std::nullopt);
    }
}

void LanguageController::caretMoved() {
    if (m_lookupWordStart) {
        const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
        const ByteOffset caret = m_services.host.caretOffset();
        bool inside = snapshot && m_services.host.documentPath() == m_lookupPath && caret >= *m_lookupWordStart;
        if (inside) {
            const std::string prefix = snapshot->text(text::ByteRange{.begin = *m_lookupWordStart, .end = caret});
            inside = prefix.empty() || allIdentifierBytes(prefix);
        }
        if (!inside) {
            closeLookup();
        }
    }
    if (m_quickDoc->isVisible()) {
        m_quickDoc->hide();
    }
    if (m_parameterInfo->isVisible()) {
        requestSignatureHelp(std::nullopt); // The server answers null once the caret leaves the call.
    }
    if (m_structureVisible && hasDocument()) {
        m_structure->highlightLine(static_cast<int>(caretPosition().line));
    }
    m_highlightTimer.start();
}

void LanguageController::setHighlightDelay(std::chrono::milliseconds delay) {
    m_highlightTimer.setInterval(static_cast<int>(delay.count()));
}

// --- Parameter info and quick documentation ---

void LanguageController::showParameterInfo() { requestSignatureHelp(std::nullopt); }

void LanguageController::requestSignatureHelp(std::optional<char> trigger) {
    if (!hasDocument()) {
        return;
    }
    const bool retrigger = m_parameterInfo->isVisible();
    start<std::optional<fl::SignatureHelp>>(
        m_signatureSlot,
        [&server = m_services.server, path = m_services.host.documentPath(), position = caretPosition(), trigger,
         retrigger](std::stop_token token) {
            return fl::requestSignatureHelp(server, path, position, trigger, retrigger, std::move(token));
        },
        [this, explicitRequest = !trigger && !retrigger](Result<std::optional<fl::SignatureHelp>> help) {
            if (!help) {
                report(help.error(), "Parameter info");
                return;
            }
            if (!*help) {
                m_parameterInfo->hide();
                if (explicitRequest) {
                    emit statusMessage(QStringLiteral("No parameter info here"));
                }
                return;
            }
            m_parameterInfo->setHelp(**help);
            m_parameterInfo->showAt(m_services.host.caretScreenRect(), /*above=*/true);
        });
}

void LanguageController::showQuickDocumentation() {
    if (!hasDocument()) {
        return;
    }
    start<std::optional<fl::HoverInfo>>(
        m_hoverSlot,
        [&server = m_services.server, path = m_services.host.documentPath(), position = caretPosition()](
            std::stop_token token) { return fl::requestHover(server, path, position, std::move(token)); },
        [this](Result<std::optional<fl::HoverInfo>> hover) {
            if (!hover) {
                report(hover.error(), "Quick documentation");
                return;
            }
            if (!*hover) {
                emit statusMessage(QStringLiteral("No documentation found"));
                return;
            }
            m_quickDoc->setMarkup((*hover)->contents);
            m_quickDoc->showAt(m_services.host.caretScreenRect());
        });
}

} // namespace frost::ui::lang
