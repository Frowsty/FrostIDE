// SPDX-License-Identifier: Apache-2.0
// CPP-8 real Clang IndexDataConsumer producer; stdout is one bounded read-only JSON unit.
#pragma once
#include <string_view>
namespace frost::clang_engine {
int runSemanticIndex(std::string_view resourceDirectory);
}
