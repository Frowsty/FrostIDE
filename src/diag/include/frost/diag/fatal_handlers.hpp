// SPDX-License-Identifier: Apache-2.0
//
// The fatal paths that do not reach crashpad by themselves, routed into
// ICrashReporter::crashNow() (ADR 0022, D-G):
//
//   path                                   handler                          report
//   access violation / SIGSEGV, stack       crashpad's own (exception        the fault
//     overflow on the UI thread             filter / signal handlers)
//   FROST_PRE/POST/ASSERT (enforce)         contractViolationHandler         ContractViolation + frames
//   quick_enforce (__builtin_trap)          crashpad's own                   the trap
//   std::terminate                          terminate handler (per thread)   Terminate
//   abort()                                 crashpad's SIGABRT handler       Windows: STATUS_FATAL_APP_EXIT
//                                           (frost's before it starts)       Linux: the signal
//   CRT invalid parameter (Windows)         _set_invalid_parameter_handler   InvalidParameter
//   pure virtual call (Windows)             _set_purecall_handler            PureCall
//   Qt fatal message in the GUI             frost::app (diagnostics_qt)      QtFatal
//   __fastfail                              none: the kernel ends the process without any user-mode
//                                           code (a documented gap, ADR 0022 Q3)
#pragma once

#include "frost/core/assert.hpp"
#include "frost/diag/crash_reporter.hpp"

namespace frost::diag {

/*
 * Installs every handler in the table above that frost owns, for `reporter`, which must live
 * for the rest of the process (it is kept by pointer). Call once, on the main thread, as early
 * as possible: before the reporter is active crashNow() still ends the process with a message
 * (see ICrashReporter::crashNow). Also installs the calling thread's handlers
 * (installThreadFatalHandlers). Windows: _set_abort_behavior(0, _WRITE_ABORT_MSG |
 * _CALL_REPORTFAULT), so abort() neither shows a box nor asks Windows Error Reporting, and a
 * SIGABRT handler that ends the process through crashNow(Abort) until the reporter starts;
 * registration then replaces it with crashpad's own, which writes the report itself.
 */
void installFatalHandlers(ICrashReporter& reporter) noexcept;

// The per-thread part: std::set_terminate, which the MSVC runtime keeps per thread, and on
// Linux an alternate signal stack (so a stack overflow still reaches crashpad). Every thread
// frost::diag starts calls it; other modules' threads keep the runtime's default (abort(),
// which still reaches the SIGABRT path on Windows and crashpad on Linux).
void installThreadFatalHandlers() noexcept;

/*
 * Linux: handlers for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP and SIGSYS that set
 * PR_SET_DUMPABLE to 0 (no core file, no ptrace), restore the default action and re-raise.
 * Installed before the reporter starts, so crashpad's handlers chain to them after writing the
 * report. No-op on Windows.
 */
void installLastChanceHandlers() noexcept;

// The reporter installFatalHandlers() registered, or nullptr. Async-signal-safe.
[[nodiscard]] ICrashReporter* fatalReporter() noexcept;

// The contract violation handler installFatalHandlers() installs: logs the violation; for a
// terminating one (enforce) records the frames and calls crashNow(ContractViolation).
void contractViolationHandler(const contracts::Violation& violation) noexcept;

} // namespace frost::diag
