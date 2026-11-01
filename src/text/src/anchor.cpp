// SPDX-License-Identifier: Apache-2.0
//
// The anchor mapping rule through a batch (frost/text/anchor.hpp) and BatchMapper
// (batch_mapper.hpp), both in the closed form documented in anchor.hpp (ADR 0017, decision 1).

#include "frost/text/anchor.hpp"

#include "frost/core/assert.hpp"

#include "batch_mapper.hpp"

namespace frost::text {
namespace {

// x + delta for a delta that may be negative; the result is a valid offset by construction.
constexpr std::uint64_t shifted(std::uint64_t x, std::int64_t delta) noexcept {
    return delta >= 0 ? x + static_cast<std::uint64_t>(delta) : x - static_cast<std::uint64_t>(-delta);
}

constexpr std::int64_t stepDelta(const RawEdit& edit) noexcept {
    return static_cast<std::int64_t>(edit.newBytes) - static_cast<std::int64_t>(edit.range.size());
}

} // namespace

ByteOffset mapOffset(ByteOffset x, Bias bias, std::span<const RawEdit> batch) noexcept {
    const std::uint64_t y = x.value();
    std::int64_t delta = 0;        // D_i: the size change of the steps passed.
    std::uint64_t runStart = 0;    // a_K + D_K of the touching run the current step belongs to.
    std::uint64_t previousEnd = 0; // b of the previous non-no-op step.
    bool havePrevious = false;
    std::uint64_t checkedEnd = 0; // For the order precondition, no-ops included.
    for (const RawEdit& edit : batch) {
        FROST_PRE(edit.range.begin <= edit.range.end && checkedEnd <= edit.range.begin.value());
        checkedEnd = edit.range.end.value();
        if (edit.isNoOp()) {
            continue;
        }
        const std::uint64_t a = edit.range.begin.value();
        const std::uint64_t b = edit.range.end.value();
        if (a > y) {
            break;
        }
        const std::uint64_t startFinal = shifted(a, delta);
        if (!havePrevious || previousEnd != a) {
            runStart = startFinal;
        }
        if (bias == Bias::Left) {
            if (b >= y) {
                return ByteOffset{runStart}; // F: the first edit containing x.
            }
        } else if (y < b) {
            return ByteOffset{startFinal + edit.newBytes}; // L == this edit.
        }
        // Right with y == b continues: a later insert at y, or a range starting at y, may be L.
        delta += stepDelta(edit);
        previousEnd = b;
        havePrevious = true;
    }
    return ByteOffset{shifted(y, delta)};
}

namespace detail {

BatchMapper::BatchMapper(std::span<const RawEdit> batch) noexcept {
    m_steps.reserve(batch.size());
    std::int64_t delta = 0;
    for (const RawEdit& edit : batch) {
        if (edit.isNoOp()) {
            continue;
        }
        Step step;
        step.begin = edit.range.begin.value();
        step.end = edit.range.end.value();
        FROST_PRE(step.begin <= step.end && (m_steps.empty() || m_steps.back().end <= step.begin));
        step.startFinal = shifted(step.begin, delta);
        step.endFinal = step.startFinal + edit.newBytes;
        step.leftTarget =
            !m_steps.empty() && m_steps.back().end == step.begin ? m_steps.back().leftTarget : step.startFinal;
        delta += stepDelta(edit);
        m_steps.push_back(step);
    }
    m_totalDelta = delta;
    // Right image of x == b_i: the end of the last step containing b_i (backward pass).
    for (std::size_t i = m_steps.size(); i-- > 0;) {
        Step& step = m_steps[i];
        step.rightAtEnd = step.endFinal;
        if (i + 1 < m_steps.size() && m_steps[i + 1].begin == step.end) {
            const Step& next = m_steps[i + 1];
            step.rightAtEnd = next.end > next.begin ? next.endFinal : next.rightAtEnd;
        }
    }
}

std::uint64_t BatchMapper::map(std::uint64_t x, Bias bias) noexcept {
    FROST_PRE(x >= m_last);
    m_last = x;
    while (m_next < m_steps.size() && m_steps[m_next].end < x) {
        ++m_next;
    }
    if (m_next == m_steps.size()) {
        if (m_steps.empty()) {
            return x;
        }
        const Step& last = m_steps.back();
        return last.endFinal + (x - last.end);
    }
    const Step& step = m_steps[m_next];
    if (step.begin > x) {
        return step.startFinal - (step.begin - x);
    }
    if (bias == Bias::Left) {
        return step.leftTarget;
    }
    return x < step.end ? step.endFinal : step.rightAtEnd;
}

} // namespace detail
} // namespace frost::text
