// SPDX-License-Identifier: Apache-2.0
//
// The per-action reachability analysis of the keymap suites (ADR 0030 decision 4): drives
// every sequence of every action of a keymap through the simulated keyboard and the matcher
// on one truth layout and platform, and reports the sequences that cannot be typed, are
// read as another stroke, or do not fire their action. KeymapReachability asserts de-DE and
// en-US complete with it and reports pl-PL and fr-FR. Test support, not product API.
// TODO(M1): in the hardening phase (ADR 0031), the deferred xkb layout oracle (ADR 0030) reruns
// it on xkb-derived truth tables.
#pragma once

#include "frost/input/key_event.hpp"
#include "frost/input/layout.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frost::input::test {

enum class GapKind : std::uint8_t {
    Unreachable, // a stroke's logical key has no physical key on the product's table
    Misread,     // the press is read as another stroke (or as none)
    NoMatch      // the stroke reads right but the matcher does not fire the action (or go Pending)
};

// One sequence of one action that is not reachable.
struct ReachabilityGap {
    std::string actionId;
    std::string sequence; // formatSequence()
    GapKind kind = GapKind::Unreachable;
    std::string detail;
    friend bool operator==(const ReachabilityGap&, const ReachabilityGap&) = default;
};

struct ReachabilityReport {
    std::string layout;
    KeyPlatform platform = KeyPlatform::Windows;
    std::size_t actions = 0;                 // actions with at least one sequence on the platform
    std::size_t sequences = 0;               // their sequences
    std::vector<ReachabilityGap> gaps;       // sorted by (actionId, sequence)
    std::vector<std::string> numpadOnly;     // actions whose every reachable sequence uses a Numpad* key
    std::vector<std::string> deadKeyStrokes; // "Ctrl+Equal editor.foldExpand": the key is dead at the pressed level
    friend bool operator==(const ReachabilityReport&, const ReachabilityReport&) = default;

    // One text block for the golden: a header line with the counts, then the three lists.
    [[nodiscard]] std::string format() const;
};

/*
 * Resolves `keymapText` (the built-in IntelliJ keymap when empty) for
 * keymapPlatformFor(platform) and drives each sequence through a MatcherRig(truth,
 * platform), from a reset keyboard:
 *   - a gesture is two taps of ShiftLeft (DoubleShift) or ControlLeft (DoubleCtrl), and must
 *     fire, with the action among index.gesture();
 *   - each stroke is pressed on physicalCodeFor(key, truth, platform, Shift held) with its
 *     left modifier keys (none: Unreachable); strokeFromEvent of the press must equal the
 *     bound stroke (else Misread, with the stroke read); a first stroke of two must go
 *     Pending, and a final stroke must Fire with the action among the candidates (single()
 *     or second()); a single stroke that is also a prefix must go Pending (else NoMatch).
 * Conflicts between actions are not gaps (IntelliJKeymap.ConflictsOnWindowsAndLinux owns
 * them). `name` replaces truth.id() in the report when not empty. Deterministic;
 * O(sequences x events). Not thread-safe (it builds its own rig per call).
 */
[[nodiscard]] ReachabilityReport analyzeReachability(const LayoutTable& truth, KeyPlatform platform,
                                                     std::string_view keymapText = {}, std::string_view name = {});

// "Unreachable", "Misread", "NoMatch"; "Windows", "X11", "Wayland".
[[nodiscard]] std::string_view gapKindName(GapKind kind) noexcept;
[[nodiscard]] std::string_view keyPlatformName(KeyPlatform platform) noexcept;

} // namespace frost::input::test
