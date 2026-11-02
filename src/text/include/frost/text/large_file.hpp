// SPDX-License-Identifier: Apache-2.0
//
// The large-file guard's limits shared by the editor view (m1/editor-view, ADR 0037) and the
// document load service (m1/save-manager). Qt-free and header-only, so any layer may include it.
#pragma once

#include <cstdint>

namespace frost::text {

/*
 * The long-line cap: a line longer than this many bytes is "long". The load service opens a
 * document with a long line in large-file mode (no soft wrap, no syntax highlighting), and the
 * editor view never shapes, highlights or paints more than this many bytes of one display row.
 * 1,000,000 bytes is a starting value chosen so that minified JavaScript and generated one-line
 * JSON still open; the hardening phase measures it against B5 (keystroke to paint) and may lower it.
 */
inline constexpr std::uint64_t kLongLineCapBytes = 1'000'000;

} // namespace frost::text
