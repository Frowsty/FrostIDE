// SPDX-License-Identifier: Apache-2.0
//
// The theme key registry: every colour key, metric key and icon role a FrostIDE theme
// defines, plus the contrast pairs the theme tests enforce (ADR 0015, decisions 2, 4 and 6).
// It is the single source of truth for the .theme.json compiler, the theme blob codec,
// FrostStyle and the tests. Qt-free.
//
/*
 * Key names follow the .theme.json "ui" key format ("Component.part.property"), taken from
 * the public IntelliJ Platform SDK theme documentation page ("Themes: Customize UI Theme
 * Properties", plugins.jetbrains.com/docs/intellij/themes-customize.html) as cited in
 * ADR 0015; no JetBrains theme file or colour value was consulted (PLAN.md risk L1).
 *
 * Tables are append-only and grouped by area with section comments (base, shell, editor),
 * so later slices add keys in their own area. Every enumerator is dense, starts at 0 and
 * ends with a kCount sentinel; entry i of each table describes key i. A new key needs a
 * fallback or a value in every shipped theme, and changes kRegistryHash, which makes every
 * compiled theme blob stale (they are rebuilt by the build).
 *
 * Reserved keys: List.background, List.foreground, Tree.background, Tree.foreground,
 * TextField.selectionBackground, TextField.selectionForeground, every ScrollBar.* colour,
 * Separator.color, Menu.separatorColor and StatusBar.foreground are compiled, stored in every
 * blob and validated, but no FrostStyle drawing path reads them yet (item views fill with the
 * palette's Base, line edits select with its Highlight, scroll bars and separators are
 * Fusion's, and the status bar arrives with m1/shell-frame). Overriding one changes nothing
 * on screen until the slice that draws it maps it, adding its contrast pairs then.
 *
 * The consteval checks at the end of this header prove at compile time that the tables are
 * consistent: entry i has key i, names are unique and well formed, fallback chains are
 * acyclic, every contrast pair names existing keys, and every pair whose background key may
 * be translucent names an opaque underlay. Everything here is constexpr data: no static
 * initialisation, thread-safe, and every lookup is allocation-free.
 */
#pragma once

#include "frost/ui/model/fnv1a.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace frost::ui {

enum class ColorKey : std::uint16_t {
    // --- base: surfaces ---
    MainWindowBackground,
    IslandBackground,
    IslandBorderColor,
    EditorBackground,
    ToolWindowBackground,
    PanelBackground,
    PopupBackground,
    PopupBorderColor,
    SeparatorColor,
    // --- base: text ---
    LabelForeground,
    LabelSecondaryForeground,
    LabelDisabledForeground,
    LinkForeground,
    // --- base: components ---
    ComponentBorderColor,
    ComponentFocusColor,
    ComponentDisabledBorderColor,
    ButtonBackground,
    ButtonForeground,
    ButtonBorderColor,
    ButtonHoverBackground,
    ButtonPressedBackground,
    ButtonDefaultBackground,
    ButtonDefaultForeground,
    ButtonDefaultBorderColor,
    ButtonDefaultHoverBackground,
    TextFieldBackground,
    TextFieldForeground,
    TextFieldBorderColor,
    TextFieldPlaceholderForeground,
    TextFieldSelectionBackground,
    TextFieldSelectionForeground,
    ComboBoxBackground,
    ComboBoxForeground,
    ComboBoxBorderColor,
    CheckBoxBackground,
    CheckBoxBorderColor,
    CheckBoxCheckedBackground,
    CheckBoxCheckmarkColor,
    ListBackground,
    ListForeground,
    ListHoverBackground,
    ListSelectionBackground,
    ListSelectionForeground,
    ListSelectionInactiveBackground,
    TreeBackground,
    TreeForeground,
    TreeHoverBackground,
    TreeSelectionBackground,
    TreeSelectionForeground,
    TreeSelectionInactiveBackground,
    TabbedPaneUnderlineColor,
    TabbedPaneHoverBackground,
    ScrollBarThumbColor,
    ScrollBarThumbHoverColor,
    ScrollBarTrackColor,
    ToolTipBackground,
    ToolTipForeground,
    ToolTipBorderColor,
    // --- shell ---
    MenuBackground,
    MenuForeground,
    MenuSelectionBackground,
    MenuSelectionForeground,
    MenuSeparatorColor,
    StatusBarForeground,
    // --- editor --- (m1/editor-view adds its keys here)
    kCount
};

