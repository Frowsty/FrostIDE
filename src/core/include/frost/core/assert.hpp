// SPDX-License-Identifier: Apache-2.0
//
// Contract checks: FROST_PRE (preconditions), FROST_POST (postconditions) and
// FROST_ASSERT (internal invariants), standing in for C++26 contracts, whose
// syntax is banned in v1 (PLAN.md section 2.1).
//
// Each check obeys a semantic chosen per translation unit with
// FROST_CONTRACT_SEMANTIC, mirroring the four C++26 contract semantics:
//   0 ignore         the condition is not evaluated;
//   1 observe        a violation calls the violation handler, then continues;
//   2 enforce        a violation calls the handler, then aborts (the default);
//   3 quick_enforce  a violation traps immediately with no handler call.
// Debug and Release both default to enforce; hot paths that measured a cost may
// select quick_enforce locally. Conditions must be free of side effects, since
// "ignore" skips them.
//
// The semantic is fixed when assert.hpp is first included, so define it before
// any include. It then applies to every check expanded in the TU, including
// checks in inline functions and templates from headers, and those must see the
// same semantic in every TU of a program (ODR). A non-default semantic is
// therefore safe only in a .cpp that includes no header containing checks, or
// when the whole program is built with it.
#pragma once

#include <source_location>

#ifndef FROST_CONTRACT_SEMANTIC
#define FROST_CONTRACT_SEMANTIC 2
#endif

namespace frost::contracts {

enum class Kind { Precondition, Postcondition, Assertion };

// Describes one failed check; passed to the violation handler.
struct Violation {
    Kind kind;
    const char* expression; // The condition's source text.
    std::source_location where;
    // True when the process ends after the handler returns (enforce): reportViolationAndAbort
    // sets it, so a handler can write a crash report instead of logging and continuing.
    bool terminating = false;
};

/*
 * A violation handler must not throw. The default handler writes the kind, expression and
 * location to stderr. frost's program installs frost::diag's handler at start-up
 * (frost/diag/fatal_handlers.hpp), which logs the violation and, when `terminating` is set,
 * writes a crash report with the stack through the crash reporter and does not return.
 */
using ViolationHandler = void (*)(const Violation&) noexcept;

// Installs `handler` (nullptr restores the default) and returns the previous one.
// Thread-safe; intended for tests and for the crash reporter at startup.
ViolationHandler setViolationHandler(ViolationHandler handler) noexcept;

// Called by the macros below; not for direct use.
void reportViolation(const Violation& violation) noexcept;
[[noreturn]] void reportViolationAndAbort(const Violation& violation) noexcept;

namespace detail {
// Converts the checked condition to bool. It takes exactly one argument, so
// FROST_PRE(ok, "message") is a compile error rather than a comma expression
// that is always true; commas inside template arguments, as in
// FROST_PRE(std::is_same_v<A, B>), still parse as one argument.
template<class T>
[[nodiscard]] constexpr bool condition(const T& value) noexcept(noexcept(static_cast<bool>(value))) {
    return static_cast<bool>(value);
}
} // namespace detail

} // namespace frost::contracts

#if FROST_CONTRACT_SEMANTIC == 0
#define FROST_CONTRACT_CHECK_(kind, ...) static_cast<void>(sizeof(::frost::contracts::detail::condition(__VA_ARGS__)))
#elif FROST_CONTRACT_SEMANTIC == 1
#define FROST_CONTRACT_CHECK_(kind, ...)                                                                               \
    (::frost::contracts::detail::condition(__VA_ARGS__)                                                                \
         ? static_cast<void>(0)                                                                                        \
         : ::frost::contracts::reportViolation({kind, #__VA_ARGS__, std::source_location::current()}))
#elif FROST_CONTRACT_SEMANTIC == 2
#define FROST_CONTRACT_CHECK_(kind, ...)                                                                               \
    (::frost::contracts::detail::condition(__VA_ARGS__)                                                                \
         ? static_cast<void>(0)                                                                                        \
         : ::frost::contracts::reportViolationAndAbort({kind, #__VA_ARGS__, std::source_location::current()}))
#elif FROST_CONTRACT_SEMANTIC == 3
#define FROST_CONTRACT_CHECK_(kind, ...)                                                                               \
    (::frost::contracts::detail::condition(__VA_ARGS__) ? static_cast<void>(0) : __builtin_trap())
#else
#error "FROST_CONTRACT_SEMANTIC must be 0 (ignore), 1 (observe), 2 (enforce) or 3 (quick_enforce)"
#endif

#define FROST_PRE(...) FROST_CONTRACT_CHECK_(::frost::contracts::Kind::Precondition, __VA_ARGS__)
#define FROST_POST(...) FROST_CONTRACT_CHECK_(::frost::contracts::Kind::Postcondition, __VA_ARGS__)
#define FROST_ASSERT(...) FROST_CONTRACT_CHECK_(::frost::contracts::Kind::Assertion, __VA_ARGS__)
