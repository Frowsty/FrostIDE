// SPDX-License-Identifier: Apache-2.0
//
// KeymapService and MemoryKeymapSettings (keymap_service.hpp; ADR 0016 decision 11): the
// shell's facade over keymap, preferences, binding index, layout and matcher, and the
// in-memory settings double with fault injection.

#include "frost/input/keymap_service.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::input {

MemoryKeymapSettings::MemoryKeymapSettings(KeymapPreferences initial) : m_preferences(std::move(initial)) {}

std::optional<ErrorCode> MemoryKeymapSettings::takeFault(SettingsOp op) const {
    const auto it = std::ranges::find(m_faults, op, &Fault::op);
    if (it == m_faults.end()) {
        return std::nullopt;
    }
    const ErrorCode code = it->code;
    if (--it->remaining == 0) {
        m_faults.erase(it);
    }
    return code;
}

Result<KeymapPreferences> MemoryKeymapSettings::load() const {
    LockGuard lock(m_mutex);
    ++m_loads;
    if (const auto fault = takeFault(SettingsOp::Load)) {
        return fail(*fault, "injected keymap settings load fault");
    }
    return m_preferences;
}

Result<void> MemoryKeymapSettings::store(const KeymapPreferences& preferences) {
    LockGuard lock(m_mutex);
    ++m_stores;
    if (const auto fault = takeFault(SettingsOp::Store)) {
        return fail(*fault, "injected keymap settings store fault");
    }
    m_preferences = preferences;
    return {};
}

void MemoryKeymapSettings::injectFault(SettingsOp op, ErrorCode code, std::uint32_t count) {
    FROST_PRE(count >= 1);
    LockGuard lock(m_mutex);
    m_faults.push_back(Fault{op, code, count});
}

void MemoryKeymapSettings::set(KeymapPreferences preferences) {
    LockGuard lock(m_mutex);
    m_preferences = std::move(preferences);
}

KeymapPreferences MemoryKeymapSettings::current() const {
    LockGuard lock(m_mutex);
    return m_preferences;
}

std::uint32_t MemoryKeymapSettings::loadCount() const {
    LockGuard lock(m_mutex);
    return m_loads;
}

std::uint32_t MemoryKeymapSettings::storeCount() const {
    LockGuard lock(m_mutex);
    return m_stores;
}

KeymapService::KeymapService(const ActionRegistry& registry, IKeymapSettings& settings, KeymapPlatform platform,
                             const LayoutTable& layout, IClock::Duration platformInterval, Keymap keymap)
    : m_registry(&registry), m_settings(&settings), m_platform(platform), m_layout(layout),
      m_platformInterval(platformInterval), m_keymap(std::move(keymap)) {}

Result<KeymapService> KeymapService::create(const ActionRegistry& registry, IKeymapSettings& settings,
                                            KeymapPlatform platform, const LayoutTable& layout,
                                            IClock::Duration platformDoubleTapInterval) {
    auto defaults = builtinKeymap("intellij");
    if (!defaults) {
        return fail(ErrorCode::Internal, std::format("the built-in keymap is broken: {}", defaults.error().message()));
    }
    std::optional<Error> problem;
    KeymapPreferences preferences;
    if (auto loaded = settings.load()) {
        preferences = sanitize(*loaded, problem);
    } else {
        problem = std::move(loaded.error());
    }
    Keymap keymap = std::move(*defaults);
    if (preferences.keymapId != keymap.id()) {
        auto chosen = builtinKeymap(preferences.keymapId);
        if (!chosen) {
            return fail(ErrorCode::Internal, std::format("built-in keymap '{}' is broken: {}", preferences.keymapId,
                                                         chosen.error().message()));
        }
        keymap = std::move(*chosen);
    }
    KeymapService service(registry, settings, platform, layout, platformDoubleTapInterval, Keymap{});
    service.apply(std::move(preferences), std::move(keymap));
    service.m_lastSettingsError = std::move(problem);
    return service;
}

void KeymapService::apply(KeymapPreferences preferences, Keymap keymap) {
    m_keymap = std::move(keymap);
    m_preferences = std::move(preferences);
    const IClock::Duration interval = m_preferences.doubleTapInterval > IClock::Duration::zero()
                                          ? m_preferences.doubleTapInterval
                                          : m_platformInterval;
    m_matcher.setOptions(MatcherOptions{interval});
    m_matcher.reset();
    rebuildIndex();
}

