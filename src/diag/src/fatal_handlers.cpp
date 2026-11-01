// SPDX-License-Identifier: Apache-2.0
//
// The fatal-path handlers (frost/diag/fatal_handlers.hpp). Each one ends in
// ICrashReporter::crashNow(), which allocates nothing; the handlers themselves allocate only
// where the C++ runtime already did (an exception's what()).

#include "frost/diag/fatal_handlers.hpp"

#include "frost/core/log.hpp"

#include "crash_internal.hpp"

#include <atomic>
#include <csignal>
#include <exception>

#ifdef _WIN32
#include <cstdlib>

#include <stdlib.h>
#else
#include <client/crashpad_client.h>
#include <sys/prctl.h>

#include <signal.h>
#include <unistd.h>
#endif

namespace frost::diag {
namespace {

constinit std::atomic<ICrashReporter*> g_reporter{nullptr};

// crashNow() through the registered reporter, or the same message and exit without one.
[[noreturn]] void fatal(FatalReason reason, std::string_view message) noexcept {
    if (ICrashReporter* reporter = g_reporter.load(std::memory_order_acquire); reporter != nullptr) {
        reporter->crashNow(reason, message);
    }
    detail::enterFatalSection();
    detail::FixedText<1200> line;
    line.append("frost: fatal ").append(toString(reason)).append(": ").append(message.substr(0, 1024)).append("\n");
    detail::writeToStderr(line.view());
    detail::endWithoutReport();
}

std::string_view kindName(contracts::Kind kind) noexcept {
    switch (kind) {
    case contracts::Kind::Precondition: return "precondition";
    case contracts::Kind::Postcondition: return "postcondition";
    case contracts::Kind::Assertion: return "assertion";
    }
    return "contract";
}

// Unsigned decimal, for line numbers on allocation-free paths.
template<std::size_t N>
void appendNumber(detail::FixedText<N>& text, unsigned value) noexcept {
    char digits[10];
    std::size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0) {
        const char digit = digits[--count];
        text.append(std::string_view(&digit, 1));
    }
}

void terminateHandler() noexcept {
    detail::FixedText<1024> message;
    message.append("std::terminate called");
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& exception) {
            message.append(" with an uncaught exception: ").append(exception.what());
        } catch (...) {
            message.append(" with an uncaught exception of unknown type");
        }
    }
    fatal(FatalReason::Terminate, message.view());
}

#ifdef _WIN32
void abortSignalHandler(int) noexcept { fatal(FatalReason::Abort, "abort() called"); }

void invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) noexcept {
    fatal(FatalReason::InvalidParameter, "invalid parameter passed to a C runtime function");
}

void pureCallHandler() noexcept { fatal(FatalReason::PureCall, "pure virtual function call"); }
#else
constexpr int kLastChanceSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS};

// No core file and no ptrace attach from here on, then the signal's default action.
void lastChanceHandler(int signal, siginfo_t*, void*) noexcept {
    (void)::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    struct sigaction defaultAction{};
    defaultAction.sa_handler = SIG_DFL;
    sigemptyset(&defaultAction.sa_mask);
    (void)::sigaction(signal, &defaultAction, nullptr);
    // A fault re-executes and faults again on return; anything else is raised again. The
    // signal is blocked while this handler runs, so it is delivered when the handler returns.
    (void)::raise(signal);
}
#endif

} // namespace

ICrashReporter* fatalReporter() noexcept { return g_reporter.load(std::memory_order_acquire); }

void installThreadFatalHandlers() noexcept {
    (void)std::set_terminate(&terminateHandler);
#ifndef _WIN32
    // An alternate signal stack for this thread, so a stack overflow (SIGSEGV with the stack
    // exhausted) still reaches crashpad's handler. crashpad's SetHandlerSocket does this only for
    // the registering thread, which in the GUI is "frost-diag", not the UI thread. Freed when
    // the thread exits. TODO(M3): every worker thread (frost::sched's pool) as well.
    (void)crashpad::CrashpadClient::InitializeSignalStackForThread();
#endif
}

void installLastChanceHandlers() noexcept {
#ifndef _WIN32
    struct sigaction action{};
    action.sa_sigaction = &lastChanceHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int signal : kLastChanceSignals) {
        (void)::sigaction(signal, &action, nullptr);
    }
#endif
}

void contractViolationHandler(const contracts::Violation& violation) noexcept {
    // The log record carries the stack as module+offset lines (cpptrace), so an observed
    // violation, which writes no crash report, can still be located offline.
    if (log::enabled(log::Level::Critical)) {
        log::critical("contract", "{} violated: {} at {}:{} in {}{}", kindName(violation.kind), violation.expression,
                      violation.where.file_name(), violation.where.line(), violation.where.function_name(),
                      detail::describeStack(1));
    }
    if (!violation.terminating) {
        return;
    }
    detail::FixedText<1024> message;
    message.append(kindName(violation.kind)).append(" violated: ").append(violation.expression);
    message.append(" at ").append(violation.where.file_name()).append(":");
    appendNumber(message, static_cast<unsigned>(violation.where.line()));
    // The stack from here, minus this handler, so the report names the failed check's caller
    // chain even though crashNow() runs deeper.
    std::uintptr_t frames[64];
    const std::size_t count = detail::captureFrames(frames, 64, 1);
    char text[64 * 19];
    const std::size_t length = detail::formatFrames(frames, count, text, sizeof(text));
    detail::setAnnotation(AnnotationKey::Frames, std::string_view(text, length));
    detail::FixedText<1200> line;
    line.append("FrostIDE ").append(message.view()).append("\n");
    detail::writeToStderr(line.view());
    log::flush();
    fatal(FatalReason::ContractViolation, message.view());
}

void installFatalHandlers(ICrashReporter& reporter) noexcept {
    g_reporter.store(&reporter, std::memory_order_release);
    (void)contracts::setViolationHandler(&contractViolationHandler);
#ifdef _WIN32
    // abort() then neither shows its message box nor asks Windows Error Reporting. This SIGABRT
    // handler serves until the reporter starts; SetHandlerIPCPipe installs crashpad's own, which
    // writes the report (STATUS_FATAL_APP_EXIT).
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)std::signal(SIGABRT, &abortSignalHandler);
    (void)_set_invalid_parameter_handler(&invalidParameterHandler);
    (void)_set_purecall_handler(&pureCallHandler);
#endif
    installThreadFatalHandlers();
}

} // namespace frost::diag
