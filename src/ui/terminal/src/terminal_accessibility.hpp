// SPDX-License-Identifier: Apache-2.0
// Qt accessibility for the actual terminal viewport; never writes child input or changes program output.
#pragma once
namespace frost::ui {
class TerminalView;
void initializeTerminalAccessibility(TerminalView& view);
void notifyTerminalAccessibility(TerminalView& view, bool outputChanged);
}
