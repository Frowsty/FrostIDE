// SPDX-License-Identifier: Apache-2.0
//
// EditorKeyRouting (ADR 0016, ADR 0037): how an EditorView turns Qt key events into editor actions.
// Each event is translated by frost::ui's adapter, matched by the KeymapService (the IntelliJ keymap,
// with ADR 0030's German reading), fired through the ActionRegistry, or typed when it passes through.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/input/action.hpp"
#include "frost/input/key_event.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/input/layout.hpp"
#include "frost/ui/editor/editor_actions.hpp"
#include "frost/ui/ime_tracker.hpp"

#include <memory>
#include <optional>

class QEvent;
class QKeyEvent;

namespace frost::ui {

class EditorView;

/*
 * Routes the keys of every EditorView that is given it. Two forms:
 *   - the shell's: borrow its ActionRegistry and KeymapService, in which registerEditorActions was
 *     called with `host` (the shell's other actions then share the keymap);
 *   - standalone (createStandalone): own a registry with only the editor actions, in-memory keymap
 *     preferences and a KeymapService on a given layout, for editors outside the shell and tests.
 * Per key: translateKeyEvent, KeymapService::handle with the view's context (baseContext() plus
 * EditorFocus, EditorReadOnly, EditorHasSelection); Fire runs the action through the registry,
 * Consumed and Pending swallow the key, and PassThrough types frost::input::typedText.
 *
 * GUI thread only. Borrowed objects must outlive the routing, and the routing every view using it.
 */
class EditorKeyRouting {
public:
    EditorKeyRouting(input::ActionRegistry& registry, input::KeymapService& keymap, EditorActionHost& host,
                     input::KeyPlatform platform, const IClock& clock = systemClock());
    ~EditorKeyRouting();
    EditorKeyRouting(const EditorKeyRouting&) = delete ("views and handlers point at the routing");
    EditorKeyRouting& operator=(const EditorKeyRouting&) = delete ("views and handlers point at the routing");

    /*
     * The standalone form over `layout` (copied by the KeymapService), resolving the built-in
     * IntelliJ keymap for `keymapPlatform` and translating keys with `keyPlatform`'s semantics.
     * Errors: KeymapService::create's, registerEditorActions'.
     */
    [[nodiscard]] static Result<std::unique_ptr<EditorKeyRouting>>
    createStandalone(const input::LayoutTable& layout, input::KeymapPlatform keymapPlatform,
                     input::KeyPlatform keyPlatform, const IClock& clock = systemClock());

    // Handles a KeyPress or KeyRelease for `view`. Returns true when the key was used.
    bool handleKey(EditorView& view, const QKeyEvent& event);
    // For QEvent::ShortcutOverride: true when the editor would use the key, so a QShortcut of
    // the window must not take it. Changes no match state.
    [[nodiscard]] bool wantsKey(EditorView& view, const QKeyEvent& event);
    // Tracks IME composition (QEvent::InputMethod, focus changes) and resets the matcher when a
    // composition starts.
    void observe(const QEvent& event);

    [[nodiscard]] input::ActionRegistry& registry() noexcept { return *m_registry; }
    [[nodiscard]] input::KeymapService& keymap() noexcept { return *m_keymap; }
    [[nodiscard]] EditorActionHost& host() noexcept { return *m_host; }
    [[nodiscard]] input::KeyPlatform platform() const noexcept { return m_platform; }

    // The level and trust actions are gated on (the shell keeps them live); keys are added per view.
    void setBaseContext(const input::ActionContext& context) noexcept { m_base = context; }
    [[nodiscard]] const input::ActionContext& baseContext() const noexcept { return m_base; }
    // baseContext() plus the view's keys.
    [[nodiscard]] input::ActionContext contextFor(const EditorView& view) const noexcept;

private:
    EditorKeyRouting() = default;
    [[nodiscard]] input::KeyEvent translate(const QKeyEvent& event) const noexcept;

    // The standalone form's own objects (declared first, destroyed last).
    std::unique_ptr<input::ActionRegistry> m_ownedRegistry;
    std::unique_ptr<input::MemoryKeymapSettings> m_ownedSettings;
    std::unique_ptr<EditorActionHost> m_ownedHost;
    std::optional<input::KeymapService> m_ownedKeymap;

    input::ActionRegistry* m_registry = nullptr;
    input::KeymapService* m_keymap = nullptr;
    EditorActionHost* m_host = nullptr;
    input::KeyPlatform m_platform = input::KeyPlatform::Windows;
    const IClock* m_clock = nullptr;
    ImeTracker m_ime;
    input::ActionContext m_base{};
};

} // namespace frost::ui