void KeymapService::setModuleDefaults(std::vector<Binding> defaults) {
    m_moduleDefaults = std::move(defaults);
    rebuildIndex();
}

void KeymapService::rebuildIndex() {
    if (m_moduleDefaults.empty()) {
        m_index = BindingIndex::build(m_keymap, m_platform, m_preferences.overrides, *m_registry);
    } else {
        // Module defaults become overrides placed before the user's, for actions the keymap does not
        // name; the user's later overrides replace them (BindingIndex applies overrides in order).
        std::vector<std::string_view> named;
        const std::vector<Binding> resolved = m_keymap.bindings(m_platform);
        named.reserve(resolved.size());
        for (const Binding& binding : resolved) {
            named.push_back(binding.actionId);
        }
        std::ranges::sort(named);
        std::vector<ActionOverride> overrides;
        overrides.reserve(m_moduleDefaults.size() + m_preferences.overrides.size());
        for (const Binding& binding : m_moduleDefaults) {
            if (!std::ranges::binary_search(named, std::string_view(binding.actionId))) {
                overrides.push_back(ActionOverride{binding.actionId, binding.sequences});
            }
        }
        overrides.insert(overrides.end(), m_preferences.overrides.begin(), m_preferences.overrides.end());
        m_index = BindingIndex::build(m_keymap, m_platform, overrides, *m_registry);
    }
    m_indexRegistryGeneration = m_registry->generation();
    ++m_bindingsGeneration;
}

void KeymapService::ensureIndex() noexcept {
    if (m_registry->generation() != m_indexRegistryGeneration) {
        rebuildIndex(); // Allocates: the documented exception on the keystroke path.
    }
}

MatchResult KeymapService::handle(const KeyEvent& event, const ActionContext& context) noexcept {
    ensureIndex();
    const ActionRegistry& registry = *m_registry;
    const auto gate = [&](ActionId id) { return registry.isEnabled(id, context); };
    return m_matcher.feed(event, m_index, m_layout, gate);
}

MatchOutcome KeymapService::peek(const KeyEvent& event, const ActionContext& context) noexcept {
    ensureIndex();
    const ActionRegistry& registry = *m_registry;
    const auto gate = [&](ActionId id) { return registry.isEnabled(id, context); };
    return m_matcher.peek(event, m_index, m_layout, gate);
}

void KeymapService::setLayout(const LayoutTable& layout) noexcept {
    m_layout = layout;
    m_matcher.reset();
    // Display text and reachability depend on the layout; consumers re-query.
    ++m_bindingsGeneration;
}

Result<void> KeymapService::setPreferences(KeymapPreferences preferences) {
    if (auto valid = validate(preferences); !valid) {
        return valid;
    }
    auto keymap = builtinKeymap(preferences.keymapId);
    if (!keymap) {
        return std::unexpected(std::move(keymap.error()));
    }
    if (auto stored = m_settings->store(preferences); !stored) {
        return stored;
    }
    apply(std::move(preferences), std::move(*keymap));
    m_lastSettingsError.reset();
    return {};
}

void KeymapService::reloadPreferences() {
    auto loaded = m_settings->load();
    if (!loaded) {
        m_lastSettingsError = std::move(loaded.error());
        return;
    }
    std::optional<Error> problem;
    KeymapPreferences preferences = sanitize(*loaded, problem);
    auto keymap = builtinKeymap(preferences.keymapId);
    if (!keymap) {
        m_lastSettingsError = std::move(keymap.error());
        return;
    }
    apply(std::move(preferences), std::move(*keymap));
    m_lastSettingsError = std::move(problem);
}

std::span<const Sequence> KeymapService::shortcutsFor(ActionId id) noexcept {
    ensureIndex();
    return m_index.shortcutsFor(id);
}

std::optional<Sequence> KeymapService::primaryShortcut(ActionId id) {
    ensureIndex();
    return m_index.primaryShortcut(id, m_layout);
}

const BindingIndex& KeymapService::bindings() noexcept {
    ensureIndex();
    return m_index;
}

std::vector<Sequence> KeymapService::unreachableBindings() {
    ensureIndex();
    return m_index.unreachable(m_layout);
}

} // namespace frost::input
