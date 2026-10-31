// SPDX-License-Identifier: Apache-2.0
//
// Saturation arithmetic (C++26 P0543): add_sat, sub_sat, mul_sat and
// saturate_cast clamp to the destination type's range instead of wrapping.
//
// Polyfill: implemented with Clang's checked-arithmetic builtins when the
// standard library lacks <numeric>'s versions. Removal condition: both
// supported standard libraries define __cpp_lib_saturation_arithmetic.
#pragma once

#include "frost/compat/features.hpp"

#include <concepts>
#include <limits>
#include <numeric>
#include <utility>

namespace frost {

// Integer types eligible for saturation: every standard integer type except bool and character types.
template<class T>
concept saturating_integer =
    std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, wchar_t> &&
    !std::same_as<T, char8_t> && !std::same_as<T, char16_t> && !std::same_as<T, char32_t>;

#if FROST_HAS_STD_SATURATION_ARITHMETIC

using std::add_sat;
using std::mul_sat;
using std::saturate_cast;
using std::sub_sat;

#else

// x + y clamped to T's range.
template<saturating_integer T>
[[nodiscard]] constexpr T add_sat(T x, T y) noexcept {
    T result{};
    if (!__builtin_add_overflow(x, y, &result)) {
        return result;
    }
    // Overflow direction follows the sign of y (for unsigned types, always upward).
    if constexpr (std::is_unsigned_v<T>) {
        return std::numeric_limits<T>::max();
    } else {
        return y > 0 ? std::numeric_limits<T>::max() : std::numeric_limits<T>::min();
    }
}

// x - y clamped to T's range.
template<saturating_integer T>
[[nodiscard]] constexpr T sub_sat(T x, T y) noexcept {
    T result{};
    if (!__builtin_sub_overflow(x, y, &result)) {
        return result;
    }
    if constexpr (std::is_unsigned_v<T>) {
        return std::numeric_limits<T>::min();
    } else {
        return y < 0 ? std::numeric_limits<T>::max() : std::numeric_limits<T>::min();
    }
}

// x * y clamped to T's range.
template<saturating_integer T>
[[nodiscard]] constexpr T mul_sat(T x, T y) noexcept {
    T result{};
    if (!__builtin_mul_overflow(x, y, &result)) {
        return result;
    }
    if constexpr (std::is_unsigned_v<T>) {
        return std::numeric_limits<T>::max();
    } else {
        // The product's sign is negative exactly when the operands' signs differ.
        return (x < 0) != (y < 0) ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
    }
}

// Converts x to R, clamping values outside R's range to its nearest bound.
template<saturating_integer R, saturating_integer T>
[[nodiscard]] constexpr R saturate_cast(T x) noexcept {
    if (std::cmp_less(x, std::numeric_limits<R>::min())) {
        return std::numeric_limits<R>::min();
    }
    if (std::cmp_greater(x, std::numeric_limits<R>::max())) {
        return std::numeric_limits<R>::max();
    }
    return static_cast<R>(x);
}

#endif

} // namespace frost
