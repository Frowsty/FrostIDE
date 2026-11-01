// SPDX-License-Identifier: Apache-2.0
//
// Registry: key registration checks and lookups (registry.hpp).

#include "frost/settings/registry.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <format>

namespace frost::settings {

namespace {

[[nodiscard]] bool isLower(char c) noexcept { return c >= 'a' && c <= 'z'; }
[[nodiscard]] bool isAlnum(char c) noexcept { return isLower(c) || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

[[nodiscard]] std::string_view firstSegment(std::string_view key) noexcept { return key.substr(0, key.find('.')); }

} // namespace

std::string_view toString(Scope scope) noexcept {
    switch (scope) {
    case Scope::Default: return "default";
    case Scope::Application: return "application";
    case Scope::ProjectShared: return "project-shared";
    case Scope::ProjectPersonal: return "project-personal";
    case Scope::FileLevel: return "file";
    }
    return "Scope(?)";
}

bool isValidSettingKey(std::string_view key) noexcept {
    if (key.empty() || key.size() > 128) {
        return false;
    }
    std::size_t segments = 0;
    std::size_t start = 0;
    while (start <= key.size()) {
        const std::size_t end = std::min(key.find('.', start), key.size());
        const std::string_view segment = key.substr(start, end - start);
        if (segment.empty() || !isLower(segment.front()) || !std::ranges::all_of(segment, isAlnum)) {
            return false;
        }
        ++segments;
        start = end + 1;
    }
    return segments >= 2;
}

Result<void> Registry::add(KeySpec spec) {
    if (!isValidSettingKey(spec.key)) {
        return fail(ErrorCode::InvalidArgument, std::format("invalid setting key \"{}\"", spec.key));
    }
    if (spec.title.empty() || spec.description.empty() || spec.category.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{}: title, description and category are required", spec.key));
    }
    if (spec.scopes.has(Scope::Default) ||
        !(spec.scopes.has(Scope::Application) || spec.scopes.has(Scope::ProjectShared) ||
          spec.scopes.has(Scope::ProjectPersonal))) {
        return fail(ErrorCode::InvalidArgument, std::format("{}: needs a file scope and never Default", spec.key));
    }
    if (spec.schema.type == ValueType::Enum && spec.schema.options.empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("{}: an enum needs options", spec.key));
    }
    if (spec.schema.type == ValueType::RecordList && spec.schema.fields.empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("{}: a record list needs fields", spec.key));
    }
    if (auto ok = validateValue(spec.schema, spec.defaultValue); !ok) {
        return fail(ErrorCode::InvalidArgument, std::format("{}: the default {}", spec.key, ok.error().message()));
    }
    const auto it = std::ranges::lower_bound(m_index, spec.key, {}, &std::pair<std::string, std::size_t>::first);
    if (it != m_index.end() && it->first == spec.key) {
        return fail(ErrorCode::AlreadyExists, std::format("setting key \"{}\" is already registered", spec.key));
    }
    m_index.insert(it, {spec.key, m_keys.size()});
    m_keys.push_back(std::move(spec));
    return {};
}

std::optional<std::size_t> Registry::indexOf(std::string_view key) const noexcept {
    const auto it = std::ranges::lower_bound(m_index, key, {}, [](const std::pair<std::string, std::size_t>& entry) {
        return std::string_view(entry.first);
    });
    if (it != m_index.end() && it->first == key) {
        return it->second;
    }
    return std::nullopt;
}

const KeySpec* Registry::find(std::string_view key) const noexcept {
    const auto index = indexOf(key);
    return index ? &m_keys[*index] : nullptr;
}

const KeySpec& Registry::at(std::size_t index) const {
    FROST_PRE(index < m_keys.size());
    return m_keys[index];
}

bool Registry::knowsNamespace(std::string_view key) const noexcept {
    const std::string_view prefix = firstSegment(key);
    if (prefix.empty() || prefix.size() == key.size()) {
        return false;
    }
    const auto it = std::ranges::lower_bound(m_index, prefix, {}, [](const std::pair<std::string, std::size_t>& entry) {
        return std::string_view(entry.first);
    });
    return it != m_index.end() && firstSegment(it->first) == prefix;
}

} // namespace frost::settings
