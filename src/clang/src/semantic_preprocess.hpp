// SPDX-License-Identifier: Apache-2.0
// CPP-13 actual Clang preprocessor output plus callback-derived includes and macro events.
#pragma once
#include <string_view>
namespace frost::clang_engine {
int runSemanticPreprocess(std::string_view resourceDirectory);
}