enum class MetricKey : std::uint16_t {
    // --- base ---
    IslandArc,
    IslandGap,
    ComponentArc,
    ButtonArc,
    ComponentFocusWidth,
    ButtonHeight,
    TextFieldHeight,
    ListRowHeight,
    TreeRowHeight,
    MenuItemHeight,
    TabbedPaneTabHeight,
    StatusBarHeight,
    ScrollBarWidth,
    ComponentPaddingH,
    LayoutMargin,
    LayoutSpacing,
    IconSize,
    ToolbarIconSize,
    FontUiSize,
    FontSmallSize,
    WindowInactiveAlpha,
    // --- shell --- / --- editor ---
    kCount
};

// Tint roles of monochrome icons (icons.ColorPalette in .theme.json; ADR 0015 decision 2).
enum class IconRole : std::uint8_t { Default, Secondary, Accent, Error, Warning, Success, Disabled, Selected, kCount };

inline constexpr std::size_t kColorKeyCount = static_cast<std::size_t>(ColorKey::kCount);
inline constexpr std::size_t kMetricKeyCount = static_cast<std::size_t>(MetricKey::kCount);
inline constexpr std::size_t kIconRoleCount = static_cast<std::size_t>(IconRole::kCount);

// How a metric's value is interpreted: logical pixels (scaled by zoom) or a plain ratio.
enum class MetricUnit : std::uint8_t { Px, Ratio };

struct ColorKeyInfo {
    ColorKey key;
    std::string_view name;
    ColorKey fallback; // ColorKey::kCount: no fallback.
    bool opaque;       // The resolved value must have alpha 255.
};

struct MetricKeyInfo {
    MetricKey key;
    std::string_view name;
    float min, max;  // Inclusive range of valid values (normal and compact).
    bool hasCompact; // Compact mode may set a different value (otherwise compact == normal).
    MetricUnit unit;
    MetricKey fallback = MetricKey::kCount; // MetricKey::kCount: no fallback.
};

struct IconRoleInfo {
    IconRole role;
    std::string_view name; // "Icon.Default"
};

// What a contrast pair measures; minimumContrast (contrast.hpp) gives each kind's threshold.
enum class ContrastKind : std::uint8_t { IslandSeparation, Text, SecondaryText, DisabledText, NonText, Border };

struct ContrastPair {
    ColorKey foreground, background;
    ContrastKind kind;
    // The opaque surface beneath a translucent background; required (and checked below) when the
    // background key is not opaque.
    ColorKey underlay = ColorKey::kCount;
};

struct IconContrastPair {
    IconRole role;
    ColorKey background;
    ContrastKind kind;
    ColorKey underlay = ColorKey::kCount; // As ContrastPair::underlay.
};

namespace detail {
inline constexpr ColorKey kNoColor = ColorKey::kCount;
} // namespace detail

