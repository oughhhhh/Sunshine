/**
 * @file tests/unit/platform/windows/test_capture_region.cpp
 * @brief Tests for Desktop Duplication crop and rotation geometry.
 */

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/platform/windows/capture_region.h"

namespace {
  using platf::dxgi::capture_region;
  using platf::dxgi::capture_region_t;
  using platf::dxgi::capture_rotation_e;
  using platf::dxgi::copy_captured_frame;
  using platf::dxgi::duplication_region;

  /**
   * @brief Expose region fields for equality assertions.
   * @param region Capture rectangle and its containing surface.
   * @return Surface dimensions, rectangle origin, and rectangle dimensions.
   */
  std::array<int, 6> geometry(const capture_region_t &region) {
    return {region.full_width, region.full_height, region.x, region.y, region.width, region.height};
  }

  TEST(CaptureRegionTest, ZeroUsesWholeOutput) {
    const auto region = capture_region(1920, 1080, 0);
    EXPECT_EQ(geometry(region), (std::array {1920, 1080, 0, 0, 1920, 1080}));
    EXPECT_FALSE(region.is_cropped());
  }

  TEST(CaptureRegionTest, CentersUsingIntegerHalves) {
    const auto region = capture_region(1920, 1081, 641);
    EXPECT_EQ(geometry(region), (std::array {1920, 1081, 640, 220, 641, 641}));
    EXPECT_TRUE(region.is_cropped());
  }

  TEST(CaptureRegionTest, ClampsToShorterDimension) {
    EXPECT_EQ(geometry(capture_region(720, 1281, 2000)), (std::array {720, 1281, 0, 280, 720, 720}));
    EXPECT_EQ(geometry(capture_region(1920, 1080, 2000)), (std::array {1920, 1080, 420, 0, 1080, 1080}));
    EXPECT_FALSE(capture_region(720, 720, 2000).is_cropped());
  }

  TEST(CaptureRegionTest, RejectsInvalidGeometry) {
    EXPECT_THROW(capture_region(0, 1080, 640), std::invalid_argument);
    EXPECT_THROW(capture_region(1920, -1, 640), std::invalid_argument);
    EXPECT_THROW(capture_region(1920, 1080, -1), std::invalid_argument);
  }

  TEST(CaptureRegionTest, MapsRegionIntoDuplicationTexture) {
    const capture_region_t region {10, 7, 2, 1, 3, 2};
    EXPECT_EQ(geometry(duplication_region(region, capture_rotation_e::identity)), geometry(region));
    EXPECT_EQ(geometry(duplication_region(region, capture_rotation_e::clockwise_90)), (std::array {7, 10, 1, 5, 2, 3}));
    EXPECT_EQ(geometry(duplication_region(region, capture_rotation_e::clockwise_180)), (std::array {10, 7, 5, 4, 3, 2}));
    EXPECT_EQ(geometry(duplication_region(region, capture_rotation_e::clockwise_270)), (std::array {7, 10, 4, 2, 2, 3}));
  }

  TEST(CaptureRegionTest, CopiesPackedPixelsIntoUprightFrame) {
    const auto verify = [](capture_rotation_e rotation, std::initializer_list<std::uint32_t> source, std::ptrdiff_t source_stride) {
      constexpr std::ptrdiff_t pixel_size = sizeof(std::uint32_t);
      constexpr int width = 3;
      constexpr int height = 2;
      std::array<std::uint32_t, width * height> destination {};
      copy_captured_frame(reinterpret_cast<const std::uint8_t *>(source.begin()), source_stride * pixel_size, reinterpret_cast<std::uint8_t *>(destination.data()), width * pixel_size, width, height, pixel_size, rotation);
      EXPECT_EQ(destination, (std::array<std::uint32_t, 6> {1, 2, 3, 4, 5, 6}));
    };

    verify(capture_rotation_e::identity, {1, 2, 3, 4, 5, 6}, 3);
    verify(capture_rotation_e::identity, {1, 2, 3, 0, 4, 5, 6, 0}, 4);
    verify(capture_rotation_e::clockwise_90, {3, 6, 0, 2, 5, 0, 1, 4, 0}, 3);
    verify(capture_rotation_e::clockwise_180, {6, 5, 4, 0, 3, 2, 1, 0}, 4);
    verify(capture_rotation_e::clockwise_270, {4, 1, 0, 5, 2, 0, 6, 3, 0}, 3);
  }
}  // namespace
