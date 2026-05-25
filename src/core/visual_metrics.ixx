module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <vector>

export module awj.visual_metrics;

import awj.image;
import awj.visual_quality;

export namespace awj {

struct LumaImage {
  std::size_t width{};
  std::size_t height{};
  std::vector<double> pixels{};

  [[nodiscard]] bool empty() const noexcept {
    return width == 0 || height == 0 || pixels.empty();
  }
};

export std::expected<LumaImage, std::string> make_luma_image(
    const ImageBuffer& image) {
  if (image.width == 0 || image.height == 0 || image.planes.empty()) {
    return std::unexpected{"输入图像为空，无法计算视觉指标。"};
  }
  if (image.bit_depth != 8) {
    return std::unexpected{"当前视觉指标仅支持 8-bit 图像。"};
  }

  if (image.width > std::numeric_limits<std::size_t>::max() / image.height) {
    return std::unexpected{"输入图像尺寸过大，无法计算视觉指标。"};
  }

  const auto& plane = image.planes.front();
  LumaImage luma{.width = image.width, .height = image.height};
  luma.pixels.resize(image.width * image.height);

  if (image.pixel_format == PixelFormat::rgba || image.pixel_format == PixelFormat::rgb) {
    const std::size_t channels = image.pixel_format == PixelFormat::rgba ? 4 : 3;
    if (image.width > std::numeric_limits<std::size_t>::max() / channels) {
      return std::unexpected{"RGB/RGBA 图像宽度过大，无法计算视觉指标。"};
    }
    const std::size_t min_stride = image.width * channels;
    if (plane.stride < min_stride ||
        plane.stride > std::numeric_limits<std::size_t>::max() / image.height ||
        plane.bytes.size() < plane.stride * image.height) {
      return std::unexpected{"RGB/RGBA 图像 buffer 尺寸无效，无法计算视觉指标。"};
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(plane.bytes.data());
    for (std::size_t y = 0; y < image.height; ++y) {
      const auto* row = bytes + y * plane.stride;
      for (std::size_t x = 0; x < image.width; ++x) {
        const auto* pixel = row + x * channels;
        luma.pixels[y * image.width + x] =
            (0.2126 * static_cast<double>(pixel[0]) +
             0.7152 * static_cast<double>(pixel[1]) +
             0.0722 * static_cast<double>(pixel[2])) /
            255.0;
      }
    }
    return luma;
  }

  if (image.pixel_format == PixelFormat::gray) {
    if (plane.stride < image.width ||
        plane.stride > std::numeric_limits<std::size_t>::max() / image.height ||
        plane.bytes.size() < plane.stride * image.height) {
      return std::unexpected{"灰度图像 buffer 尺寸无效，无法计算视觉指标。"};
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(plane.bytes.data());
    for (std::size_t y = 0; y < image.height; ++y) {
      const auto* row = bytes + y * plane.stride;
      for (std::size_t x = 0; x < image.width; ++x) {
        luma.pixels[y * image.width + x] = static_cast<double>(row[x]) / 255.0;
      }
    }
    return luma;
  }

  return std::unexpected{"当前视觉指标仅支持 RGB、RGBA 或灰度图像。"};
}

namespace visual_metrics_detail {

std::expected<void, std::string> validate_same_shape(const LumaImage& reference,
                                                     const LumaImage& candidate) {
  if (reference.empty() || candidate.empty()) {
    return std::unexpected{"视觉指标输入为空。"};
  }
  if (reference.width != candidate.width || reference.height != candidate.height ||
      reference.pixels.size() != candidate.pixels.size()) {
    return std::unexpected{"视觉指标输入尺寸不一致。"};
  }
  return {};
}

double pixel_at_clamped(const LumaImage& image, std::size_t x, std::size_t y) noexcept {
  x = std::min(x, image.width - 1);
  y = std::min(y, image.height - 1);
  return image.pixels[y * image.width + x];
}

double gradient_magnitude(const LumaImage& image, std::size_t x, std::size_t y) noexcept {
  const auto left = x == 0 ? x : x - 1;
  const auto right = std::min(x + 1, image.width - 1);
  const auto top = y == 0 ? y : y - 1;
  const auto bottom = std::min(y + 1, image.height - 1);
  const double dx = pixel_at_clamped(image, right, y) - pixel_at_clamped(image, left, y);
  const double dy = pixel_at_clamped(image, x, bottom) - pixel_at_clamped(image, x, top);
  return std::sqrt(dx * dx + dy * dy);
}

LumaImage downsample_2x(const LumaImage& source) {
  if (source.width <= 1 || source.height <= 1) {
    return source;
  }
  if (source.width > std::numeric_limits<std::size_t>::max() - 1 ||
      source.height > std::numeric_limits<std::size_t>::max() - 1) {
    return source;
  }
  LumaImage result{.width = (source.width + 1) / 2,
                   .height = (source.height + 1) / 2};
  if (result.width > std::numeric_limits<std::size_t>::max() / result.height) {
    return source;
  }
  result.pixels.resize(result.width * result.height);
  for (std::size_t y = 0; y < result.height; ++y) {
    for (std::size_t x = 0; x < result.width; ++x) {
      const std::size_t sx = x * 2;
      const std::size_t sy = y * 2;
      double sum = 0.0;
      double count = 0.0;
      for (std::size_t oy = 0; oy < 2 && sy + oy < source.height; ++oy) {
        for (std::size_t ox = 0; ox < 2 && sx + ox < source.width; ++ox) {
          sum += source.pixels[(sy + oy) * source.width + sx + ox];
          count += 1.0;
        }
      }
      result.pixels[y * result.width + x] = sum / count;
    }
  }
  return result;
}

double ssim_global(const LumaImage& reference, const LumaImage& candidate) noexcept {
  const auto count = static_cast<double>(reference.pixels.size());
  double mean_ref = 0.0;
  double mean_candidate = 0.0;
  for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
    mean_ref += reference.pixels[i];
    mean_candidate += candidate.pixels[i];
  }
  mean_ref /= count;
  mean_candidate /= count;

  double variance_ref = 0.0;
  double variance_candidate = 0.0;
  double covariance = 0.0;
  for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
    const double ref_delta = reference.pixels[i] - mean_ref;
    const double candidate_delta = candidate.pixels[i] - mean_candidate;
    variance_ref += ref_delta * ref_delta;
    variance_candidate += candidate_delta * candidate_delta;
    covariance += ref_delta * candidate_delta;
  }
  variance_ref /= count;
  variance_candidate /= count;
  covariance /= count;

  constexpr double k1 = 0.01;
  constexpr double k2 = 0.03;
  constexpr double c1 = k1 * k1;
  constexpr double c2 = k2 * k2;
  const double numerator = (2.0 * mean_ref * mean_candidate + c1) *
                           (2.0 * covariance + c2);
  const double denominator = (mean_ref * mean_ref + mean_candidate * mean_candidate + c1) *
                             (variance_ref + variance_candidate + c2);
  if (denominator <= 0.0) {
    return 1.0;
  }
  return std::clamp(numerator / denominator, 0.0, 1.0);
}

}  // namespace visual_metrics_detail

export std::expected<double, std::string> compute_gmsd(
    const LumaImage& reference,
    const LumaImage& candidate) {
  if (auto valid = visual_metrics_detail::validate_same_shape(reference, candidate); !valid) {
    return std::unexpected{valid.error()};
  }

  constexpr double c = 0.0026;
  std::vector<double> similarities;
  similarities.reserve(reference.pixels.size());
  double mean = 0.0;
  for (std::size_t y = 0; y < reference.height; ++y) {
    for (std::size_t x = 0; x < reference.width; ++x) {
      const double ref_grad = visual_metrics_detail::gradient_magnitude(reference, x, y);
      const double candidate_grad = visual_metrics_detail::gradient_magnitude(candidate, x, y);
      const double similarity = (2.0 * ref_grad * candidate_grad + c) /
                                (ref_grad * ref_grad + candidate_grad * candidate_grad + c);
      similarities.push_back(similarity);
      mean += similarity;
    }
  }
  mean /= static_cast<double>(similarities.size());

  double variance = 0.0;
  for (double similarity : similarities) {
    const double delta = similarity - mean;
    variance += delta * delta;
  }
  variance /= static_cast<double>(similarities.size());
  return std::sqrt(variance);
}

export std::expected<double, std::string> compute_ms_ssim(
    const LumaImage& reference,
    const LumaImage& candidate) {
  if (auto valid = visual_metrics_detail::validate_same_shape(reference, candidate); !valid) {
    return std::unexpected{valid.error()};
  }

  constexpr std::array<double, 5> weights{0.0448, 0.2856, 0.3001, 0.2363, 0.1333};
  LumaImage ref_level = reference;
  LumaImage candidate_level = candidate;
  double value = 1.0;
  for (std::size_t level = 0; level < weights.size(); ++level) {
    const double ssim = visual_metrics_detail::ssim_global(ref_level, candidate_level);
    value *= std::pow(std::clamp(ssim, 1e-9, 1.0), weights[level]);
    if (level + 1 < weights.size()) {
      ref_level = visual_metrics_detail::downsample_2x(ref_level);
      candidate_level = visual_metrics_detail::downsample_2x(candidate_level);
    }
  }
  return std::clamp(value, 0.0, 1.0);
}

export std::expected<VisualScoreBreakdown, std::string> calculate_visual_score(
    const LumaImage& reference,
    const LumaImage& candidate) {
  auto gmsd = compute_gmsd(reference, candidate);
  if (!gmsd) {
    return std::unexpected{gmsd.error()};
  }
  auto ms_ssim = compute_ms_ssim(reference, candidate);
  if (!ms_ssim) {
    return std::unexpected{ms_ssim.error()};
  }
  return calculate_visual_score(*gmsd, *ms_ssim);
}

}  // namespace awj
