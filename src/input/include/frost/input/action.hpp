// SPDX-License-Identifier: Apache-2.0
//
// The action registry (PLAN.md section 3.3, ADR 0016 decision 9): every command the IDE
// can run, with its id, English name, synonyms, context rule, availability-level and
// trust gating, and its handler. Keymaps, menus and Find Action all refer to actions by id.
/*
 * Gating. An action is applicable when its ContextRule matches the focus context;
 * inapplicable actions are excluded from Find Action and never fire. An applicable action
 * may still be disabled, for one reason reported by whyDisabled() in this order:
 * NotApplicable, Level (the availability level is below requiredLevel), Untrusted
 * (executesProjectCode and the project is not trusted, PLAN.md sections 3.3 and 3.5),
 * NoHandler (a placeholder registered without a handler), Callback (its enabled callback
 * said no). Disabled actions stay visible (Find Action lists them with the reason) but
 * never run: tryInvoke() checks the same gates the matcher's gate does, so every
 * InvokeSource (keyboard, menu, Find Action, automation) is gated alike. The registry
 * trusts the ActionContext it is given: callers must build `trusted` and `level` from the
 * live trust and level state at the moment of the call, never from a stale capture.
 *
 * Callbacks. Handlers and enabled callbacks must not throw: they run inside noexcept
 * paths (the matcher's gate, tryInvoke), where a throw terminates. Enabled callbacks must
 * not mutate the registry or re-enter KeymapService; add() checks the first with a
 * precondition. Handlers may call add() (registering more actions), which invalidates
 * spans from all() and increments generation().
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::input {

// Identifies an action within the registry that issued it; valid for that registry's lifetime.
using ActionId = StrongIndex<struct ActionIdTag, std::uint32_t>;
inline constexpr ActionId kNoAction{UINT32_MAX};

// Context flags an action's rule tests. At most 64; append-only (values may be persisted
// by later milestones' context tests).
enum class ContextKey : std::uint8_t {
    EditorFocus,
    EditorReadOnly,
    EditorHasSelection,
    ToolWindowFocus,
    ProjectViewFocus,
    TerminalFocus,
    TextFieldFocus,
    PopupFocus,
    DialogFocus,
    LookupActive,
    ProjectOpen,
    Debugging,
};

// A set of ContextKeys (64-bit). O(1) operations.
class ContextKeys {
public:
    constexpr ContextKeys() noexcept = default;
    constexpr ContextKeys(std::initializer_list<ContextKey> keys) noexcept {
        for (const ContextKey key : keys) {
            m_bits |= bit(key);
        }
    }
    [[nodiscard]] static constexpr ContextKeys all() noexcept {
        ContextKeys keys;
        keys.m_bits = ~std::uint64_t{0};
        return keys;
    }
    [[nodiscard]] constexpr bool has(ContextKey key) const noexcept { return (m_bits & bit(key)) != 0; }
    constexpr ContextKeys& set(ContextKey key, bool on = true) noexcept {
        m_bits = on ? (m_bits | bit(key)) : (m_bits & ~bit(key));
        return *this;
    }
    // True when every key of `other` is in this set.
    [[nodiscard]] constexpr bool containsAll(ContextKeys other) const noexcept {
        return (m_bits & other.m_bits) == other.m_bits;
    }
    // True when this set and `other` share a key.
    [[nodiscard]] constexpr bool intersects(ContextKeys other) const noexcept { return (m_bits & other.m_bits) != 0; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_bits == 0; }
    [[nodiscard]] constexpr std::uint64_t bits() const noexcept { return m_bits; }
    friend constexpr bool operator==(ContextKeys, ContextKeys) noexcept = default;

private:
    [[nodiscard]] static constexpr std::uint64_t bit(ContextKey key) noexcept {
        return std::uint64_t{1} << static_cast<unsigned>(key);
    }
    std::uint64_t m_bits = 0;
};

// Applicable when the context holds every key of `all` and none of `none`. O(1).
struct ContextRule {
    ContextKeys all;
    ContextKeys none;
    [[nodiscard]] constexpr bool matches(ContextKeys context) const noexcept {
        return context.containsAll(all) && !context.intersects(none);
    }
    friend constexpr bool operator==(const ContextRule&, const ContextRule&) noexcept = default;
};

// Availability levels L0-L4 (PLAN.md section 4). TODO(M2): move to core when the level tracker lands.
enum class AvailabilityLevel : std::uint8_t { L0, L1, L2, L3, L4 };

// The state actions are gated on, captured by the shell at the time of the key press (or,
// for Find Action, the focus keys from when the popup was invoked). `level` and `trusted`
// always come from the live state when the context is built or reused.
struct ActionContext {
    ContextKeys keys;
    AvailabilityLevel level = AvailabilityLevel::L0;
    // The project root is trusted (PLAN.md section 3.5); false in restricted mode.
    bool trusted = false;
};

enum class InvokeSource : std::uint8_t { Keyboard, Menu, FindAction, Automation };

struct InvokeInfo {
    InvokeSource source = InvokeSource::Keyboard;
    bool autoRepeat = false;
    friend constexpr bool operator==(const InvokeInfo&, const InvokeInfo&) noexcept = default;
};

// Why an action is disabled, in the order whyDisabled() checks.
enum class DisabledReason : std::uint8_t { None, NotApplicable, Level, Untrusted, NoHandler, Callback };

[[nodiscard]] std::string_view toString(DisabledReason reason) noexcept;

// Everything an action is registered with.
struct ActionSpec {
    // "group.camelCaseName" (pattern [a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9]*)+, at most 64 bytes).
    std::string id;
    // English source text (translated by the UI later); 1-256 bytes.
    std::string name;
    std::string description;
    // The Find Action category, e.g. "Edit".
    std::string group;
    // At most 32, each 1-256 bytes.
    std::vector<std::string> synonyms;
    ContextRule context;
    AvailabilityLevel requiredLevel = AvailabilityLevel::L0;
    // Gated by ActionContext::trusted (PLAN.md sections 3.3 and 3.5).
    bool executesProjectCode = false;
    // Higher wins among actions sharing a shortcut in the same context.
    int priority = 0;
    // false hides the action from Find Action.
    bool searchable = true;
    // Empty: enabled whenever no other reason disables it. Must not throw, mutate the
    // registry or re-enter KeymapService.
    std::move_only_function<bool(const ActionContext&) const> enabled;
    // Empty: a placeholder, disabled with NoHandler. Must not throw.
    std::move_only_function<void(const ActionContext&, const InvokeInfo&)> handler;
};

// An action's registered description: ActionSpec without the callbacks.
struct ActionInfo {
    std::string id;
    std::string name;
    std::string description;
    std::string group;
    std::vector<std::string> synonyms;
    ContextRule context;
    AvailabilityLevel requiredLevel = AvailabilityLevel::L0;
    bool executesProjectCode = false;
    int priority = 0;
    bool searchable = true;
    bool hasHandler = false;
};

enum class InvokeResult : std::uint8_t { Ran, Disabled, NoHandler };

// True if `id` matches the action-id pattern [a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9]*)+ and
// is at most 64 bytes. O(n).
[[nodiscard]] bool isValidActionId(std::string_view id) noexcept;

/*
 * The registry. Not thread-safe: owned by the UI thread; handlers run on the caller's
 * thread. Entries live in a std::deque, so references from info() and the stored
 * callbacks never move; the id index is a sorted vector of (view of the stored id,
 * ActionId). There is no removal in v1, so ids stay valid for the registry's lifetime.
 */
