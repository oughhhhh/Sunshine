/**
 * @file src/platform/windows/display_ram.cpp
 * @brief Definitions for handling ram.
 */
// local includes
#include "display.h"
#include "misc.h"
#include "src/logging.h"

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {
  /**
   * @brief Captured frame buffer shared between capture and encode stages.
   */
  struct img_t: public ::platf::img_t {
    /**
     * @brief Destroy the RAM-backed DXGI image.
     */
    ~img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  /**
   * @brief Blend a monochrome cursor shape into a RAM frame.
   *
   * @param cursor Cursor image or visibility state to composite.
   * @param img Image or frame object to read from or populate.
   * @param bounds Visible portion of the cursor in frame coordinates.
   */
  void blend_cursor_monochrome(const cursor_t &cursor, img_t &img, const RECT &bounds) {
    const auto mask_height = cursor.shape_info.Height / 2;
    const auto pitch = cursor.shape_info.Pitch;

    for (LONG y = bounds.top; y < bounds.bottom; ++y) {
      const auto *and_mask = cursor.img_data.data() + (y - cursor.y) * pitch;
      const auto *xor_mask = and_mask + mask_height * pitch;
      auto *pixel = reinterpret_cast<int *>(img.data + y * img.row_pitch) + bounds.left;

      for (LONG x = bounds.left; x < bounds.right; ++x) {
        const auto cursor_x = x - cursor.x;
        const auto mask_byte = cursor_x / 8;
        const auto mask_bit = 0x80 >> (cursor_x % 8);
        *pixel &= (and_mask[mask_byte] & mask_bit) ? -1 : 0;
        *pixel ^= (xor_mask[mask_byte] & mask_bit) ? -1 : 0;
        ++pixel;
      }
    }
  }

  /**
   * @brief Alpha-blend a color cursor pixel into a captured RAM frame.
   *
   * @param img_pixel_p Pixel in the destination frame.
   * @param cursor_pixel ARGB cursor pixel to blend.
   */
  void apply_color_alpha(int *img_pixel_p, int cursor_pixel) {
    auto colors_out = (std::uint8_t *) &cursor_pixel;
    auto colors_in = (std::uint8_t *) img_pixel_p;

    // TODO: When use of IDXGIOutput5 is implemented, support different color formats
    auto alpha = colors_out[3];
    if (alpha == 255) {
      *img_pixel_p = cursor_pixel;
    } else {
      colors_in[0] = colors_out[0] + (colors_in[0] * (255 - alpha) + 255 / 2) / 255;
      colors_in[1] = colors_out[1] + (colors_in[1] * (255 - alpha) + 255 / 2) / 255;
      colors_in[2] = colors_out[2] + (colors_in[2] * (255 - alpha) + 255 / 2) / 255;
    }
  }

  /**
   * @brief Apply a masked-color cursor pixel into a captured RAM frame.
   *
   * @param img_pixel_p Pixel in the destination frame.
   * @param cursor_pixel ARGB cursor pixel with masked-cursor semantics.
   */
  void apply_color_masked(int *img_pixel_p, int cursor_pixel) {
    // TODO: When use of IDXGIOutput5 is implemented, support different color formats
    auto alpha = ((std::uint8_t *) &cursor_pixel)[3];
    if (alpha == 0xFF) {
      *img_pixel_p ^= cursor_pixel;
    } else {
      *img_pixel_p = cursor_pixel;
    }
  }

  /**
   * @brief Blend a color cursor shape into a RAM frame.
   *
   * @param cursor Cursor image or visibility state to composite.
   * @param img Image or frame object to read from or populate.
   * @param bounds Visible portion of the cursor in frame coordinates.
   * @param masked Whether the cursor image uses an AND/XOR mask.
   */
  void blend_cursor_color(const cursor_t &cursor, img_t &img, const RECT &bounds, bool masked) {
    const auto pitch = cursor.shape_info.Pitch;
    const auto apply_pixel = masked ? apply_color_masked : apply_color_alpha;

    for (LONG y = bounds.top; y < bounds.bottom; ++y) {
      const auto *cursor_row = reinterpret_cast<const int *>(cursor.img_data.data() + (y - cursor.y) * pitch);
      const auto *cursor_begin = cursor_row + (bounds.left - cursor.x);
      const auto *cursor_end = cursor_begin + (bounds.right - bounds.left);
      auto *pixel = reinterpret_cast<int *>(img.data + y * img.row_pitch) + bounds.left;
      std::for_each(cursor_begin, cursor_end, [&](int cursor_pixel) {
        apply_pixel(pixel, cursor_pixel);
        ++pixel;
      });
    }
  }

  /**
   * @brief Blend the current cursor image into a RAM frame.
   *
   * @param cursor Cursor image or visibility state to composite.
   * @param img Image or frame object to read from or populate.
   */
  void blend_cursor(const cursor_t &cursor, img_t &img) {
    const auto shape_width = static_cast<LONG>(cursor.shape_info.Width);
    const int mask_planes = cursor.shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? 2 : 1;
    const auto shape_height = static_cast<LONG>(cursor.shape_info.Height) / mask_planes;
    const RECT frame_bounds {0, 0, img.width, img.height};
    const RECT cursor_bounds {cursor.x, cursor.y, cursor.x + shape_width, cursor.y + shape_height};
    RECT bounds;
    if (!IntersectRect(&bounds, &frame_bounds, &cursor_bounds)) {
      return;
    }

    switch (cursor.shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        blend_cursor_color(cursor, img, bounds, false);
        break;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        blend_cursor_monochrome(cursor, img, bounds);
        break;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        blend_cursor_color(cursor, img, bounds, true);
        break;
      default:
        BOOST_LOG(warning) << "Unsupported cursor format ["sv << cursor.shape_info.Type << ']';
    }
  }

  capture_e display_ddup_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    resource_t::pointer res_p {};
    auto capture_status = dup.next_frame(frame_info, timeout, &res_p);
    resource_t res {res_p};

    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    const bool mouse_update_flag = frame_info.LastMouseUpdateTime.QuadPart != 0 || frame_info.PointerShapeBufferSize > 0;
    const bool frame_update_flag = frame_info.AccumulatedFrames != 0 || frame_info.LastPresentTime.QuadPart != 0;
    const bool update_flag = mouse_update_flag || frame_update_flag;

    if (!update_flag) {
      return capture_e::timeout;
    }

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    if (auto qpc_displayed = std::max(frame_info.LastPresentTime.QuadPart, frame_info.LastMouseUpdateTime.QuadPart)) {
      // Translate QueryPerformanceCounter() value to steady_clock time point
      frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), qpc_displayed);
    }

    if (frame_info.PointerShapeBufferSize > 0) {
      auto &img_data = cursor.img_data;

      img_data.resize(frame_info.PointerShapeBufferSize);

      UINT dummy;
      status = dup.dup->GetFramePointerShape(img_data.size(), img_data.data(), &dummy, &cursor.shape_info);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to get new pointer shape [0x"sv << util::hex(status).to_string_view() << ']';

        return capture_e::error;
      }
    }

    if (frame_info.LastMouseUpdateTime.QuadPart) {
      cursor.x = frame_info.PointerPosition.Position.x - dup.region.x;
      cursor.y = frame_info.PointerPosition.Position.y - dup.region.y;
      cursor.visible = frame_info.PointerPosition.Visible;
    }

    if (frame_update_flag) {
      {
        texture2d_t src {};
        status = res->QueryInterface(IID_ID3D11Texture2D, (void **) &src);

        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't query interface [0x"sv << util::hex(status).to_string_view() << ']';
          return capture_e::error;
        }

        D3D11_TEXTURE2D_DESC desc;
        src->GetDesc(&desc);

        // Validate the full acquired texture before copying the region.
        if (!dup.source_size_matches(desc)) {
          return capture_e::reinit;
        }

        // If we don't know the capture format yet, grab it from this texture and create the staging texture
        if (capture_format == DXGI_FORMAT_UNKNOWN) {
          D3D11_TEXTURE2D_DESC t {};
          t.Width = width_before_rotation;
          t.Height = height_before_rotation;
          t.MipLevels = 1;
          t.ArraySize = 1;
          t.SampleDesc.Count = 1;
          t.Usage = D3D11_USAGE_STAGING;
          t.Format = desc.Format;
          t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

          auto status = device->CreateTexture2D(&t, nullptr, &texture);

          if (FAILED(status)) {
            BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
            return capture_e::error;
          }

          capture_format = desc.Format;
          BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
        }

        // It's also possible for the capture format to change on the fly. If that happens,
        // reinitialize capture to try format detection again and create new images.
        if (capture_format != desc.Format) {
          BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
          return capture_e::reinit;
        }

        dup.copy_frame(device_ctx.get(), texture.get(), src.get());
      }
    }

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // If we don't know the final capture format yet, encode a dummy image
    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(debug) << "Capture format is still unknown. Encoding a blank image"sv;

      if (dummy_img(img)) {
        return capture_e::error;
      }
    } else {
      // Map the staging texture for CPU access (making it inaccessible for the GPU)
      status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

        return capture_e::error;
      }

      // Now that we know the capture format, we can finish creating the image
      const int row_pitch = dup.rotation == capture_rotation_e::identity ? static_cast<int>(img_info.RowPitch) : width * get_pixel_pitch();
      if (complete_img(img, false, row_pitch)) {
        device_ctx->Unmap(texture.get(), 0);
        img_info.pData = nullptr;
        return capture_e::error;
      }

      copy_captured_frame(static_cast<const std::uint8_t *>(img_info.pData), img_info.RowPitch, img->data, img->row_pitch, width, height, img->pixel_pitch, dup.rotation);

      // Unmap the staging texture to allow GPU access again
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
    }

    if (cursor_visible && cursor.visible) {
      blend_cursor(cursor, *img);
    }

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_ddup_ram_t::release_snapshot() {
    return dup.release_frame();
  }

  std::shared_ptr<platf::img_t> display_ram_t::alloc_img() {
    auto img = std::make_shared<img_t>();

    // Initialize fields that are format-independent
    img->width = width;
    img->height = height;

    return img;
  }

  int display_ram_t::complete_img(platf::img_t *img, bool dummy) {
    return complete_img(img, dummy, dummy ? 0 : static_cast<int>(img_info.RowPitch));
  }

  int display_ram_t::complete_img(platf::img_t *img, bool dummy, int row_pitch) {
    // If this is not a dummy image, we must know the format by now
    if (!dummy && capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(error) << "display_ram_t::complete_img() called with unknown capture format!";
      return -1;
    }

    img->pixel_pitch = get_pixel_pitch();

    if (dummy && !img->row_pitch) {
      // Assume our dummy image will have no padding
      img->row_pitch = img->pixel_pitch * img->width;
    }

    // Reallocate the image buffer if the pitch changes
    if (!dummy && img->row_pitch != row_pitch) {
      img->row_pitch = row_pitch;
      delete[] img->data;
      img->data = nullptr;
    }

    if (!img->data) {
      img->data = new std::uint8_t[img->row_pitch * height];
    }

    return 0;
  }

  /**
   * @memberof platf::dxgi::display_ram_t
   */
  int display_ram_t::dummy_img(platf::img_t *img) {
    if (complete_img(img, true)) {
      return -1;
    }

    std::fill_n((std::uint8_t *) img->data, height * img->row_pitch, 0);
    return 0;
  }

  std::vector<DXGI_FORMAT> display_ram_t::get_supported_capture_formats() {
    return {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM};
  }

  int display_ddup_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    return 0;
  }

  std::unique_ptr<avcodec_encode_device_t> display_ram_t::make_avcodec_encode_device(pix_fmt_e pix_fmt) {
    return std::make_unique<avcodec_encode_device_t>();
  }

}  // namespace platf::dxgi
