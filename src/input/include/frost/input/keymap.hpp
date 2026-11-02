// SPDX-License-Identifier: Apache-2.0
//
// Keymaps (PLAN.md row 227, ADR 0016 decision 8): the keymap file format v1, the built-in
// IntelliJ-compatible keymap, per-platform resolution, user overrides and their
// validation, display text, and BindingIndex, the immutable lookup structure the shortcut
// matcher searches.
/*
 * Format v1 (UTF-8, optional BOM, LF or CRLF line ends; strict):
 *
 *   # full-line comments only (optional leading whitespace, then '#')
 *   frost-keymap 1                     the first line that is neither blank nor a comment
 *   id intellij                        [a-z][a-z0-9-]*, at most 64 bytes; exactly once
 *   name IntelliJ-compatible           1-128 bytes; exactly once (both before any section)
 *   [default]                          sections [default], [windows], [linux], [macos],
 *   editor.copy = Ctrl+C | Ctrl+Insert   each at most once; bindings only inside one
 *   agent.terminal =                   explicitly unbound on that platform
 *
 * Bindings read `<action-id> = <sequence> { | <sequence> }`. Spaces or tabs are allowed
 * around '=' and '|' and at the end of a line; each sequence uses the Strict syntax of
 * parseSequence. Action ids match [a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9]*)+ (at most 64
 * bytes); an action appears at most once per section and a sequence at most once per
 * action. Input is at most 1 MiB and a line at most 4096 bytes.
 *
 * Resolution for platform P: an action listed in [P] takes its [P] line, otherwise its
 * [default] line; on macOS only, [default] lines not listed in [macos] are derived by
 * swapping Ctrl and Meta in every stroke (gestures are unchanged). Order: [default] file
 * order, then actions that appear only in [P], in [P] file order.
 *
 * Action ids follow ADR 0016's convention: a group (editor, code, search, navigate,
 * refactor, window, file, view, vcs, run, debug, build, bookmarks, tools, agent) and a
 * camelCase name. A keymap may name actions later milestones register; those bindings
 * are inert until the action exists (BindingIndex::unresolved lists them).
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/input/action.hpp"
#include "frost/input/key.hpp"
#include "frost/input/layout.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::input {

// The platform a keymap is resolved for. MacOS is defined but untested (D24).
enum class KeymapPlatform : std::uint8_t { Windows, Linux, MacOS };

// The platform of this build: MacOS only under __APPLE__, which is unsupported (D24).
[[nodiscard]] KeymapPlatform currentKeymapPlatform() noexcept;

/*
 * English display text for logs, tests, menus and Find Action: "Ctrl+Shift+A",
 * "Ctrl+K, Ctrl+C", "Double Shift". Modifiers read Ctrl, Alt, Shift and Win (Windows),
 * Super (Linux) or Cmd (macOS, where Alt reads Option). A writing key shows its label on
 * `layout` (labelFor, uppercased with simpleUpper: Ctrl+BracketLeft is "Ctrl+Ü" on
 * de-DE), its US character when unreachable, and digit-row keys their digit ("Alt+1" on
 * AZERTY). Named keys read Enter, Esc, Page Up, Left and so on; numpad keys "Num /",
 * "Num 0". UTF-8. Modifier and named-key names go through frost::i18n::tr (context
 * "frost::input::keys"), so they are English unless a translator or pseudo-localization is on;
 * key labels, digits and F-keys are never translated.
 */
[[nodiscard]] std::string toDisplayText(const Sequence& sequence, const LayoutTable& layout, KeymapPlatform platform);

// One action's shortcuts in a section or a resolved keymap. An empty list is an explicit
// unbinding.
struct Binding {
    std::string actionId;
    std::vector<Sequence> sequences;
    friend bool operator==(const Binding&, const Binding&) = default;
};

enum class KeymapSection : std::uint8_t { Default, Windows, Linux, MacOS };

