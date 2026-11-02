// SPDX-License-Identifier: Apache-2.0
//
// EditorKeyRouting (frost/ui/editor/editor_key_routing.hpp): Qt key events through the adapter, the
// KeymapService and the ActionRegistry into EditorView actions and typed text.

#include "frost/ui/editor/editor_key_routing.hpp"

#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/key_translation.hpp"

#include <QEvent>
#include <QKeyEvent>

#include <string>
#include <string_view>
#include <utility>

namespace frost::ui {

namespace {

// Appends the UTF-8 form of `cp` (a scalar value from the OS's key text).
void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// The UTF-8 of typed key text; surrogates and values past U+10FFFF are dropped.
std::string toUtf8(std::u32string_view text) {
    std::string out;
    for (const char32_t cp : text) {
        if (cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF)) {
            appendUtf8(out, cp);
        }
    }
    return out;
}

} // namespace

EditorKeyRouting::EditorKeyRouting(input::ActionRegistry& registry, input::KeymapService& keymap,
                                   EditorActionHost& host, input::KeyPlatform platform, const IClock& clock)
    : m_registry(&registry), m_keymap(&keymap), m_host(&host), m_platform(platform), m_clock(&clock) {}

EditorKeyRouting::~EditorKeyRouting() = default;

Result<std::unique_ptr<EditorKeyRouting>> EditorKeyRouting::createStandalone(const input::LayoutTable& layout,
                                                                             input::KeymapPlatform keymapPlatform,
                                                                             input::KeyPlatform keyPlatform,
                                                                             const IClock& clock) {
    std::unique_ptr<EditorKeyRouting> routing(new EditorKeyRouting());
    routing->m_ownedRegistry = std::make_unique<input::ActionRegistry>();
    routing->m_ownedHost = std::make_unique<EditorActionHost>();
    if (Result<void> registered = registerEditorActions(*routing->m_ownedRegistry, *routing->m_ownedHost);
        !registered) {
        return std::unexpected(registered.error());
    }
    routing->m_ownedSettings = std::make_unique<input::MemoryKeymapSettings>();
    Result<input::KeymapService> service =
        input::KeymapService::create(*routing->m_ownedRegistry, *routing->m_ownedSettings, keymapPlatform, layout);
    if (!service) {
        return std::unexpected(service.error());
    }
    routing->m_ownedKeymap.emplace(std::move(*service));
    routing->m_registry = routing->m_ownedRegistry.get();
    routing->m_keymap = &*routing->m_ownedKeymap;
    routing->m_host = routing->m_ownedHost.get();
    routing->m_platform = keyPlatform;
    routing->m_clock = &clock;
    return routing;
}

input::KeyEvent EditorKeyRouting::translate(const QKeyEvent& event) const noexcept {
    return translateKeyEvent(event, KeyTranslationContext{m_platform, m_keymap->layout(), m_ime, *m_clock});
}

input::ActionContext EditorKeyRouting::contextFor(const EditorView& view) const noexcept {
    input::ActionContext context = m_base;
    context.keys.set(input::ContextKey::EditorFocus);
    context.keys.set(input::ContextKey::EditorReadOnly, view.isReadOnly());
    context.keys.set(input::ContextKey::EditorHasSelection, view.hasSelection());
    return context;
}

bool EditorKeyRouting::handleKey(EditorView& view, const QKeyEvent& event) {
    const input::KeyEvent key = translate(event);
    const input::ActionContext context = contextFor(view);
    m_host->setActive(&view);
    const input::MatchResult result = m_keymap->handle(key, context);
    switch (result.outcome) {
    case input::MatchOutcome::Fire:
        (void)m_registry->tryInvoke(result.action, context,
                                    input::InvokeInfo{input::InvokeSource::Keyboard, key.autoRepeat});
        return true;
    case input::MatchOutcome::Consumed:
    case input::MatchOutcome::Pending: return true;
    case input::MatchOutcome::PassThrough: break;
    }
    if (!key.isPress()) {
        return false;
    }
    const std::u32string_view typed = input::typedText(key, m_platform);
    if (typed.empty()) {
        return false;
    }
    return view.typeText(toUtf8(typed));
}

bool EditorKeyRouting::wantsKey(EditorView& view, const QKeyEvent& event) {
    const input::KeyEvent key = translate(event);
    const input::MatchOutcome outcome = m_keymap->peek(key, contextFor(view));
    if (outcome != input::MatchOutcome::PassThrough) {
        return true;
    }
    return key.isPress() && !input::typedText(key, m_platform).empty();
}

void EditorKeyRouting::observe(const QEvent& event) {
    m_ime.observe(event);
    if (m_ime.takeCompositionStarted()) {
        m_keymap->reset();
    }
}

} // namespace frost::ui
