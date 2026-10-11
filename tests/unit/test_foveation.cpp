/**
 * @file tests/unit/test_foveation.cpp
 * @brief Test the foveation QP delta map builder.
 */
#include <gtest/gtest.h>

#include "src/foveation.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

  foveation::config_t fixed_config() {
    foveation::config_t config;
    config.mode = foveation::mode_e::fixed;
    return config;
  }

  std::int8_t at(const std::vector<std::int8_t> &map, std::uint32_t width_in_blocks, std::uint32_t bx, std::uint32_t by) {
    return map[static_cast<std::size_t>(by) * width_in_blocks + bx];
  }

}  // namespace

TEST(FoveationTest, ModeParsesKnownValuesAndDefaultsToDisabled) {
  EXPECT_EQ(foveation::mode_from_view("fixed"), foveation::mode_e::fixed);
  EXPECT_EQ(foveation::mode_from_view("disabled"), foveation::mode_e::disabled);
  EXPECT_EQ(foveation::mode_from_view("bogus"), foveation::mode_e::disabled);
  EXPECT_EQ(foveation::mode_to_string(foveation::mode_e::fixed), "fixed");
  EXPECT_EQ(foveation::mode_to_string(foveation::mode_e::disabled), "disabled");
}

TEST(FoveationTest, DefaultConfigIsDisabled) {
  const foveation::config_t config;
  EXPECT_EQ(config.mode, foveation::mode_e::disabled);
  EXPECT_TRUE(foveation::build_qp_delta_map(config, 1920, 1080, 32).empty());
}

TEST(FoveationTest, BlocksForRoundsPartialBlocksUp) {
  EXPECT_EQ(foveation::blocks_for(1920, 32), 60u);
  EXPECT_EQ(foveation::blocks_for(1080, 32), 34u);
  EXPECT_EQ(foveation::blocks_for(1080, 16), 68u);
  EXPECT_EQ(foveation::blocks_for(32, 32), 1u);
  EXPECT_EQ(foveation::blocks_for(33, 32), 2u);
  EXPECT_EQ(foveation::blocks_for(0, 32), 0u);
  EXPECT_EQ(foveation::blocks_for(1920, 0), 0u);
}

TEST(FoveationTest, MapSizeMatchesCodecBlockGrid) {
  const auto config = fixed_config();
  EXPECT_EQ(foveation::build_qp_delta_map(config, 1920, 1080, 32).size(), 60u * 34u);
  EXPECT_EQ(foveation::build_qp_delta_map(config, 1920, 1080, 16).size(), 120u * 68u);
  EXPECT_EQ(foveation::build_qp_delta_map(config, 3840, 2160, 32).size(), 120u * 68u);
  EXPECT_EQ(foveation::build_qp_delta_map(config, 2560, 1440, 32).size(), 80u * 45u);
  EXPECT_TRUE(foveation::build_qp_delta_map(config, 0, 1080, 32).empty());
  EXPECT_TRUE(foveation::build_qp_delta_map(config, 1920, 1080, 0).empty());
}

TEST(FoveationTest, CentreIsSharpAndCornersAreCoarse) {
  auto config = fixed_config();
  config.strength = 10;
  const auto map = foveation::build_qp_delta_map(config, 1920, 1080, 32);
  ASSERT_EQ(map.size(), 60u * 34u);

  EXPECT_EQ(at(map, 60, 30, 17), -5);
  EXPECT_EQ(at(map, 60, 0, 0), 5);
  EXPECT_EQ(at(map, 60, 59, 0), 5);
  EXPECT_EQ(at(map, 60, 0, 33), 5);
  EXPECT_EQ(at(map, 60, 59, 33), 5);
  EXPECT_EQ(*std::min_element(map.begin(), map.end()), -5);
  EXPECT_EQ(*std::max_element(map.begin(), map.end()), 5);
}

