/**
 * @file src/foveation.cpp
 * @brief Definitions for foveated encoding: per-block QP delta maps around a focus point.
 */
// this include
#include "foveation.h"

// standard includes
#include <algorithm>
#include <cmath>

namespace foveation {

  mode_e mode_from_view(std::string_view value) {
    if (value == "fixed") {
      return mode_e::fixed;
    }
    return mode_e::disabled;
  }

  std::string_view mode_to_string(mode_e mode) {
    switch (mode) {
      case mode_e::fixed:
        return "fixed";
      case mode_e::disabled:
      default:
        return "disabled";
    }
  }

  std::uint32_t blocks_for(std::uint32_t pixels, std::uint32_t block_size) {
    if (block_size == 0) {
      return 0;
    }
    return pixels / block_size + (pixels % block_size ? 1 : 0);
  }

  std::vector<std::int8_t> build_qp_delta_map(
    const config_t &config,
    std::uint32_t frame_width,
    std::uint32_t frame_height,
    std::uint32_t block_size
  ) {
    const auto width_in_blocks = blocks_for(frame_width, block_size);
    const auto height_in_blocks = blocks_for(frame_height, block_size);
    if (config.mode == mode_e::disabled || width_in_blocks == 0 || height_in_blocks == 0) {
      return {};
    }

    const auto clamp_fraction = [](double value) {
      return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.5;
    };
    const auto clamp_length = [](double value) {
      return std::isfinite(value) ? std::max(value, 0.0) : 0.0;
    };

    const int strength = std::clamp(config.strength, 0, max_qp_delta);
    const int fovea_delta = -(strength / 2);

    const double center_x = clamp_fraction(config.center_x) * frame_width;
    const double center_y = clamp_fraction(config.center_y) * frame_height;
    const double radius = clamp_length(config.radius) * frame_height;
    const double falloff = clamp_length(config.falloff) * frame_height;

    std::vector<std::int8_t> map(static_cast<std::size_t>(width_in_blocks) * height_in_blocks);
    for (std::uint32_t by = 0; by < height_in_blocks; ++by) {
      // Measure from the centre of the visible part of each block, so partial edge blocks behave
      const double block_top = static_cast<double>(by) * block_size;
      const double y = (block_top + std::min<double>(block_top + block_size, frame_height)) / 2.0;

      for (std::uint32_t bx = 0; bx < width_in_blocks; ++bx) {
        const double block_left = static_cast<double>(bx) * block_size;
        const double x = (block_left + std::min<double>(block_left + block_size, frame_width)) / 2.0;

        const double distance = std::hypot(x - center_x, y - center_y);
        double weight;
        if (distance <= radius) {
          weight = 0.0;
        } else if (falloff <= 0.0 || distance >= radius + falloff) {
          weight = 1.0;
        } else {
          const double t = (distance - radius) / falloff;
          weight = t * t * (3.0 - 2.0 * t);
        }

        const int delta = fovea_delta + static_cast<int>(std::lround(weight * strength));
        map[static_cast<std::size_t>(by) * width_in_blocks + bx] =
          static_cast<std::int8_t>(std::clamp(delta, -max_qp_delta, max_qp_delta));
      }
    }

    return map;
  }

}  // namespace foveation
