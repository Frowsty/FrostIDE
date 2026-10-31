// SPDX-License-Identifier: Apache-2.0
//
// The compat layer may include C++26 library headers behind feature checks.
#pragma once
#if __has_include(<inplace_vector>)
#include <inplace_vector>
#endif