// clang-format off
inline constexpr std::array<ColorKeyInfo, kColorKeyCount> kColorKeys{{
    // --- base: surfaces ---
    {ColorKey::MainWindowBackground, "MainWindow.background", detail::kNoColor, true},
    {ColorKey::IslandBackground, "Island.background", detail::kNoColor, true},
    {ColorKey::IslandBorderColor, "Island.borderColor", ColorKey::IslandBackground, false},
    {ColorKey::EditorBackground, "Editor.background", ColorKey::IslandBackground, true},
    {ColorKey::ToolWindowBackground, "ToolWindow.background", ColorKey::IslandBackground, true},
    {ColorKey::PanelBackground, "Panel.background", ColorKey::IslandBackground, true},
    {ColorKey::PopupBackground, "Popup.background", detail::kNoColor, true},
    {ColorKey::PopupBorderColor, "Popup.borderColor", ColorKey::ComponentBorderColor, false},
    {ColorKey::SeparatorColor, "Separator.color", ColorKey::ComponentBorderColor, false},
    // --- base: text ---
    {ColorKey::LabelForeground, "Label.foreground", detail::kNoColor, false},
    {ColorKey::LabelSecondaryForeground, "Label.secondaryForeground", detail::kNoColor, false},
    {ColorKey::LabelDisabledForeground, "Label.disabledForeground", detail::kNoColor, false},
    {ColorKey::LinkForeground, "Link.foreground", detail::kNoColor, false},
    // --- base: components ---
    {ColorKey::ComponentBorderColor, "Component.borderColor", detail::kNoColor, false},
    {ColorKey::ComponentFocusColor, "Component.focusColor", detail::kNoColor, false},
    {ColorKey::ComponentDisabledBorderColor, "Component.disabledBorderColor", ColorKey::ComponentBorderColor, false},
    {ColorKey::ButtonBackground, "Button.background", detail::kNoColor, true},
    {ColorKey::ButtonForeground, "Button.foreground", ColorKey::LabelForeground, false},
    {ColorKey::ButtonBorderColor, "Button.borderColor", ColorKey::ComponentBorderColor, false},
    {ColorKey::ButtonHoverBackground, "Button.hoverBackground", detail::kNoColor, false},
    {ColorKey::ButtonPressedBackground, "Button.pressedBackground", ColorKey::ButtonHoverBackground, false},
    {ColorKey::ButtonDefaultBackground, "Button.default.background", detail::kNoColor, true},
    {ColorKey::ButtonDefaultForeground, "Button.default.foreground", detail::kNoColor, false},
    {ColorKey::ButtonDefaultBorderColor, "Button.default.borderColor", ColorKey::ButtonDefaultBackground, false},
    {ColorKey::ButtonDefaultHoverBackground, "Button.default.hoverBackground", ColorKey::ButtonDefaultBackground, false},
    {ColorKey::TextFieldBackground, "TextField.background", detail::kNoColor, true},
    {ColorKey::TextFieldForeground, "TextField.foreground", ColorKey::LabelForeground, false},
    {ColorKey::TextFieldBorderColor, "TextField.borderColor", ColorKey::ComponentBorderColor, false},
    {ColorKey::TextFieldPlaceholderForeground, "TextField.placeholderForeground", ColorKey::LabelSecondaryForeground, false},
    {ColorKey::TextFieldSelectionBackground, "TextField.selectionBackground", ColorKey::ListSelectionBackground, false},
    {ColorKey::TextFieldSelectionForeground, "TextField.selectionForeground", ColorKey::ListSelectionForeground, false},
    {ColorKey::ComboBoxBackground, "ComboBox.background", ColorKey::TextFieldBackground, true},
    {ColorKey::ComboBoxForeground, "ComboBox.foreground", ColorKey::TextFieldForeground, false},
    {ColorKey::ComboBoxBorderColor, "ComboBox.borderColor", ColorKey::TextFieldBorderColor, false},
    {ColorKey::CheckBoxBackground, "CheckBox.background", ColorKey::TextFieldBackground, true},
    {ColorKey::CheckBoxBorderColor, "CheckBox.borderColor", detail::kNoColor, false},
    {ColorKey::CheckBoxCheckedBackground, "CheckBox.checkedBackground", ColorKey::ButtonDefaultBackground, false},
    {ColorKey::CheckBoxCheckmarkColor, "CheckBox.checkmarkColor", ColorKey::ButtonDefaultForeground, false},
    {ColorKey::ListBackground, "List.background", ColorKey::IslandBackground, true},
    {ColorKey::ListForeground, "List.foreground", ColorKey::LabelForeground, false},
    {ColorKey::ListHoverBackground, "List.hoverBackground", detail::kNoColor, false},
    {ColorKey::ListSelectionBackground, "List.selectionBackground", detail::kNoColor, false},
    {ColorKey::ListSelectionForeground, "List.selectionForeground", detail::kNoColor, false},
    {ColorKey::ListSelectionInactiveBackground, "List.selectionInactiveBackground", detail::kNoColor, false},
    {ColorKey::TreeBackground, "Tree.background", ColorKey::ListBackground, true},
    {ColorKey::TreeForeground, "Tree.foreground", ColorKey::ListForeground, false},
    {ColorKey::TreeHoverBackground, "Tree.hoverBackground", ColorKey::ListHoverBackground, false},
    {ColorKey::TreeSelectionBackground, "Tree.selectionBackground", ColorKey::ListSelectionBackground, false},
    {ColorKey::TreeSelectionForeground, "Tree.selectionForeground", ColorKey::ListSelectionForeground, false},
    {ColorKey::TreeSelectionInactiveBackground, "Tree.selectionInactiveBackground", ColorKey::ListSelectionInactiveBackground, false},
    {ColorKey::TabbedPaneUnderlineColor, "TabbedPane.underlineColor", ColorKey::ComponentFocusColor, false},
    {ColorKey::TabbedPaneHoverBackground, "TabbedPane.hoverBackground", ColorKey::ListHoverBackground, false},
    {ColorKey::ScrollBarThumbColor, "ScrollBar.thumbColor", detail::kNoColor, false},
    {ColorKey::ScrollBarThumbHoverColor, "ScrollBar.thumbHoverColor", ColorKey::ScrollBarThumbColor, false},
    {ColorKey::ScrollBarTrackColor, "ScrollBar.trackColor", detail::kNoColor, false},
    {ColorKey::ToolTipBackground, "ToolTip.background", ColorKey::PopupBackground, true},
    {ColorKey::ToolTipForeground, "ToolTip.foreground", ColorKey::LabelForeground, false},
    {ColorKey::ToolTipBorderColor, "ToolTip.borderColor", ColorKey::PopupBorderColor, false},
    // --- shell ---
    {ColorKey::MenuBackground, "Menu.background", ColorKey::PopupBackground, true},
    {ColorKey::MenuForeground, "Menu.foreground", ColorKey::LabelForeground, false},
    {ColorKey::MenuSelectionBackground, "Menu.selectionBackground", ColorKey::ListSelectionBackground, false},
    {ColorKey::MenuSelectionForeground, "Menu.selectionForeground", ColorKey::ListSelectionForeground, false},
    {ColorKey::MenuSeparatorColor, "Menu.separatorColor", ColorKey::SeparatorColor, false},
    {ColorKey::StatusBarForeground, "StatusBar.foreground", ColorKey::LabelSecondaryForeground, false},
    // --- editor ---
}};