class ActionRegistry {
public:
    ActionRegistry() = default;
    ActionRegistry(const ActionRegistry&) = delete ("actions own their callbacks and ids are per registry");
    ActionRegistry& operator=(const ActionRegistry&) = delete ("actions own their callbacks and ids are per registry");

    // Registers an action. Errors: AlreadyExists for a duplicate id; InvalidArgument for a
    // bad id, an empty or longer-than-256-byte name, or more than 32 or over-long
    // synonyms. Precondition: not called while an enabled callback or the matcher's gate
    // runs (allowed inside a handler). O(log n) search plus O(n) index insertion;
    // invalidates spans from all() and increments generation().
    [[nodiscard]] Result<ActionId> add(ActionSpec spec);

    // The action with string id `id`. O(log n).
    [[nodiscard]] std::optional<ActionId> find(std::string_view id) const noexcept;
    // Precondition: `id` was issued by this registry. The reference is valid for the
    // registry's lifetime. O(1).
    [[nodiscard]] const ActionInfo& info(ActionId id) const;

    // The context rule only. O(1).
    [[nodiscard]] bool isApplicable(ActionId id, const ActionContext& context) const noexcept;
    // The first reason the action is disabled in `context`, or None (see the file comment).
    // Runs the enabled callback last. O(1) plus the callback.
    [[nodiscard]] DisabledReason whyDisabled(ActionId id, const ActionContext& context) const noexcept;
    [[nodiscard]] bool isEnabled(ActionId id, const ActionContext& context) const noexcept {
        return whyDisabled(id, context) == DisabledReason::None;
    }
    // Runs the handler when the action is enabled; a disabled action is a normal outcome
    // (Disabled, or NoHandler for a placeholder), not an error.
    InvokeResult tryInvoke(ActionId id, const ActionContext& context, InvokeInfo info = {});

    // Every action in registration order; invalidated by add().
    [[nodiscard]] std::span<const ActionId> all() const noexcept { return m_order; }
    [[nodiscard]] std::size_t size() const noexcept { return m_order.size(); }
    // Incremented by every successful add().
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

private:
    struct Entry {
        ActionInfo info;
        std::move_only_function<bool(const ActionContext&) const> enabled;
        std::move_only_function<void(const ActionContext&, const InvokeInfo&)> handler;
    };

    std::deque<Entry> m_entries;
    std::vector<std::pair<std::string_view, ActionId>> m_index; // Sorted by id string.
    std::vector<ActionId> m_order;
    std::uint64_t m_generation = 0;
    // Depth of enabled callbacks running (add()'s precondition).
    mutable std::uint32_t m_callbackDepth = 0;
};

} // namespace frost::input