TEST(FoveationTest, DeltaRisesMonotonicallyAwayFromTheCentre) {
  auto config = fixed_config();
  config.strength = 20;
  const auto map = foveation::build_qp_delta_map(config, 1920, 1080, 32);

  // Walk right along the centre row; the falloff must never step back towards the fovea
  std::vector<int> transition;
  for (std::uint32_t bx = 30; bx < 60; ++bx) {
    const int delta = at(map, 60, bx, 17);
    if (bx > 30) {
      EXPECT_GE(delta, at(map, 60, bx - 1, 17)) << "block " << bx;
    }
    if (delta > -10 && delta < 10) {
      transition.push_back(delta);
    }
  }

  // A smooth falloff passes through several intermediate values rather than one hard step
  EXPECT_GE(transition.size(), 3u);
}

TEST(FoveationTest, ZeroFalloffGivesAHardEdge) {
  auto config = fixed_config();
  config.falloff = 0.0;
  config.strength = 8;
  const auto map = foveation::build_qp_delta_map(config, 1920, 1080, 32);
  for (const auto delta : map) {
    EXPECT_TRUE(delta == -4 || delta == 4) << static_cast<int>(delta);
  }
}

TEST(FoveationTest, FoveaIsCircularInPixelsNotBlocks) {
  auto config = fixed_config();
  config.radius = 0.25;
  config.falloff = 0.0;
  const auto map = foveation::build_qp_delta_map(config, 1920, 1080, 32);

  // 0.25 of the height is 270 px. A block centred 240 px right of the centre is inside, at 304 px it is outside.
  EXPECT_LT(at(map, 60, 37, 17), 0);
  EXPECT_LT(at(map, 60, 22, 17), 0);
  EXPECT_GT(at(map, 60, 39, 17), 0);
  EXPECT_GT(at(map, 60, 20, 17), 0);
  // The same radius vertically
  EXPECT_LT(at(map, 60, 30, 9), 0);
  EXPECT_GT(at(map, 60, 30, 7), 0);
}

TEST(FoveationTest, CentreFollowsConfiguredPoint) {
  auto config = fixed_config();
  config.center_x = 0.0;
  config.center_y = 0.0;
  config.radius = 0.1;
  const auto map = foveation::build_qp_delta_map(config, 1920, 1080, 32);
  EXPECT_EQ(at(map, 60, 0, 0), -5);
  EXPECT_EQ(at(map, 60, 59, 33), 5);
  EXPECT_EQ(at(map, 60, 30, 17), 5);
}

TEST(FoveationTest, StrengthAndPositionAreClamped) {
  auto config = fixed_config();
  config.strength = 500;
  config.center_x = 7.0;
  config.center_y = -3.0;
  auto map = foveation::build_qp_delta_map(config, 1920, 1080, 32);
  ASSERT_FALSE(map.empty());
  EXPECT_EQ(*std::max_element(map.begin(), map.end()), foveation::max_qp_delta - foveation::max_qp_delta / 2);
  EXPECT_EQ(*std::min_element(map.begin(), map.end()), -(foveation::max_qp_delta / 2));
  // Clamped to the top-right corner
  EXPECT_EQ(at(map, 60, 59, 0), -(foveation::max_qp_delta / 2));

  config.strength = -4;
  map = foveation::build_qp_delta_map(config, 1920, 1080, 32);
  EXPECT_TRUE(std::all_of(map.begin(), map.end(), [](auto delta) {
    return delta == 0;
  }));
}

TEST(FoveationTest, OddStrengthKeepsTheFullSpread) {
  auto config = fixed_config();
  config.strength = 7;
  const auto map = foveation::build_qp_delta_map(config, 1920, 1080, 32);
  EXPECT_EQ(*std::min_element(map.begin(), map.end()), -3);
  EXPECT_EQ(*std::max_element(map.begin(), map.end()), 4);
}

TEST(FoveationTest, NonFiniteInputsFallBackToSafeValues) {
  auto config = fixed_config();
  config.center_x = std::numeric_limits<double>::quiet_NaN();
  config.radius = std::numeric_limits<double>::infinity();
  config.falloff = -1.0;
  const auto map = foveation::build_qp_delta_map(config, 1920, 1080, 32);
  ASSERT_EQ(map.size(), 60u * 34u);
  // An infinite radius would make the whole frame fovea, so it is treated as zero; the NaN centre becomes 0.5
  EXPECT_EQ(at(map, 60, 0, 0), 5);
  EXPECT_EQ(at(map, 60, 59, 33), 5);
}