// A parsed keymap. Immutable after parsing and safe to share read-only.
class Keymap {
public:
    [[nodiscard]] const std::string& id() const noexcept { return m_id; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] bool hasSection(KeymapSection section) const noexcept {
        return m_present[static_cast<std::size_t>(section)];
    }
    // The section's lines in file order (empty when absent).
    [[nodiscard]] std::span<const Binding> section(KeymapSection section) const noexcept {
        return m_sections[static_cast<std::size_t>(section)];
    }
    // The resolved bindings for `platform` (see the file comment), explicit unbindings
    // included with an empty list. O(B log B).
    [[nodiscard]] std::vector<Binding> bindings(KeymapPlatform platform) const;

    friend bool operator==(const Keymap&, const Keymap&) = default;

private:
    friend Result<Keymap> parseKeymap(std::string_view text);

    std::string m_id;
    std::string m_name;
    std::array<std::vector<Binding>, 4> m_sections;
    std::array<bool, 4> m_present{};
};

// The most sequences one keymap line may list (as for an override, kMaxSequencesPerOverride).
inline constexpr std::size_t kMaxSequencesPerBinding = 8;

/*
 * Parses format v1. Errors: Corrupt "keymap line L, column C: <reason>" (invalid UTF-8,
 * NUL or control bytes, a missing or duplicate header, bad directives, unknown or
 * duplicated sections, a directive after a section, a binding outside a section,
 * duplicates, bad sequences, more than kMaxSequencesPerBinding sequences on a line, a
 * line over 4096 bytes); Unsupported for another format version; OutOfRange for input
 * over 1 MiB, or input whose canonical form (formatKeymap) would be over 1 MiB, so that
 * every parsed keymap formats to text this function accepts. O(n log B) for n bytes and
 * B bindings.
 */
[[nodiscard]] Result<Keymap> parseKeymap(std::string_view text);
// The canonical text: header, id, name, then the present sections in the order default,
// windows, linux, macos, each line as `id = A | B` (or `id =`), LF line ends, no comments.
// For every k parseKeymap returned, parseKeymap(formatKeymap(k)) == k, and formatting is
// a fixpoint: canonical lines fit 4096 bytes and the canonical text 1 MiB.
[[nodiscard]] std::string formatKeymap(const Keymap& keymap);

// Parses the built-in keymap `id` from its embedded text on each call (no statics; the
// caller keeps the result). Errors: NotFound for unknown ids; Internal if the embedded
// text does not parse. Budget: B1 (KeymapService::create may run during start-up; at most
// 1 ms in Release).
[[nodiscard]] Result<Keymap> builtinKeymap(std::string_view id);
// {"intellij"}. Valid for the program's lifetime.
[[nodiscard]] std::span<const std::string_view> builtinKeymapIds() noexcept;
// The embedded source text of a built-in keymap ("" for unknown ids), for tests.
[[nodiscard]] std::string_view builtinKeymapText(std::string_view id) noexcept;

// A user override: replaces the action's whole shortcut list; an empty list removes all.
struct ActionOverride {
    std::string actionId;
    std::vector<Sequence> sequences;
    friend bool operator==(const ActionOverride&, const ActionOverride&) = default;
};

/*
 * The keymap preferences stored in application-scope settings (never project-shared,
 * none an `executes` key): keymap.id, keymap.doubleTapIntervalMs and keymap.overrides (an
 * array of {action, shortcuts: [Strict sequence strings]}). A doubleTapInterval of 0
 * means "use the platform interval".
 */
struct KeymapPreferences {
    std::string keymapId = "intellij";
    IClock::Duration doubleTapInterval{};
    std::vector<ActionOverride> overrides;
    friend bool operator==(const KeymapPreferences&, const KeymapPreferences&) = default;
};

// Limits checked by validate().
inline constexpr IClock::Duration kMaxDoubleTapInterval = std::chrono::seconds(2);
inline constexpr std::size_t kMaxOverrides = 4096;
inline constexpr std::size_t kMaxSequencesPerOverride = 8;

