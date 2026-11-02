// SPDX-License-Identifier: Apache-2.0
//
// The tool windows' themed icons and colours (tool_icons.hpp).

#include "tool_icons.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"

namespace frost::ui::detail {

QIcon IconCache::get(std::string_view id) {
    const FrostStyle* style = FrostStyle::current();
    if (style == nullptr) {
        return {};
    }
    if (style->icons().get() != m_atlas) {
        m_atlas = style->icons().get();
        m_icons.clear();
    }
    const std::string key(id);
    if (const auto it = m_icons.find(key); it != m_icons.end()) {
        return it->second;
    }
    QIcon icon = style->icon(id);
    m_icons.emplace(key, icon);
    return icon;
}

QColor iconRoleColor(IconRole role, QColor fallback) {
    const FrostStyle* style = FrostStyle::current();
    if (style == nullptr) {
        return fallback;
    }
    return QColor::fromRgba(style->theme().iconColor(role).argb);
}

QColor themeColor(ColorKey key, QColor fallback) {
    const FrostStyle* style = FrostStyle::current();
    return style == nullptr ? fallback : style->color(key);
}

std::string_view severityIconId(build::Severity severity) noexcept {
    switch (severity) {
    case build::Severity::Error: return icons::kStatusError;
    case build::Severity::Warning: return icons::kStatusWarning;
    case build::Severity::Message: return icons::kStatusInfo;
    }
    return icons::kStatusInfo;
}

QColor severityColor(build::Severity severity) {
    switch (severity) {
    case build::Severity::Error: return iconRoleColor(IconRole::Error, QColor(0xd0, 0x30, 0x30));
    case build::Severity::Warning: return iconRoleColor(IconRole::Warning, QColor(0xb0, 0x80, 0x00));
    case build::Severity::Message: return {};
    }
    return {};
}

} // namespace frost::ui::detail
