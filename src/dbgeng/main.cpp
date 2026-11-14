// SPDX-License-Identifier: Apache-2.0
// Windows-only DAP stdio process for the operating system's DbgEng; no debugger DLL is bundled.
#include "internal.hpp"

int main() { return frost::dbgeng::runAdapter(); }
