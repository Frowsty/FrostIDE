// SPDX-License-Identifier: Apache-2.0
//
// The synthetic layout tables of the frost::input tests (synthetic_layouts.hpp).

#include "input/support/synthetic_layouts.hpp"

#include "frost/input/simulated_keyboard.hpp"

#include <gtest/gtest.h>

#include <map>

namespace frost::input::test {

LayoutTable usWith(const std::vector<std::pair<Code, KeyLevels>>& changes, std::string_view id) {
    std::map<Code, KeyLevels> cells;
    const LayoutTable& us = simulatedLayout(SimulatedLayoutId::EnUs);
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlBackslash); ++i) {
        cells[static_cast<Code>(i)] = us.levels(static_cast<Code>(i));
    }
    for (const auto& [code, levels] : changes) {
        cells[code] = levels;
    }
    const std::vector<std::pair<Code, KeyLevels>> keys(cells.begin(), cells.end());
    auto table = LayoutTable::make(id, MappingMode::FullTable, keys, {});
    EXPECT_TRUE(table.has_value()) << id;
    return table ? *table : us;
}

namespace synthetic {

namespace {

constexpr KeyLevels chars(char32_t base, char32_t shift) { return KeyLevels{base, shift, 0, 0, 0, 0}; }

} // namespace

LayoutTable noSlash() {
    return usWith({{Code::Slash, chars(U'-', U'_')}, {Code::Minus, KeyLevels{}}}, "synthetic:noSlash");
}

LayoutTable swissLike() {
    return usWith(
        {{Code::Minus, chars(U'\'', U'?')}, {Code::Quote, chars(U'ä', U'à')}, {Code::Slash, chars(U'-', U'_')}},
        "synthetic:swissLike");
}

LayoutTable backwardChain() {
    return usWith({{Code::Minus, chars(U'/', U'?')}, {Code::Slash, chars(U'\'', U'"')}}, "synthetic:backwardChain");
}

LayoutTable frLike() {
    return usWith(
        {{Code::Comma, chars(U';', U'.')}, {Code::Semicolon, chars(U'm', U'M')}, {Code::KeyM, chars(U',', U'?')}},
        "synthetic:frLike");
}

LayoutTable swap() {
    return usWith({{Code::Slash, chars(U'-', U'_')}, {Code::Minus, chars(U'/', U'?')}}, "synthetic:swap");
}

LayoutTable twoProducers() {
    return usWith({{Code::Comma, chars(U'-', U'<')}, {Code::Slash, chars(U'-', U'_')}, {Code::Minus, KeyLevels{}}},
                  "synthetic:twoProducers");
}

} // namespace synthetic

} // namespace frost::input::test
