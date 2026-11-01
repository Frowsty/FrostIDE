// SPDX-License-Identifier: Apache-2.0
//
// readActiveLayout on Linux (and other non-Windows systems): the en-US table in
// LettersByLabel mode. An M1 limitation (ADR 0016 decision 12): letters follow the labels
// the X11 or Wayland adapter reports, everything else is positional. TODO(M11): a
// libxkbcommon-based reader, pending the dependency decision.

#include "frost/input/layout.hpp"
#include "frost/input/simulated_keyboard.hpp"

namespace frost::input {

Result<LayoutTable> readActiveLayout() { return linuxM1Layout(); }

} // namespace frost::input