// Checks `preferences` strictly: a known built-in keymap id, an interval in [0, 2 s], at
// most 4096 overrides with unique, well-formed action ids, and at most 8 valid,
// non-duplicate sequences each. Errors: InvalidArgument naming the first bad entry.
[[nodiscard]] Result<void> validate(const KeymapPreferences& preferences);
// Keeps every valid entry, drops invalid overrides (and duplicates after the first), and
// resets an invalid interval or keymap id to its default. `firstProblem` receives the
// first problem found (unchanged when there is none).
[[nodiscard]] KeymapPreferences sanitize(const KeymapPreferences& preferences, std::optional<Error>& firstProblem);

// One sequence bound to several actions, or a single-stroke binding equal to a
// two-stroke prefix (prefixConflict; the prefix wins at match time).
struct Conflict {
    Sequence sequence;
    std::vector<ActionId> actions;
    bool prefixConflict = false;
    friend bool operator==(const Conflict&, const Conflict&) = default;
};

/*
 * The matcher's lookup structure: every resolved binding of registered actions, with
 * overrides applied, sorted by (first stroke, second stroke, action priority descending,
 * keymap order). Immutable; safe to share read-only. Every accessor returns a span into
 * vectors this object owns, valid while it lives.
 */
class BindingIndex {
public:
    BindingIndex() = default;

    // Resolves `keymap` for `platform`, applies `overrides` (each replaces its action's
    // whole list) and indexes the bindings of actions registered in `registry`; ids with
    // no registered action are listed by unresolved(). O(B log B).
    [[nodiscard]] static BindingIndex build(const Keymap& keymap, KeymapPlatform platform,
                                            std::span<const ActionOverride> overrides, const ActionRegistry& registry);

    // Actions bound to the single stroke `stroke`. O(log B). Budget: B5.
    [[nodiscard]] std::span<const ActionId> single(Stroke stroke) const noexcept;
    // Actions bound to a two-stroke sequence starting with `first`. O(log B). Budget: B5.
    [[nodiscard]] std::span<const ActionId> prefixed(Stroke first) const noexcept;
    // Actions bound to `first, second`. O(log B). Budget: B5.
    [[nodiscard]] std::span<const ActionId> second(Stroke first, Stroke second) const noexcept;
    // Actions bound to `gesture`, in priority order. O(1).
    [[nodiscard]] std::span<const ActionId> gesture(Gesture gesture) const noexcept;
    // The action's sequences in keymap order (the first is the keymap's primary). O(1).
    [[nodiscard]] std::span<const Sequence> shortcutsFor(ActionId id) const noexcept;
    // Action ids named by the keymap or the overrides that have no registered action, sorted.
    [[nodiscard]] std::span<const std::string> unresolved() const noexcept { return m_unresolved; }

    // Every conflict, sorted by sequence. O(B log B).
    [[nodiscard]] std::vector<Conflict> conflicts() const;
    // The bound sequences with a stroke whose key `layout` cannot reach, sorted and unique.
    [[nodiscard]] std::vector<Sequence> unreachable(const LayoutTable& layout) const;
    // The action's first sequence whose keys are all reachable on `layout`.
    [[nodiscard]] std::optional<Sequence> primaryShortcut(ActionId id, const LayoutTable& layout) const;
    // Number of indexed (action, sequence) pairs.
    [[nodiscard]] std::size_t size() const noexcept {
        return m_singleActions.size() + m_doubleActions.size() + m_gestureActions[0].size() +
               m_gestureActions[1].size();
    }

private:
    struct Range {
        std::uint32_t begin = 0;
        std::uint32_t count = 0;
    };

    std::vector<Stroke> m_singleKeys;
    std::vector<ActionId> m_singleActions;
    std::vector<std::pair<Stroke, Stroke>> m_doubleKeys;
    std::vector<ActionId> m_doubleActions;
    std::array<std::vector<ActionId>, 2> m_gestureActions; // DoubleShift, DoubleCtrl.
    std::vector<Sequence> m_sequences;                     // Per action, keymap order.
    std::vector<Range> m_ranges;                           // Indexed by ActionId value.
    std::vector<std::string> m_unresolved;
};

} // namespace frost::input
