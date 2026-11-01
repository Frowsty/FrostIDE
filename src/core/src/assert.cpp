// SPDX-License-Identifier: Apache-2.0
//
// Contract violation reporting for FROST_PRE/FROST_POST/FROST_ASSERT
// (see frost/core/assert.hpp).

#include "frost/core/assert.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace frost::contracts {
namespace {

const char* kindName(Kind kind) noexcept {
    switch (kind) {
    case Kind::Precondition: return "precondition";
    case Kind::Postcondition: return "postcondition";
    case Kind::Assertion: return "assertion";
    }
    return "contract";
}

// Writes the violation to stderr and flushes it. stdio is not async-signal-safe,
// so a violation detected inside a signal handler could block on the stdio lock.
// TODO(M1): the crash reporter writes with WriteFile/write(2) instead.
void defaultHandler(const Violation& violation) noexcept {
    std::fprintf(stderr, "FrostIDE %s violated: %s\n  at %s:%u in %s\n", kindName(violation.kind), violation.expression,
                 violation.where.file_name(), static_cast<unsigned>(violation.where.line()),
                 violation.where.function_name());
    std::fflush(stderr);
}

/*
 * The installed handler; nullptr means defaultHandler. constinit keeps it free of
 * dynamic initialisation (startup hygiene); atomic so tests can swap it while
 * other threads check contracts. Installation is a release and lookup an acquire,
 * so a thread that loads a new handler also sees whatever state the installer
 * prepared for it.
 */
constinit std::atomic<ViolationHandler> g_handler{nullptr};

} // namespace

ViolationHandler setViolationHandler(ViolationHandler handler) noexcept {
    return g_handler.exchange(handler, std::memory_order_acq_rel);
}

void reportViolation(const Violation& violation) noexcept {
    const ViolationHandler handler = g_handler.load(std::memory_order_acquire);
    (handler != nullptr ? handler : &defaultHandler)(violation);
}

void reportViolationAndAbort(const Violation& violation) noexcept {
    reportViolation(violation);
    std::abort();
}

} // namespace frost::contracts
