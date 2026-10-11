/**
 * @file src/foveation.h
 * @brief Declarations for foveated encoding: per-block QP delta maps around a focus point.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string_view>
#include <vector>

namespace foveation {

  enum class mode_e : std::uint8_t {
    disabled,  ///< Uniform quality across the frame
    fixed,  ///< Fovea at a configured, fixed point of the frame
  };

  /**
   * @brief Foveation settings shared by the encoders that support per-block QP control.
   */
  struct config_t {
    mode_e mode = mode_e::disabled;

    // Fovea centre as a fraction of the frame width and height, 0.0 is left/top
    double center_x = 0.5;
    double center_y = 0.5;

    // Radius of the full-quality fovea as a fraction of the frame height
    double radius = 0.15;

    // Width of the smooth transition band outside the fovea as a fraction of the frame height
    double falloff = 0.20;

    // QP difference between the fovea and the far periphery
    int strength = 10;
  };

  /** Largest QP delta any supported codec accepts. */
  inline constexpr int max_qp_delta = 51;

  /** HEVC coding tree block size used by NVENC, which only supports a 32x32 maximum CU size. */
  inline constexpr std::uint32_t nvenc_hevc_block_size = 32;

  [[nodiscard]] mode_e mode_from_view(std::string_view value);

  [[nodiscard]] std::string_view mode_to_string(mode_e mode);

  /** Number of blocks of `block_size` needed to cover `pixels`, rounding partial blocks up. */
  [[nodiscard]] std::uint32_t blocks_for(std::uint32_t pixels, std::uint32_t block_size);

  /**
   * @brief Build a raster-order signed QP delta per block.
   *
   * Blocks within the fovea get a negative delta (sharper), blocks beyond the transition band
   * get a positive delta (coarser), with a smoothstep falloff between them. Distances are measured
   * in pixels, so the fovea stays circular at any aspect ratio. The fovea delta is -strength/2 and
   * the periphery delta is strength - strength/2, so the two always differ by exactly `strength`.
   *
   * @param config Foveation settings; out-of-range values are clamped.
   * @param frame_width Encoded frame width in pixels.
   * @param frame_height Encoded frame height in pixels.
   * @param block_size Codec block size in pixels (16 for H.264 MBs, 32 for NVENC HEVC CTBs).
   * @return One delta per block, or an empty map if foveation is disabled or the frame is empty.
   */
  [[nodiscard]] std::vector<std::int8_t> build_qp_delta_map(
    const config_t &config,
    std::uint32_t frame_width,
    std::uint32_t frame_height,
    std::uint32_t block_size
  );

}  // namespace foveation
