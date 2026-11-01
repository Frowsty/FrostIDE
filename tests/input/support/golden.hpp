// SPDX-License-Identifier: Apache-2.0
//
// Golden-file comparison for the frost::input tests. Goldens are consistency checks: they
// pin reviewed output (resolved keymaps, layout mappings, rankings) so any change shows up
// as a diff. They live in tests/input/goldens/ (FROST_INPUT_GOLDEN_DIR), LF line ends.
#pragma once

#include <string_view>

namespace frost::input::test {

/*
 * Compares `actual` with the golden file `name`, byte for byte (binary mode, LF). On a
 * mismatch the test fails with the first differing line. With FROST_UPDATE_GOLDENS=1 the
 * file is rewritten and the test still FAILS ("golden updated: review the diff"), so an
 * update never passes silently. Update goldens in the Windows tree: the WSL mirror is
 * overwritten by the next rsync.
 */
void expectGolden(std::string_view name, std::string_view actual);

} // namespace frost::input::test
