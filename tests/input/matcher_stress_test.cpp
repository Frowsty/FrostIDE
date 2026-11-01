// SPDX-License-Identifier: Apache-2.0
//
// ShortcutMatcherStress: the random-stream matcher property of matcher_rig.hpp at stress
// size, 50,000 streams of 32 events per layout and platform (input_tests runs 2,000).

#include "input/support/matcher_rig.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <string>
#include <tuple>

namespace frost::input {
namespace {

class ShortcutMatcherStress : public ::testing::TestWithParam<std::tuple<SimulatedLayoutId, KeyPlatform>> {};

TEST_P(ShortcutMatcherStress, RandomStreams) {
    const auto [layout, platform] = GetParam();
    test::PropertyStats stats;
    test::runMatcherProperty(simulatedLayout(layout), platform, 0x57AE55000U + static_cast<unsigned>(layout), 50'000,
                             32, stats);
    EXPECT_GE(stats.events, 50'000U * 32U);
    EXPECT_GT(stats.sequenceFires, 0U);
    EXPECT_GT(stats.gestureFires, 0U);
}

INSTANTIATE_TEST_SUITE_P(Layouts, ShortcutMatcherStress,
                         ::testing::Combine(::testing::Values(SimulatedLayoutId::EnUs, SimulatedLayoutId::DeDe,
                                                              SimulatedLayoutId::PlPl, SimulatedLayoutId::FrFr),
                                            ::testing::Values(KeyPlatform::Windows, KeyPlatform::X11)),
                         [](const auto& info) {
                             std::string name(simulatedLayoutName(std::get<0>(info.param)));
                             std::erase(name, '-');
                             return name + (std::get<1>(info.param) == KeyPlatform::Windows ? "_Windows" : "_X11");
                         });

} // namespace
} // namespace frost::input