inline constexpr std::array<MetricKeyInfo, kMetricKeyCount> kMetricKeys{{
    // --- base ---
    {MetricKey::IslandArc, "Island.arc", 0.0F, 32.0F, true, MetricUnit::Px},
    {MetricKey::IslandGap, "Island.gap", 0.0F, 32.0F, true, MetricUnit::Px},
    {MetricKey::ComponentArc, "Component.arc", 0.0F, 16.0F, false, MetricUnit::Px},
    {MetricKey::ButtonArc, "Button.arc", 0.0F, 16.0F, false, MetricUnit::Px},
    {MetricKey::ComponentFocusWidth, "Component.focusWidth", 1.0F, 4.0F, false, MetricUnit::Px},
    {MetricKey::ButtonHeight, "Button.height", 16.0F, 64.0F, true, MetricUnit::Px},
    {MetricKey::TextFieldHeight, "TextField.height", 16.0F, 64.0F, true, MetricUnit::Px},
    {MetricKey::ListRowHeight, "List.rowHeight", 12.0F, 64.0F, true, MetricUnit::Px},
    {MetricKey::TreeRowHeight, "Tree.rowHeight", 12.0F, 64.0F, true, MetricUnit::Px, MetricKey::ListRowHeight},
    {MetricKey::MenuItemHeight, "Menu.itemHeight", 12.0F, 64.0F, true, MetricUnit::Px},
    {MetricKey::TabbedPaneTabHeight, "TabbedPane.tabHeight", 16.0F, 64.0F, true, MetricUnit::Px},
    {MetricKey::StatusBarHeight, "StatusBar.height", 16.0F, 64.0F, true, MetricUnit::Px},
    {MetricKey::ScrollBarWidth, "ScrollBar.width", 4.0F, 32.0F, true, MetricUnit::Px},
    {MetricKey::ComponentPaddingH, "Component.paddingH", 0.0F, 32.0F, true, MetricUnit::Px},
    {MetricKey::LayoutMargin, "Layout.margin", 0.0F, 32.0F, true, MetricUnit::Px},
    {MetricKey::LayoutSpacing, "Layout.spacing", 0.0F, 32.0F, true, MetricUnit::Px},
    {MetricKey::IconSize, "Icon.size", 12.0F, 32.0F, false, MetricUnit::Px},
    {MetricKey::ToolbarIconSize, "Toolbar.iconSize", 12.0F, 32.0F, true, MetricUnit::Px},
    {MetricKey::FontUiSize, "Font.uiSize", 8.0F, 32.0F, false, MetricUnit::Px},
    {MetricKey::FontSmallSize, "Font.smallSize", 6.0F, 32.0F, false, MetricUnit::Px},
    {MetricKey::WindowInactiveAlpha, "Window.inactiveAlpha", 0.0F, 1.0F, false, MetricUnit::Ratio},
    // --- shell --- / --- editor ---
}};

