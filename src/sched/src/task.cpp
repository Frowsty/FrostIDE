// SPDX-License-Identifier: Apache-2.0
//
// Exception handling at the task boundary (see frost/sched/task.hpp).

#include "frost/sched/task.hpp"

#include "frost/core/assert.hpp"

#include <exception>
#include <source_location>
#include <string>

namespace frost::sched::detail {

std::string currentExceptionMessage() {
    try {
        throw; // Rethrows the exception being handled, to inspect its type.
    } catch (const std::exception& exception) {
        return exception.what();
    } catch (...) {
        return "an exception not derived from std::exception";
    }
}

void abortOnEscapedException() noexcept {
    // The handler sees the exception text in the "expression" slot. Building the
    // string can itself throw (bad_alloc); then the fixed text is used.
    constexpr const char* kFixed = "no exception escapes a Task body";
    std::string text;
    try {
        text = std::string(kFixed) + " (caught: " + currentExceptionMessage() + ")";
    } catch (...) {
        text.clear();
    }
    contracts::reportViolationAndAbort(contracts::Violation{
        contracts::Kind::Assertion, text.empty() ? kFixed : text.c_str(), std::source_location::current()});
}

} // namespace frost::sched::detail
