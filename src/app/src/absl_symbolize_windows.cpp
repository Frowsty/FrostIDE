// SPDX-License-Identifier: Apache-2.0
//
// Abseil's symbolizer, replaced in frost_app.dll on Windows (ADR 0044). re2, which the editor's
// regular-expression search uses, reports fatal errors through absl::log, whose stack dump names
// frames with absl::Symbolize; abseil's Windows implementation calls SymInitialize and
// SymFromAddr, which would make frost_app.dll import dbghelp.dll.
/*
 * FrostIDE resolves no symbols in process: crash reports are symbolized offline (ADR 0022), and
 * FrostProcess.AppDllImportsOnlyAllowedDlls keeps dbghelp.dll out of frost_app.dll's imports. These
 * two definitions are linked as an object file of frost_app.dll, so the linker never pulls
 * absl_symbolize.lib's member (which defines nothing else). They behave like abseil's own
 * symbolize_unimplemented.inc (Apache-2.0, https://github.com/abseil/abseil-cpp): initialising
 * does nothing and no address gets a name, so a fatal re2 error still prints its frames' addresses.
 * Thread-safe (stateless).
 */

#include <absl/debugging/symbolize.h>

namespace absl {
ABSL_NAMESPACE_BEGIN

void InitializeSymbolizer(const char* /*argv0*/) {}

bool Symbolize(const void* /*pc*/, char* /*out*/, int /*out_size*/) { return false; }

ABSL_NAMESPACE_END
} // namespace absl
