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

awj::ImageBuffer make_high_bit_depth_test_image(int bit_depth) {
  const auto maximum = static_cast<unsigned int>((1u << bit_depth) - 1u);
  awj::ImagePlane plane{.stride = 16};
  const auto append_sample = [&plane](unsigned int sample) {
    plane.bytes.push_back(std::byte{static_cast<unsigned char>(sample & 0xffu)});
    plane.bytes.push_back(std::byte{static_cast<unsigned char>(sample >> 8u)});
  };
  for (unsigned int channel = 0; channel < 4; ++channel) {
    append_sample(maximum);
  }
  append_sample(0);
  append_sample(0);
  append_sample(0);
  append_sample(maximum);
  awj::ImageBuffer image{.width = 2,
                          .height = 1,
                          .pixel_format = awj::PixelFormat::rgba,
                          .alpha_mode = awj::AlphaMode::straight,
                          .bit_depth = bit_depth};
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

  for (const int bit_depth : {10, 16}) {
    auto high_bit_depth = awj::make_luma_image(make_high_bit_depth_test_image(bit_depth));
    if (!high_bit_depth || high_bit_depth->pixels.size() != 2 ||
        !nearly_equal(high_bit_depth->pixels[0], 1.0) ||
        !nearly_equal(high_bit_depth->pixels[1], 0.0)) {
      return fail(high_bit_depth ? "高位深图像 luma 归一化无效。"
                                 : high_bit_depth.error());
    }
  }

  return 0;
}