inline constexpr std::array<IconRoleInfo, kIconRoleCount> kIconRoles{{
    {IconRole::Default, "Icon.Default"},
    {IconRole::Secondary, "Icon.Secondary"},
    {IconRole::Accent, "Icon.Accent"},
    {IconRole::Error, "Icon.Error"},
    {IconRole::Warning, "Icon.Warning"},
    {IconRole::Success, "Icon.Success"},
    {IconRole::Disabled, "Icon.Disabled"},
    {IconRole::Selected, "Icon.Selected"},
}};

inline constexpr std::array kContrastPairs{
    // Islands must stand out from the window behind them (PLAN.md section 2, "Theming").
    ContrastPair{ColorKey::IslandBackground, ColorKey::MainWindowBackground, ContrastKind::IslandSeparation},
    ContrastPair{ColorKey::EditorBackground, ColorKey::MainWindowBackground, ContrastKind::IslandSeparation},
    ContrastPair{ColorKey::ToolWindowBackground, ColorKey::MainWindowBackground, ContrastKind::IslandSeparation},
    // Text on every surface it appears on.
    ContrastPair{ColorKey::LabelForeground, ColorKey::IslandBackground, ContrastKind::Text},
    ContrastPair{ColorKey::LabelForeground, ColorKey::MainWindowBackground, ContrastKind::Text},
    ContrastPair{ColorKey::LabelForeground, ColorKey::PopupBackground, ContrastKind::Text},
    ContrastPair{ColorKey::LabelForeground, ColorKey::TextFieldBackground, ContrastKind::Text},
    ContrastPair{ColorKey::TextFieldForeground, ColorKey::TextFieldBackground, ContrastKind::Text},
    ContrastPair{ColorKey::ButtonForeground, ColorKey::ButtonBackground, ContrastKind::Text},
    // Buttons keep their label colour when hovered or pressed (frost_style.cpp drawButtonPanel).
    ContrastPair{ColorKey::ButtonForeground, ColorKey::ButtonHoverBackground, ContrastKind::Text,
                 ColorKey::IslandBackground},
    ContrastPair{ColorKey::ButtonForeground, ColorKey::ButtonPressedBackground, ContrastKind::Text,
                 ColorKey::IslandBackground},
    /*
     * The default button: resting, and hovered or pressed (both fill with
     * Button.default.hoverBackground). Button.default.foreground also draws the inner line of
     * a focused default button's two-colour focus indicator, so these pairs hold that line to
     * the Text minimum against either fill.
     */
    ContrastPair{ColorKey::ButtonDefaultForeground, ColorKey::ButtonDefaultBackground, ContrastKind::Text},
    ContrastPair{ColorKey::ButtonDefaultForeground, ColorKey::ButtonDefaultHoverBackground, ContrastKind::Text,
                 ColorKey::IslandBackground},
    ContrastPair{ColorKey::TextFieldPlaceholderForeground, ColorKey::TextFieldBackground, ContrastKind::Text},
    ContrastPair{ColorKey::ComboBoxForeground, ColorKey::ComboBoxBackground, ContrastKind::Text},
    ContrastPair{ColorKey::ListSelectionForeground, ColorKey::ListSelectionBackground, ContrastKind::Text,
                 ColorKey::ListBackground},
    // The palette's HighlightedText is List.selectionForeground in every group, also on the
    // Inactive group's List.selectionInactiveBackground.
    ContrastPair{ColorKey::ListSelectionForeground, ColorKey::ListSelectionInactiveBackground, ContrastKind::Text,
                 ColorKey::ListBackground},
    // Trees draw their selection with the Tree.* keys, active and inactive.
    ContrastPair{ColorKey::TreeSelectionForeground, ColorKey::TreeSelectionBackground, ContrastKind::Text,
                 ColorKey::TreeBackground},
    ContrastPair{ColorKey::TreeSelectionForeground, ColorKey::TreeSelectionInactiveBackground, ContrastKind::Text,
                 ColorKey::TreeBackground},
    ContrastPair{ColorKey::MenuForeground, ColorKey::MenuBackground, ContrastKind::Text},
    ContrastPair{ColorKey::MenuSelectionForeground, ColorKey::MenuSelectionBackground, ContrastKind::Text,
                 ColorKey::MenuBackground},
    ContrastPair{ColorKey::LinkForeground, ColorKey::IslandBackground, ContrastKind::Text},
    ContrastPair{ColorKey::ToolTipForeground, ColorKey::ToolTipBackground, ContrastKind::Text},
    ContrastPair{ColorKey::LabelSecondaryForeground, ColorKey::IslandBackground, ContrastKind::SecondaryText},
    ContrastPair{ColorKey::LabelDisabledForeground, ColorKey::IslandBackground, ContrastKind::DisabledText},
    // Focus rings and the unchecked check box's border (WCAG 2.2 success criterion 1.4.11).
    ContrastPair{ColorKey::ComponentFocusColor, ColorKey::IslandBackground, ContrastKind::NonText},
    ContrastPair{ColorKey::ComponentFocusColor, ColorKey::TextFieldBackground, ContrastKind::NonText},
    ContrastPair{ColorKey::CheckBoxBorderColor, ColorKey::CheckBoxBackground, ContrastKind::NonText},
    // Component borders: required in High Contrast only.
    ContrastPair{ColorKey::ComponentBorderColor, ColorKey::IslandBackground, ContrastKind::Border},
    ContrastPair{ColorKey::TextFieldBorderColor, ColorKey::TextFieldBackground, ContrastKind::Border},
};

