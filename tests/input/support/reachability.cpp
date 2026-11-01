// SPDX-License-Identifier: Apache-2.0
//
// The per-action reachability analysis (reachability.hpp).

#include "input/support/reachability.hpp"

#include "frost/input/keymap.hpp"
#include "frost/input/shortcut_matcher.hpp"

#include "input/support/matcher_rig.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

namespace frost::input::test {

std::string_view gapKindName(GapKind kind) noexcept {
    switch (kind) {
    case GapKind::Unreachable: return "Unreachable";
    case GapKind::Misread: return "Misread";
    case GapKind::NoMatch: return "NoMatch";
    }
    return "?";
}

std::string_view keyPlatformName(KeyPlatform platform) noexcept {
    switch (platform) {
    case KeyPlatform::Windows: return "Windows";
    case KeyPlatform::X11: return "X11";
    case KeyPlatform::Wayland: return "Wayland";
    }
    return "?";
}

std::string ReachabilityReport::format() const {
    std::string out = std::format("{} {}: {} actions, {} sequences, {} gaps\n", layout, keyPlatformName(platform),
                                  actions, sequences, gaps.size());
    for (const ReachabilityGap& gap : gaps) {
        out += std::format("  gap {} {} {}: {}\n", gap.actionId, gap.sequence, gapKindName(gap.kind), gap.detail);
    }
    for (const std::string& action : numpadOnly) {
        out += std::format("  numpad-only {}\n", action);
    }
    for (const std::string& stroke : deadKeyStrokes) {
        out += std::format("  dead-key {}\n", stroke);
    }
    return out;
}

namespace {

bool contains(std::span<const ActionId> ids, ActionId id) { return std::ranges::find(ids, id) != ids.end(); }

bool usesNumpad(const Sequence& s) {
    for (std::size_t i = 0; i < s.length(); ++i) {
        if (isNumpadKey(s.stroke(i).key)) {
            return true;
        }
    }
    return false;
}

// The gap of one stroke sequence (nullopt when it fires), recording dead-key presses.
std::optional<ReachabilityGap> checkStrokes(MatcherRig& rig, const Binding& binding, const Sequence& s, ActionId id,
                                            std::vector<std::string>& deadKeyStrokes) {
    const auto gap = [&](GapKind kind, std::string detail) {
        return ReachabilityGap{binding.actionId, formatSequence(s), kind, std::move(detail)};
    };
    rig.resetKeyboard();
    for (std::size_t i = 0; i < s.length(); ++i) {
        const Stroke stroke = s.stroke(i);
        const auto code = physicalCodeFor(stroke.key, *rig.truth, rig.platform, stroke.mods.has(Modifier::Shift));
        if (!code) {
            return gap(GapKind::Unreachable, std::format("no key maps to {}", keyName(stroke.key)));
        }
        const Step step = rig.stroke(stroke.mods, *code);
        if (step.keyEvent.deadKey != 0) {
            deadKeyStrokes.push_back(formatStroke(stroke) + " " + binding.actionId);
        }
        const std::optional<Stroke> read = strokeFromEvent(step.keyEvent, *rig.table);
        if (read != stroke) {
            return gap(GapKind::Misread,
                       std::format("{} read as {}", codeName(*code), read ? formatStroke(*read) : "nothing"));
        }
        const bool last = i + 1 == s.length();
        const bool prefix = !rig.index.prefixed(stroke).empty();
        const MatchOutcome outcome = step.keyResult.outcome;
        if (!last || (s.length() == 1 && prefix)) {
            if (outcome != MatchOutcome::Pending) {
                return gap(GapKind::NoMatch,
                           std::format("{} gave {}, not Pending", codeName(*code), toString(outcome)));
            }
            continue;
        }
        const std::span<const ActionId> candidates =
            s.length() == 1 ? rig.index.single(stroke) : rig.index.second(s.first(), stroke);
        if (outcome != MatchOutcome::Fire || !contains(candidates, id)) {
            return gap(GapKind::NoMatch, std::format("{} gave {}{}", codeName(*code), toString(outcome),
                                                     outcome == MatchOutcome::Fire ? " of another action" : ""));
        }
    }
    return std::nullopt;
}

std::optional<ReachabilityGap> checkGesture(MatcherRig& rig, const Binding& binding, const Sequence& s, ActionId id) {
    rig.resetKeyboard();
    const Code tapped = s.gesture() == Gesture::DoubleShift ? Code::ShiftLeft : Code::ControlLeft;
    static_cast<void>(rig.tap(tapped));
    const Step second = rig.tap(tapped);
    const std::span<const ActionId> candidates = rig.index.gesture(s.gesture());
    if (second.fire.outcome != MatchOutcome::Fire || !contains(candidates, second.fire.action) ||
        !contains(candidates, id)) {
        return ReachabilityGap{binding.actionId, formatSequence(s), GapKind::NoMatch,
                               std::format("two taps of {} did not fire", codeName(tapped))};
    }
    return std::nullopt;
}

} // namespace

ReachabilityReport analyzeReachability(const LayoutTable& truth, KeyPlatform platform, std::string_view keymapText,
                                       std::string_view name) {
    MatcherRig rig(truth, platform, keymapText);
    ReachabilityReport report;
    report.layout = std::string(name.empty() ? truth.id() : name);
    report.platform = platform;
    for (const Binding& binding : rig.keymap.bindings(keymapPlatformFor(platform))) {
        if (binding.sequences.empty()) {
            continue;
        }
        const ActionId id = rig.idOf(binding.actionId);
        ++report.actions;
        bool anyReachable = false;
        bool allNumpad = true;
        for (const Sequence& s : binding.sequences) {
            ++report.sequences;
            const std::optional<ReachabilityGap> gap = s.isGesture()
                                                           ? checkGesture(rig, binding, s, id)
                                                           : checkStrokes(rig, binding, s, id, report.deadKeyStrokes);
            if (gap) {
                report.gaps.push_back(*gap);
                continue;
            }
            anyReachable = true;
            allNumpad = allNumpad && usesNumpad(s);
        }
        if (anyReachable && allNumpad) {
            report.numpadOnly.push_back(binding.actionId);
        }
    }
    std::ranges::sort(report.gaps, [](const ReachabilityGap& a, const ReachabilityGap& b) {
        return std::tie(a.actionId, a.sequence) < std::tie(b.actionId, b.sequence);
    });
    std::ranges::sort(report.numpadOnly);
    std::ranges::sort(report.deadKeyStrokes);
    const auto [first, last] = std::ranges::unique(report.deadKeyStrokes);
    report.deadKeyStrokes.erase(first, last);
    return report;
}

} // namespace frost::input::test
