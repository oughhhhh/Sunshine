/**
 * @file src/platform/windows/capture_region.h
 * @brief Geometry and pixel layout for Desktop Duplication capture regions.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace platf::dxgi {
  /**
   * @brief Clockwise quarter turns from the duplication texture to the desktop.
   */
  enum class capture_rotation_e {
    identity,  ///< No rotation.
    clockwise_90,  ///< One clockwise quarter turn.
    clockwise_180,  ///< Two clockwise quarter turns.
    clockwise_270  ///< Three clockwise quarter turns.
  };

  /**
   * @brief A capture rectangle and the dimensions of the surface containing it.
   */
  struct capture_region_t {
    int full_width = 0;  ///< Width of the containing surface.
    int full_height = 0;  ///< Height of the containing surface.
    int x = 0;  ///< Left edge of the capture rectangle.
    int y = 0;  ///< Top edge of the capture rectangle.
    int width = 0;  ///< Width of the capture rectangle.
    int height = 0;  ///< Height of the capture rectangle.

    /**
     * @brief Check whether the rectangle covers only part of the surface.
     * @return True when a subresource copy is needed.
     */
    bool is_cropped() const {
      return width != full_width || height != full_height;
    }
  };

  /**
   * @brief Compute a centered square capture region in desktop coordinates.
   * @param width Desktop width in pixels.
   * @param height Desktop height in pixels.
   * @param roi_size Requested square side in pixels, or zero for the full desktop.
   * @return The capture rectangle, bounded by the desktop dimensions.
   * @throws std::invalid_argument If the desktop dimensions or region size are invalid.
   */
  inline capture_region_t capture_region(int width, int height, int roi_size) {
    if (width <= 0 || height <= 0 || roi_size < 0) {
      throw std::invalid_argument("invalid capture geometry");
    }
    if (roi_size == 0) {
      return {width, height, 0, 0, width, height};
    }

    const int side = std::min({roi_size, width, height});
    const int x = std::clamp(width / 2 - side / 2, 0, width - side);
    const int y = std::clamp(height / 2 - side / 2, 0, height - side);
    return {width, height, x, y, side, side};
  }

  /**
   * @brief Express a desktop rectangle in the unrotated duplication texture.
   * @param region Rectangle in oriented desktop coordinates.
   * @param rotation Rotation from the texture to the desktop.
   * @return The same rectangle in texture coordinates.
   */
  inline capture_region_t duplication_region(const capture_region_t &region, capture_rotation_e rotation) {
    const auto &[full_width, full_height, x, y, width, height] = region;
    switch (rotation) {
      case capture_rotation_e::clockwise_90:
        return {full_height, full_width, y, full_width - x - width, height, width};
      case capture_rotation_e::clockwise_180:
        return {full_width, full_height, full_width - x - width, full_height - y - height, width, height};
      case capture_rotation_e::clockwise_270:
        return {full_height, full_width, full_height - y - height, x, height, width};
      case capture_rotation_e::identity:
        return region;
    }
    return region;
  }

  /**
   * @brief Copy a mapped packed texture into an upright RAM frame.
   * @param source First byte of the mapped texture.
   * @param source_pitch Byte stride between rows in the mapped texture.
   * @param destination First byte of the destination frame.
   * @param destination_pitch Byte stride between rows in the destination frame.
   * @param width Destination width in pixels.
   * @param height Destination height in pixels.
   * @param pixel_pitch Bytes per packed pixel.
   * @param rotation Rotation from the texture to the desktop.
   */
  inline void copy_captured_frame(const std::uint8_t *source, std::ptrdiff_t source_pitch, std::uint8_t *destination, std::ptrdiff_t destination_pitch, int width, int height, std::ptrdiff_t pixel_pitch, capture_rotation_e rotation) {
    if (rotation == capture_rotation_e::identity) {
      if (source_pitch == destination_pitch) {
        std::memcpy(destination, source, source_pitch * height);
      } else {
        for (int y = 0; y < height; ++y) {
          std::memcpy(destination + y * destination_pitch, source + y * source_pitch, width * pixel_pitch);
        }
      }
      return;
    }

    // Source offset of the destination's top-left pixel and strides along its axes.
    std::ptrdiff_t start = 0;
    std::ptrdiff_t x_step = pixel_pitch;
    std::ptrdiff_t y_step = source_pitch;
    switch (rotation) {
      case capture_rotation_e::clockwise_90:
        start = (width - 1) * source_pitch;
        x_step = -source_pitch;
        y_step = pixel_pitch;
        break;
      case capture_rotation_e::clockwise_180:
        start = (height - 1) * source_pitch + (width - 1) * pixel_pitch;
        x_step = -pixel_pitch;
        y_step = -source_pitch;
        break;
      case capture_rotation_e::clockwise_270:
        start = (height - 1) * pixel_pitch;
        x_step = source_pitch;
        y_step = -pixel_pitch;
        break;
      case capture_rotation_e::identity:
        break;
    }

    for (int y = 0; y < height; ++y) {
      const auto *src_row = source + start + y * y_step;
      auto *dst_row = destination + y * destination_pitch;
      for (int x = 0; x < width; ++x) {
        std::memcpy(dst_row + x * pixel_pitch, src_row + x * x_step, pixel_pitch);
      }
    }
  }
}  // namespace platf::dxgi