inline constexpr std::array kIconContrastPairs{
    IconContrastPair{IconRole::Default, ColorKey::IslandBackground, ContrastKind::NonText},
    IconContrastPair{IconRole::Secondary, ColorKey::IslandBackground, ContrastKind::NonText},
    IconContrastPair{IconRole::Accent, ColorKey::IslandBackground, ContrastKind::NonText},
    IconContrastPair{IconRole::Error, ColorKey::IslandBackground, ContrastKind::NonText},
    IconContrastPair{IconRole::Warning, ColorKey::IslandBackground, ContrastKind::NonText},
    IconContrastPair{IconRole::Success, ColorKey::IslandBackground, ContrastKind::NonText},
    IconContrastPair{IconRole::Selected, ColorKey::ListSelectionBackground, ContrastKind::NonText,
                     ColorKey::ListBackground},
    // IconRole::Disabled is exempt (WCAG exempts inactive components).
};
// clang-format on

// The table entry of a key. O(1).
[[nodiscard]] constexpr const ColorKeyInfo& info(ColorKey key) noexcept {
    return kColorKeys[static_cast<std::size_t>(key)];
}
[[nodiscard]] constexpr const MetricKeyInfo& info(MetricKey key) noexcept {
    return kMetricKeys[static_cast<std::size_t>(key)];
}
[[nodiscard]] constexpr const IconRoleInfo& info(IconRole role) noexcept {
    return kIconRoles[static_cast<std::size_t>(role)];
}

