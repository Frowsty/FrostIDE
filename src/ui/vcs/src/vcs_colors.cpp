// SPDX-License-Identifier: Apache-2.0
//
// The VCS file status colours (frost/ui/vcs/vcs_colors.hpp).

#include "frost/ui/vcs/vcs_colors.hpp"

#include "frost/ui/editor/editor_colors.hpp"
#include "frost/ui/frost_style.hpp"

namespace frost::ui {

std::optional<ColorKey> vcsColorKey(vcs::VcsFileStatus status) noexcept {
    using S = vcs::VcsFileStatus;
    switch (status) {
    case S::Unmodified: return std::nullopt;
    case S::Added: return ColorKey::VcsAdded;
    case S::Modified: return ColorKey::VcsModified;
    case S::Deleted: return ColorKey::VcsDeleted;
    case S::Renamed: return ColorKey::VcsRenamed;
    case S::Untracked: return ColorKey::VcsUntracked;
    case S::Conflicted: return ColorKey::VcsConflicted;
    case S::Ignored: return ColorKey::VcsIgnored;
    }
    return std::nullopt;
}

std::optional<QColor> vcsStatusColor(vcs::VcsFileStatus status, const Theme* theme) {
    const std::optional<ColorKey> key = vcsColorKey(status);
    if (!key) {
        return std::nullopt;
    }
    if (theme != nullptr) {
        return toQColor(theme->color(*key));
    }
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        return style->color(*key);
    }
    // Glacier Dark's values, for widgets shown without a style (tools, some tests).
    switch (*key) {
    case ColorKey::VcsAdded: return QColor(0x6F, 0xBF, 0x73);
    case ColorKey::VcsModified:
    case ColorKey::VcsRenamed: return QColor(0x6D, 0x9F, 0xE6);
    case ColorKey::VcsDeleted: return QColor(0x8E, 0x94, 0x9C);
    case ColorKey::VcsUntracked: return QColor(0xD8, 0x8A, 0x6E);
    case ColorKey::VcsConflicted: return QColor(0xE8, 0x60, 0x5A);
    default: return QColor(0x9C, 0x9A, 0x62);
    }
}

std::function<std::optional<QColor>(std::string_view)> vcsFileColorProvider(const vcs::IVcsStatusProvider& statuses) {
    return [&statuses](std::string_view absolutePath) -> std::optional<QColor> {
        const std::optional<vcs::VcsFileStatus> status = statuses.statusOf(absolutePath);
        if (!status) {
            return std::nullopt;
        }
        return vcsStatusColor(*status);
    };
}

} // namespace frost::ui
