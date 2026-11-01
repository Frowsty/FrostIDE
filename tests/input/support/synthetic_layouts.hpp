// SPDX-License-Identifier: Apache-2.0
//
// Synthetic layout tables for the frost::input tests: the en-US table with some cells
// replaced, built at run time with LayoutTable::make. They cover the shapes of FullTable
// rule 3.6 (ADR 0030) that the four simulated layouts do not: a layout with no '/' at all,
// Swiss-like and backward chains, a swap, two producers of one character, and French-like
// needed dispatch. They also keep the "unreachable binding" subjects of the keymap and
// service tests now that de-DE reaches every default shortcut.
#pragma once

#include "frost/input/key.hpp"
#include "frost/input/layout.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace frost::input::test {

// The en-US simulated table with `changes` applied (a change replaces the whole cell; an
// all-zero KeyLevels empties it), as a FullTable layout called `id` with no compositions.
// Fails the test (and returns en-US) when make() rejects the cells.
[[nodiscard]] LayoutTable usWith(std::vector<std::pair<Code, KeyLevels>> changes, std::string_view id = "test");

namespace synthetic {

// '-' and '_' on Slash and nothing on Minus: no key types '/', so Key::Slash is unreachable
// and Ctrl+Slash is reachable only as Ctrl+NumpadDivide (the old de-DE gap, kept as a subject).
[[nodiscard]] LayoutTable noSlash();
// Swiss-German-like: ''' on Minus, 'ä' on Quote, '-' on Slash. Rule 3.6 re-points Minus
// first (Code::Quote reaches Key::Quote positionally), which puts Key::Minus on Code::Minus,
// so Slash is re-pointed too: everything is positional.
[[nodiscard]] LayoutTable swissLike();
// A chain rule 3.6 resolves only on its second pass: '/' on Minus, ''' on Slash.
[[nodiscard]] LayoutTable backwardChain();
// French-like needed dispatch: ';' on Comma, 'm' on Semicolon, ',' on KeyM.
[[nodiscard]] LayoutTable frLike();
// '-' on Slash and '/' on Minus: each is the only code of its key, so both stay swapped.
[[nodiscard]] LayoutTable swap();
// '-' on both Comma and Slash, nothing on Minus: the tie case (Comma is re-pointed).
[[nodiscard]] LayoutTable twoProducers();

} // namespace synthetic

} // namespace frost::input::test
