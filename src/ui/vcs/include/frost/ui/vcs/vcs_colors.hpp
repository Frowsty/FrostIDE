// SPDX-License-Identifier: Apache-2.0
//
// The VCS file status colours (M5, ADR 0048): the theme's Vcs.* colour for a file status, and the
// colour providers the Solution Explorer, the File System view and the editor tabs take, built over
// any frost::vcs::IVcsStatusProvider.
#pragma once

#include "frost/ui/model/theme.hpp"
#include "frost/ui/model/theme_registry.hpp"
#include "frost/vcs/git_status.hpp"
#include "frost/vcs/vcs_status.hpp"

#include <QColor>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace frost::ui {

// The theme key of `status` (Vcs.added, Vcs.modified, ...); nullopt for Unmodified. O(1).
[[nodiscard]] std::optional<ColorKey> vcsColorKey(vcs::VcsFileStatus status) noexcept;

/*
 * The colour of a file with `status`: `theme`'s value, or FrostStyle::current()'s theme when `theme`
 * is null, or built-in dark colours without either; nullopt for Unmodified (the default text colour).
 * GUI thread (it may read FrostStyle::current()). O(1).
 */
[[nodiscard]] std::optional<QColor> vcsStatusColor(vcs::VcsFileStatus status, const Theme* theme = nullptr);

/*
 * A provider for SolutionTreeModel::setFileColorProvider and EditorTabs::setTitleColorProvider: the
 * colour of the file at an absolute path under `statuses` (nullopt outside every work tree or for
 * unmodified files). `statuses` must outlive the function; the colours follow the current theme.
 */
[[nodiscard]] std::function<std::optional<QColor>(std::string_view absolutePath)>
vcsFileColorProvider(const vcs::IVcsStatusProvider& statuses);

} // namespace frost::ui
