// SPDX-License-Identifier: Apache-2.0
//
// Library feature detection for the compat layer (PLAN.md section 2.1).
//
// The supported standard libraries differ: MSVC STL 14.44 on Windows and
// libstdc++ 14 (the floor) on Linux. Each FROST_HAS_* macro is 1 when the
// standard library provides the feature and 0 otherwise. So far only
// function_ref.hpp and saturate.hpp polyfill on them; the other macros (flat_map,
// inplace_vector, ...) are probes for planned polyfills (PLAN.md section 2.1), and
// code must not assume frost/compat supplies those yet. FROST_HAS_STD_EXPECTED is
// a hard requirement (see the static_assert below). Each polyfill states its
// removal condition, so retiring one is a matter of raising a library floor.
#pragma once

#include <version>

#if defined(__cpp_lib_flat_map) && __cpp_lib_flat_map >= 202207L
#define FROST_HAS_STD_FLAT_MAP 1
#else
#define FROST_HAS_STD_FLAT_MAP 0
#endif

#if defined(__cpp_lib_generator) && __cpp_lib_generator >= 202207L
#define FROST_HAS_STD_GENERATOR 1
#else
#define FROST_HAS_STD_GENERATOR 0
#endif

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
#define FROST_HAS_STD_MOVE_ONLY_FUNCTION 1
#else
#define FROST_HAS_STD_MOVE_ONLY_FUNCTION 0
#endif

#if defined(__cpp_lib_function_ref) && __cpp_lib_function_ref >= 202306L
#define FROST_HAS_STD_FUNCTION_REF 1
#else
#define FROST_HAS_STD_FUNCTION_REF 0
#endif

#if defined(__cpp_lib_saturation_arithmetic) && __cpp_lib_saturation_arithmetic >= 202311L
#define FROST_HAS_STD_SATURATION_ARITHMETIC 1
#else
#define FROST_HAS_STD_SATURATION_ARITHMETIC 0
#endif

#if defined(__cpp_lib_ranges_enumerate) && __cpp_lib_ranges_enumerate >= 202302L
#define FROST_HAS_STD_RANGES_ENUMERATE 1
#else
#define FROST_HAS_STD_RANGES_ENUMERATE 0
#endif

#if defined(__cpp_lib_ranges_chunk) && __cpp_lib_ranges_chunk >= 202202L
#define FROST_HAS_STD_RANGES_CHUNK 1
#else
#define FROST_HAS_STD_RANGES_CHUNK 0
#endif

#if defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202406L
#define FROST_HAS_STD_INPLACE_VECTOR 1
#else
#define FROST_HAS_STD_INPLACE_VECTOR 0
#endif

#if defined(__cpp_lib_stacktrace) && __cpp_lib_stacktrace >= 202011L
#define FROST_HAS_STD_STACKTRACE 1
#else
#define FROST_HAS_STD_STACKTRACE 0
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
#define FROST_HAS_STD_EXPECTED 1
#else
#define FROST_HAS_STD_EXPECTED 0
#endif

// std::expected is a hard requirement (every fallible API returns it); both
// supported standard libraries provide it, so there is no polyfill.
static_assert(FROST_HAS_STD_EXPECTED, "FrostIDE requires std::expected (MSVC STL 14.44+ or libstdc++ 14+)");
