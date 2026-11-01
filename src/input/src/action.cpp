// SPDX-License-Identifier: Apache-2.0
//
// ActionRegistry (action.hpp; ADR 0016 decision 9): registration and validation, lookup,
// and the gating shared by the matcher, menus, Find Action and automation, including the
// trust gate for actions that execute project code (PLAN.md sections 3.3 and 3.5).

#include "frost/input/action.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace frost::input {

namespace {

constexpr std::size_t kMaxActionIdBytes = 64;
constexpr std::size_t kMaxNameBytes = 256;
constexpr std::size_t kMaxSynonyms = 32;

bool isLower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool isAlnum(char c) noexcept { return isLower(c) || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

} // namespace

std::string_view toString(DisabledReason reason) noexcept {
    switch (reason) {
    case DisabledReason::None: return "None";
    case DisabledReason::NotApplicable: return "NotApplicable";
    case DisabledReason::Level: return "Level";
    case DisabledReason::Untrusted: return "Untrusted";
    case DisabledReason::NoHandler: return "NoHandler";
    case DisabledReason::Callback: return "Callback";
    }
    return "DisabledReason(?)";
}

bool isValidActionId(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxActionIdBytes) {
        return false;
    }
    // [a-z][a-zA-Z0-9]* segments separated by single dots; at least two segments.
    std::size_t segments = 0;
    std::size_t i = 0;
    while (true) {
        if (i >= id.size() || !isLower(id[i])) {
            return false;
        }
        ++i;
        while (i < id.size() && isAlnum(id[i])) {
            ++i;
        }
        ++segments;
        if (i == id.size()) {
            return segments >= 2;
        }
        if (id[i] != '.') {
            return false;
        }
        ++i;
    }
}

Result<ActionId> ActionRegistry::add(ActionSpec spec) {
    FROST_PRE(m_callbackDepth == 0); // Not from an enabled callback or the matcher's gate.
    if (!isValidActionId(spec.id)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("invalid action id '{}' (pattern [a-z][a-zA-Z0-9]*(.[a-z][a-zA-Z0-9]*)+, at most 64 "
                                "bytes)",
                                spec.id));
    }
    if (spec.name.empty() || spec.name.size() > kMaxNameBytes) {
        return fail(ErrorCode::InvalidArgument, std::format("action '{}': a name is 1-256 bytes", spec.id));
    }
    if (spec.synonyms.size() > kMaxSynonyms) {
        return fail(ErrorCode::InvalidArgument, std::format("action '{}': at most 32 synonyms", spec.id));
    }
    for (const std::string& synonym : spec.synonyms) {
        if (synonym.empty() || synonym.size() > kMaxNameBytes) {
            return fail(ErrorCode::InvalidArgument, std::format("action '{}': a synonym is 1-256 bytes", spec.id));
        }
    }
    const auto at =
        std::ranges::lower_bound(m_index, std::string_view(spec.id), {}, &std::pair<std::string_view, ActionId>::first);
    if (at != m_index.end() && at->first == spec.id) {
        return fail(ErrorCode::AlreadyExists, std::format("action '{}' is already registered", spec.id));
    }
    const auto position = at - m_index.begin();

    const ActionId id{static_cast<std::uint32_t>(m_entries.size())};
    Entry& entry = m_entries.emplace_back();
    entry.info.id = std::move(spec.id);
    entry.info.name = std::move(spec.name);
    entry.info.description = std::move(spec.description);
    entry.info.group = std::move(spec.group);
    entry.info.synonyms = std::move(spec.synonyms);
    entry.info.context = spec.context;
    entry.info.requiredLevel = spec.requiredLevel;
    entry.info.executesProjectCode = spec.executesProjectCode;
    entry.info.priority = spec.priority;
    entry.info.searchable = spec.searchable;
    entry.info.hasHandler = static_cast<bool>(spec.handler);
    entry.enabled = std::move(spec.enabled);
    entry.handler = std::move(spec.handler);

    // The view points into the deque element, which never moves.
    m_index.insert(m_index.begin() + position, {std::string_view(entry.info.id), id});
    m_order.push_back(id);
    ++m_generation;
    return id;
}

std::optional<ActionId> ActionRegistry::find(std::string_view id) const noexcept {
    const auto at = std::ranges::lower_bound(m_index, id, {}, &std::pair<std::string_view, ActionId>::first);
    if (at != m_index.end() && at->first == id) {
        return at->second;
    }
    return std::nullopt;
}

const ActionInfo& ActionRegistry::info(ActionId id) const {
    FROST_PRE(id.value() < m_entries.size());
    return m_entries[id.value()].info;
}

bool ActionRegistry::isApplicable(ActionId id, const ActionContext& context) const noexcept {
    return id.value() < m_entries.size() && m_entries[id.value()].info.context.matches(context.keys);
}

DisabledReason ActionRegistry::whyDisabled(ActionId id, const ActionContext& context) const noexcept {
    if (!isApplicable(id, context)) {
        return DisabledReason::NotApplicable;
    }
    const Entry& entry = m_entries[id.value()];
    if (context.level < entry.info.requiredLevel) {
        return DisabledReason::Level;
    }
    // The trust gate (PLAN.md sections 3.3 and 3.5): actions that execute project code
    // never run for an untrusted project, whatever invokes them.
    if (entry.info.executesProjectCode && !context.trusted) {
        return DisabledReason::Untrusted;
    }
    if (!entry.handler) {
        return DisabledReason::NoHandler;
    }
    if (entry.enabled) {
        ++m_callbackDepth;
        const bool enabled = entry.enabled(context);
        --m_callbackDepth;
        if (!enabled) {
            return DisabledReason::Callback;
        }
    }
    return DisabledReason::None;
}

InvokeResult ActionRegistry::tryInvoke(ActionId id, const ActionContext& context, InvokeInfo info) {
    const DisabledReason reason = whyDisabled(id, context);
    if (reason == DisabledReason::NoHandler) {
        return InvokeResult::NoHandler;
    }
    if (reason != DisabledReason::None) {
        return InvokeResult::Disabled;
    }
    // The handler may call add(); the deque keeps this entry (and the callable) in place.
    m_entries[id.value()].handler(context, info);
    return InvokeResult::Ran;
}

} // namespace frost::input