namespace detail {

template<class Key, std::size_t N>
using NameIndex = std::array<std::pair<std::string_view, Key>, N>;

// The names of `table` sorted for binary search (constexpr std::sort).
template<class Key, class Info, std::size_t N, class KeyOf>
consteval NameIndex<Key, N> sortedNames(const std::array<Info, N>& table, KeyOf keyOf) {
    NameIndex<Key, N> index{};
    for (std::size_t i = 0; i < N; ++i) {
        index[i] = {table[i].name, keyOf(table[i])};
    }
    std::sort(index.begin(), index.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return index;
}

inline constexpr auto kColorNameIndex = sortedNames<ColorKey>(kColorKeys, [](const ColorKeyInfo& i) { return i.key; });
inline constexpr auto kMetricNameIndex =
    sortedNames<MetricKey>(kMetricKeys, [](const MetricKeyInfo& i) { return i.key; });
inline constexpr auto kIconRoleNameIndex =
    sortedNames<IconRole>(kIconRoles, [](const IconRoleInfo& i) { return i.role; });

template<class Key, std::size_t N>
constexpr std::optional<Key> findName(const NameIndex<Key, N>& index, std::string_view name) noexcept {
    const auto it = std::lower_bound(index.begin(), index.end(), name,
                                     [](const auto& entry, std::string_view n) { return entry.first < n; });
    if (it == index.end() || it->first != name) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace detail

// Looks a key up by its .theme.json name ("Button.default.background"). O(log n), allocation-free,
// thread-safe; nullopt for unknown names (case-sensitive).
[[nodiscard]] constexpr std::optional<ColorKey> colorKeyByName(std::string_view name) noexcept {
    return detail::findName(detail::kColorNameIndex, name);
}
[[nodiscard]] constexpr std::optional<MetricKey> metricKeyByName(std::string_view name) noexcept {
    return detail::findName(detail::kMetricNameIndex, name);
}
[[nodiscard]] constexpr std::optional<IconRole> iconRoleByName(std::string_view name) noexcept {
    return detail::findName(detail::kIconRoleNameIndex, name);
}

namespace detail {

// Table tags mixed into kRegistryHash so that moving a name between tables changes it.
inline constexpr std::uint64_t kColorTableTag = 0x434f4c4fU;  // "COLO"
inline constexpr std::uint64_t kMetricTableTag = 0x4d455452U; // "METR"
inline constexpr std::uint64_t kRoleTableTag = 0x524f4c45U;   // "ROLE"

consteval std::uint64_t registryHash() {
    Fnv1a64 h;
    h.updateInteger(kColorTableTag, 4);
    for (const ColorKeyInfo& entry : kColorKeys) {
        h.update(entry.name);
        h.updateInteger(0, 1);
        h.updateInteger(entry.opaque ? 1 : 0, 1);
    }
    h.updateInteger(kMetricTableTag, 4);
    for (const MetricKeyInfo& entry : kMetricKeys) {
        h.update(entry.name);
        h.updateInteger(0, 1);
        h.updateInteger(entry.hasCompact ? 1 : 0, 1);
        h.updateInteger(static_cast<std::uint64_t>(entry.unit), 1);
        h.updateInteger(std::bit_cast<std::uint32_t>(entry.min), 4);
        h.updateInteger(std::bit_cast<std::uint32_t>(entry.max), 4);
    }
    h.updateInteger(kRoleTableTag, 4);
    for (const IconRoleInfo& entry : kIconRoles) {
        h.update(entry.name);
        h.updateInteger(0, 1);
    }
    return h.value();
}

// "Component.part.property": two or more dot-separated segments, each a letter followed by
// letters and digits.
consteval bool wellFormedName(std::string_view name) {
    std::size_t segments = 0;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = std::min(name.find('.', start), name.size());
        const std::string_view segment = name.substr(start, end - start);
        if (segment.empty()) {
            return false;
        }
        const char first = segment[0];
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z'))) {
            return false;
        }
        for (const char c : segment) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                return false;
            }
        }
        ++segments;
        start = end + 1;
    }
    return segments >= 2;
}

