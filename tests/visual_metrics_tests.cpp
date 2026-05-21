#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string_view>

import awj.image;
import awj.visual_metrics;
import awj.visual_quality;

namespace {

bool nearly_equal(double left, double right, double epsilon = 1e-9) {
  return std::abs(left - right) <= epsilon;
}

int fail(std::string_view message) {
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

awj::ImageBuffer make_test_image(std::byte changed_blue) {
  awj::ImagePlane plane{.stride = 8};
  plane.bytes = {
      std::byte{255}, std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{0},   changed_blue,   std::byte{255},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
  };
  awj::ImageBuffer image{.width = 2,
                          .height = 2,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = 8};
  image.planes.push_back(std::move(plane));
  return image;
}

}  // namespace

int main() {
  auto reference = awj::make_luma_image(make_test_image(std::byte{255}));
  if (!reference) {
    return fail(reference.error());
  }
  auto identical = awj::make_luma_image(make_test_image(std::byte{255}));
  if (!identical) {
    return fail(identical.error());
  }
  auto changed = awj::make_luma_image(make_test_image(std::byte{0}));
  if (!changed) {
    return fail(changed.error());
  }

  auto identical_gmsd = awj::compute_gmsd(*reference, *identical);
  if (!identical_gmsd || !nearly_equal(*identical_gmsd, 0.0)) {
    return fail("相同图像 GMSD 应为 0。");
  }
  auto identical_ms_ssim = awj::compute_ms_ssim(*reference, *identical);
  if (!identical_ms_ssim || !nearly_equal(*identical_ms_ssim, 1.0)) {
    return fail("相同图像 MS-SSIM 应为 1。");
  }

  auto changed_gmsd = awj::compute_gmsd(*reference, *changed);
  if (!changed_gmsd || *changed_gmsd < 0.0) {
    return fail("变化图像 GMSD 无效。");
  }
  auto changed_ms_ssim = awj::compute_ms_ssim(*reference, *changed);
  if (!changed_ms_ssim || *changed_ms_ssim >= 1.0) {
    return fail("变化图像 MS-SSIM 应低于 1。");
  }

  auto score = awj::calculate_visual_score(*reference, *changed);
  if (!score || score->visual_score < 1.0 || score->visual_score > 99.0) {
    return fail("视觉指标 score 范围无效。");
  }

  return 0;
}
