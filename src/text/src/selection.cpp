// SPDX-License-Identifier: Apache-2.0
//
// SelectionSet (frost/text/selection.hpp): the shared array, normalisation by one sorted sweep, the
// transform through a raw batch with derived endpoint biases, and the invariants (ADR 0017,
// decision 4).

#include "frost/text/selection.hpp"

#include "frost/core/assert.hpp"

#include "batch_mapper.hpp"

#include <format>
#include <numeric>
#include <tuple>
#include <utility>

namespace frost::text {

// The shared, immutable array behind a non-empty SelectionSet.
struct SelectionSet::Data : detail::RefCounted {
    std::vector<Selection> items;
    std::size_t primary = 0;
};

namespace {

// Section 3.2's rule for the accumulated selection [start, end) and the next one.
bool mergeable(ByteRange current, ByteRange next) noexcept {
    return next.begin < current.end || (next.begin == current.end && (current.empty() || next.empty()));
}

} // namespace

SelectionSet::SelectionSet() noexcept = default;
SelectionSet::SelectionSet(const SelectionSet& other) noexcept = default;
SelectionSet::SelectionSet(SelectionSet&& other) noexcept = default;
SelectionSet& SelectionSet::operator=(const SelectionSet& other) noexcept = default;
SelectionSet& SelectionSet::operator=(SelectionSet&& other) noexcept = default;
SelectionSet::~SelectionSet() = default;

SelectionSet::SelectionSet(detail::Ref<Data> data) noexcept : m_data(std::move(data)) {}

SelectionSet SelectionSet::caret(ByteOffset at) noexcept {
    const Selection one = Selection::caret(at);
    return fromSelections(std::span<const Selection>(&one, 1), 0);
}

SelectionSet SelectionSet::fromSelections(std::span<const Selection> selections, std::size_t primary,
                                          std::vector<std::uint32_t>* origins) noexcept {
    if (origins != nullptr) {
        origins->clear();
    }
    if (selections.empty()) {
        return {};
    }
    FROST_PRE(primary < selections.size());
    const std::size_t n = selections.size();
    std::vector<std::uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    const auto key = [&](std::uint32_t i) {
        const ByteRange r = selections[i].range();
        return std::tuple(r.begin, r.end, i);
    };
    // Already sorted by (start, end) (the transform's usual case): the identity order is the sort.
    bool sorted = true;
    for (std::size_t i = 1; i < n && sorted; ++i) {
        const ByteRange a = selections[i - 1].range();
        const ByteRange b = selections[i].range();
        sorted = std::tie(a.begin, a.end) <= std::tie(b.begin, b.end);
    }
    if (!sorted) {
        std::sort(order.begin(), order.end(), [&](std::uint32_t x, std::uint32_t y) { return key(x) < key(y); });
    }

    detail::Ref<Data> data = detail::Ref<Data>::make();
    data->items.reserve(n);
    // The selection being accumulated by the sweep.
    ByteRange range{};
    bool directed = false; // A non-empty member has given the direction.
    bool reversed = false;
    bool isPrimary = false;
    std::uint32_t origin = 0;
    const auto start = [&](std::uint32_t i) {
        range = selections[i].range();
        directed = !selections[i].empty();
        reversed = selections[i].reversed();
        isPrimary = i == primary;
        origin = i;
    };
    const auto flush = [&] {
        const Selection merged = range.empty() ? Selection::caret(range.begin)
                                 : reversed    ? Selection{range.end, range.begin}
                                               : Selection{range.begin, range.end};
        if (isPrimary) {
            data->primary = data->items.size();
        }
        data->items.push_back(merged);
        if (origins != nullptr) {
            origins->push_back(origin);
        }
    };
    start(order[0]);
    for (std::size_t k = 1; k < n; ++k) {
        const std::uint32_t i = order[k];
        const ByteRange next = selections[i].range();
        if (mergeable(range, next)) {
            range.end = std::max(range.end, next.end);
            if (!directed && !selections[i].empty()) {
                directed = true;
                reversed = selections[i].reversed();
            }
            isPrimary = isPrimary || i == primary;
            continue;
        }
        flush();
        start(i);
    }
    flush();
    return SelectionSet(std::move(data));
}

std::size_t SelectionSet::size() const noexcept { return m_data ? m_data->items.size() : 0; }

bool SelectionSet::empty() const noexcept { return !m_data; }

std::span<const Selection> SelectionSet::all() const noexcept {
    return m_data ? std::span<const Selection>(m_data->items) : std::span<const Selection>();
}

std::size_t SelectionSet::primaryIndex() const noexcept { return m_data ? m_data->primary : 0; }

ByteOffset SelectionSet::maxOffset() const noexcept {
    return m_data ? m_data->items.back().range().end : ByteOffset{0};
}

SelectionSet SelectionSet::transformed(std::span<const RawEdit> batch,
                                       std::vector<std::uint32_t>* origins) const noexcept {
    if (!m_data) {
        if (origins != nullptr) {
            origins->clear();
        }
        return {};
    }
    // The endpoints come in non-decreasing order (sorted, non-overlapping selections), so one
    // mapper cursor serves both biases.
    detail::BatchMapper mapper(batch);
    std::vector<Selection> mapped;
    mapped.reserve(m_data->items.size());
    for (const Selection& selection : m_data->items) {
        const ByteRange r = selection.range();
        if (selection.empty()) {
            mapped.push_back(Selection::caret(ByteOffset{mapper.map(r.begin.value(), Bias::Right)}));
            continue;
        }
        const ByteOffset lo{mapper.map(r.begin.value(), Bias::Left)};
        const ByteOffset hi{mapper.map(r.end.value(), Bias::Right)};
        mapped.push_back(selection.reversed() ? Selection{hi, lo} : Selection{lo, hi});
    }
    return fromSelections(mapped, m_data->primary, origins);
}

bool operator==(const SelectionSet& a, const SelectionSet& b) noexcept {
    if (a.m_data == b.m_data) {
        return true;
    }
    if (!a.m_data || !b.m_data) {
        return false;
    }
    return a.m_data->primary == b.m_data->primary && a.m_data->items == b.m_data->items;
}

Result<void> SelectionSet::checkInvariants() const noexcept {
    if (!m_data) {
        return {};
    }
    const std::vector<Selection>& items = m_data->items;
    if (items.empty()) {
        return fail(ErrorCode::Internal, "L3: a non-null selection array is empty");
    }
    for (std::size_t i = 1; i < items.size(); ++i) {
        const ByteRange a = items[i - 1].range();
        const ByteRange b = items[i].range();
        if (std::tie(a.begin, a.end) > std::tie(b.begin, b.end)) {
            return fail(ErrorCode::Internal, std::format("L1: selection {} [{}, {}) is before selection {} [{}, {})", i,
                                                         b.begin, b.end, i - 1, a.begin, a.end));
        }
        if (mergeable(a, b)) {
            return fail(ErrorCode::Internal, std::format("L2: selections {} [{}, {}) and {} [{}, {}) are mergeable",
                                                         i - 1, a.begin, a.end, i, b.begin, b.end));
        }
    }
    if (m_data->primary >= items.size()) {
        return fail(ErrorCode::Internal,
                    std::format("L3: primary index {} of {} selections", m_data->primary, items.size()));
    }
    return {};
}

} // namespace frost::text