template<class Info, std::size_t N, class KeyOf>
consteval bool denseAndNamed(const std::array<Info, N>& table, KeyOf keyOf) {
    for (std::size_t i = 0; i < N; ++i) {
        if (static_cast<std::size_t>(keyOf(table[i])) != i || !wellFormedName(table[i].name)) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (table[i].name == table[j].name) {
                return false;
            }
        }
    }
    return true;
}

consteval bool colorFallbacksAcyclic() {
    for (const ColorKeyInfo& entry : kColorKeys) {
        ColorKey key = entry.fallback;
        std::size_t steps = 0;
        while (key != ColorKey::kCount) {
            if (static_cast<std::size_t>(key) >= kColorKeyCount || ++steps > kColorKeyCount) {
                return false;
            }
            key = info(key).fallback;
        }
    }
    return true;
}

consteval bool metricTableValid() {
    for (const MetricKeyInfo& entry : kMetricKeys) {
        if (!(entry.min <= entry.max)) {
            return false;
        }
        MetricKey key = entry.fallback;
        std::size_t steps = 0;
        while (key != MetricKey::kCount) {
            if (static_cast<std::size_t>(key) >= kMetricKeyCount || ++steps > kMetricKeyCount) {
                return false;
            }
            key = info(key).fallback;
        }
    }
    return true;
}

consteval bool validColor(ColorKey key) { return static_cast<std::size_t>(key) < kColorKeyCount; }

// A background that may be translucent needs an opaque underlay; an opaque one needs none.
consteval bool underlayValid(ColorKey background, ColorKey underlay) {
    if (!validColor(background)) {
        return false;
    }
    if (info(background).opaque) {
        return underlay == ColorKey::kCount;
    }
    return validColor(underlay) && info(underlay).opaque;
}

consteval bool contrastPairsValid() {
    for (const ContrastPair& pair : kContrastPairs) {
        if (!validColor(pair.foreground) || !underlayValid(pair.background, pair.underlay)) {
            return false;
        }
    }
    for (const IconContrastPair& pair : kIconContrastPairs) {
        if (static_cast<std::size_t>(pair.role) >= kIconRoleCount || !underlayValid(pair.background, pair.underlay)) {
            return false;
        }
    }
    return true;
}

consteval bool iconRoleNamesPrefixed() {
    for (const IconRoleInfo& entry : kIconRoles) {
        if (!entry.name.starts_with("Icon.")) {
            return false;
        }
    }
    return true;
}

} // namespace detail

// FNV-1a 64 over every table entry's name and the properties that affect the blob layout or
// the decoder's validation (opacity, hasCompact, unit, range), in table order. Fallbacks and
// contrast pairs only affect compilation and are not covered. Embedded in every theme blob: a
// blob whose hash differs was compiled against another registry and is refused.
inline constexpr std::uint64_t kRegistryHash = detail::registryHash();

static_assert(detail::denseAndNamed(kColorKeys, [](const ColorKeyInfo& i) { return i.key; }),
              "kColorKeys: entry i must describe ColorKey i, with unique, well-formed names");
static_assert(detail::denseAndNamed(kMetricKeys, [](const MetricKeyInfo& i) { return i.key; }),
              "kMetricKeys: entry i must describe MetricKey i, with unique, well-formed names");
static_assert(detail::denseAndNamed(kIconRoles, [](const IconRoleInfo& i) { return i.role; }),
              "kIconRoles: entry i must describe IconRole i, with unique, well-formed names");
static_assert(detail::iconRoleNamesPrefixed(), "icon role names start with \"Icon.\"");
static_assert(detail::colorFallbacksAcyclic(), "colour fallback chains must end (no cycles)");
static_assert(detail::metricTableValid(), "metric ranges must be ordered and fallback chains acyclic");
static_assert(detail::contrastPairsValid(),
              "contrast pairs must name existing keys, and a pair whose background may be translucent "
              "needs an opaque underlay");

} // namespace frost::ui
