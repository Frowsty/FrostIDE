// SPDX-License-Identifier: Apache-2.0
// The tiny fixture's program; it calls into Lib, so the link needs Lib.lib.
#include "lib.h"

int main() { return tinyAnswer() == 42 ? 0 : 1; }
