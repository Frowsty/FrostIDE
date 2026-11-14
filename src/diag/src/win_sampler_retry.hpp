// SPDX-License-Identifier: Apache-2.0
//
// Bounded Windows capture retry policy. Only an image lost after resumption permits a fresh
// context/stack capture; metadata failures and unavailable target outcomes stop immediately.
#pragma once

#include "frost/diag/thread_sampler.hpp"

#include <utility>

namespace frost::diag::detail {

enum class CaptureAttempt { Finished, ImageUnavailable };
inline constexpr unsigned kMaxCaptureAttempts = 3;

// `capture` returns value-only ImageUnavailable after all targets are resumed, or Finished
// (which includes Busy/ResumeFailed POD outcomes). No Error survives into a later suspension.
template<class Capture, class Yield>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): callbacks are reused as lvalues across bounded retries.
[[nodiscard]] Result<void> captureWithRetries(StackSample& out, Capture&& capture, Yield&& yield) noexcept {
    for (unsigned attempt = 0; attempt < kMaxCaptureAttempts; ++attempt) {
        out.count = 0;
        out.outcome = StackSample::Outcome::Empty;
        {
            auto result = capture();
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            if (*result == CaptureAttempt::Finished) {
                return {};
            }
        }
        // Both the failed walk's module references and its value-only Result are gone. Give
        // the running target a scheduler opportunity before taking another fresh snapshot.
        if (attempt + 1 < kMaxCaptureAttempts) {
            yield();
        }
    }
    out.count = 0;
    out.outcome = StackSample::Outcome::Empty;
    return fail(ErrorCode::Unsupported, "sampled image unavailable after three fresh capture attempts");
}

} // namespace frost::diag::detail
