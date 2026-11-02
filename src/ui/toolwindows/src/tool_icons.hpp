// SPDX-License-Identifier: Apache-2.0
//
// Themed icons and colours for the tool windows (ADR 0039): QIcons from the current FrostStyle's
// icon atlas by id, cached per atlas set, and the theme's severity tints, with fallbacks when no
// FrostStyle is installed (Qt's own style in tests).
#pragma once

#include "frost/build/diagnostic.hpp"
#include "frost/ui/model/theme_registry.hpp"

#include <QColor>
#include <QIcon>

#include <string>
#include <string_view>
#include <unordered_map>

namespace frost::ui {
class IconAtlasSet;
} // namespace frost::ui

namespace frost::ui::detail {

/*
 * QIcons by atlas id through FrostStyle::current(); a null QIcon without a FrostStyle or for an id
 * the bake lacks. The icons follow theme switches on their own (FrostIconEngine reads the current
 * style when painting), so the cache is only dropped when the atlas set changes. GUI thread only;
 * O(1) per hit.
 */
class IconCache {
public:
    [[nodiscard]] QIcon get(std::string_view id);

private:
    const IconAtlasSet* m_atlas = nullptr;
    std::unordered_map<std::string, QIcon> m_icons;
};

// The tint the current theme gives `role`, or `fallback` without a FrostStyle.
[[nodiscard]] QColor iconRoleColor(IconRole role, QColor fallback);
// A theme colour, or `fallback` without a FrostStyle.
[[nodiscard]] QColor themeColor(ColorKey key, QColor fallback);
// The atlas icon of a severity: status.error, status.warning or status.info.
[[nodiscard]] std::string_view severityIconId(build::Severity severity) noexcept;
// The text colour of a severity's lines: the theme's Icon.Error or Icon.Warning tint; invalid for messages.
[[nodiscard]] QColor severityColor(build::Severity severity);

} // namespace frost::ui::detail
