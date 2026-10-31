// SPDX-License-Identifier: Apache-2.0
//
// Strong index types (PLAN.md section 6.5): ByteOffset, Utf16Offset, LineIndex
// and friends are distinct types over an integer, so a UTF-16 column can never
// be passed where a byte offset is expected.
//
// StrongIndex<Tag, Rep> supports the operations positions need: comparison,
// moving by a count (index + n, index - n) and the distance between two indices
// (index - index yields Rep). It deliberately has no index + index, no implicit
// conversion to or from Rep, and no multiplication.
#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>

namespace frost {

template<class Tag, std::unsigned_integral Rep>
class StrongIndex {
public:
    using rep_type = Rep;

    constexpr StrongIndex() noexcept = default;
    constexpr explicit StrongIndex(Rep value) noexcept : m_value(value) {}

    // The underlying integer, for I/O and interop; prefer the typed operations.
    [[nodiscard]] constexpr Rep value() const noexcept { return m_value; }

    friend constexpr auto operator<=>(StrongIndex, StrongIndex) noexcept = default;

    // Moving by a count. Wrap-around is the caller's responsibility (offsets are
    // validated against document sizes before arithmetic).
    friend constexpr StrongIndex operator+(StrongIndex index, Rep count) noexcept {
        return StrongIndex(static_cast<Rep>(index.m_value + count));
    }
    friend constexpr StrongIndex operator-(StrongIndex index, Rep count) noexcept {
        return StrongIndex(static_cast<Rep>(index.m_value - count));
    }
    // Distance from b to a; the caller guarantees a >= b.
    friend constexpr Rep operator-(StrongIndex a, StrongIndex b) noexcept {
        return static_cast<Rep>(a.m_value - b.m_value);
    }

    constexpr StrongIndex& operator+=(Rep count) noexcept {
        m_value = static_cast<Rep>(m_value + count);
        return *this;
    }
    constexpr StrongIndex& operator-=(Rep count) noexcept {
        m_value = static_cast<Rep>(m_value - count);
        return *this;
    }
    constexpr StrongIndex& operator++() noexcept {
        ++m_value;
        return *this;
    }
    constexpr StrongIndex& operator--() noexcept {
        --m_value;
        return *this;
    }

private:
    Rep m_value = 0;
};

// Byte position in UTF-8 text (documents are stored as UTF-8).
using ByteOffset = StrongIndex<struct ByteOffsetTag, std::uint64_t>;
// Position in UTF-16 code units (LSP and Qt positions).
using Utf16Offset = StrongIndex<struct Utf16OffsetTag, std::uint64_t>;
// Zero-based line number.
using LineIndex = StrongIndex<struct LineIndexTag, std::uint32_t>;
// Zero-based column in UTF-8 bytes within a line.
using ByteColumn = StrongIndex<struct ByteColumnTag, std::uint32_t>;

} // namespace frost

// Strong indices hash like their representation, so they can key unordered containers.
template<class Tag, class Rep>
struct std::hash<frost::StrongIndex<Tag, Rep>> {
    std::size_t operator()(frost::StrongIndex<Tag, Rep> index) const noexcept {
        return std::hash<Rep>{}(index.value());
    }
};

// Strong indices format as their value: std::format("{}", ByteOffset{7}) == "7".
template<class Tag, class Rep>
struct std::formatter<frost::StrongIndex<Tag, Rep>> : std::formatter<Rep> {
    template<class FormatContext>
    auto format(frost::StrongIndex<Tag, Rep> index, FormatContext& ctx) const {
        return std::formatter<Rep>::format(index.value(), ctx);
    }
};
