module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

export module awj.native_visual_search;

import awj.codec;
import awj.core;
import awj.encoding_defaults;
import awj.image;
import awj.visual_metrics;
import awj.visual_metrics_gpu;
import awj.visual_quality;

export namespace awj {

namespace native_visual_search_detail {

using Clock = std::chrono::steady_clock;

double elapsed_seconds(Clock::time_point started) {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

EncodeTimingDiagnostics make_visual_quality_timing() {
  EncodeTimingDiagnostics timing{};
  timing.encode_seconds = 0.0;
  timing.visual_quality_search_seconds = 0.0;
  timing.visual_quality_candidate_encode_seconds = 0.0;
  timing.visual_quality_candidate_decode_seconds = 0.0;
  timing.visual_quality_candidate_io_seconds = 0.0;
  timing.visual_quality_luma_seconds = 0.0;
  timing.gmsd_seconds = 0.0;
  timing.ms_ssim_seconds = 0.0;
  timing.visual_quality_metric_seconds = 0.0;
  return timing;
}

void accumulate_visual_quality_timing(EncodeTimingDiagnostics& target,
                                      const EncodeTimingDiagnostics& source) {
  target.visual_quality_candidate_encode_seconds +=
      source.visual_quality_candidate_encode_seconds;
  target.encode_seconds += source.encode_seconds;
  target.visual_quality_candidate_decode_seconds +=
      source.visual_quality_candidate_decode_seconds;
  target.visual_quality_candidate_io_seconds +=
      source.visual_quality_candidate_io_seconds;
  target.visual_quality_luma_seconds += source.visual_quality_luma_seconds;
  target.gmsd_seconds += source.gmsd_seconds;
  target.ms_ssim_seconds += source.ms_ssim_seconds;
  target.visual_quality_metric_seconds += source.visual_quality_metric_seconds;
  target.visual_quality_candidate_count +=
      source.visual_quality_candidate_count;
  target.visual_quality_decode_memory_fallback_count +=
      source.visual_quality_decode_memory_fallback_count;
  target.visual_quality_gpu_fallback_count +=
      source.visual_quality_gpu_fallback_count;
}

int midpoint_quality(int low, int high) noexcept {
  return low + (high - low) / 2;
}

std::expected<std::size_t, std::string> checked_visual_quality_stride(
    std::size_t width, std::size_t channels, std::string_view context) {
  if (channels == 0 || width == 0 ||
      width > std::numeric_limits<std::size_t>::max() / channels) {
    return std::unexpected{std::format("{} 输入宽度无效。", context)};
  }
  return width * channels;
}

std::expected<std::size_t, std::string> checked_visual_quality_image_bytes(
    std::size_t stride, std::size_t height, std::string_view context) {
  if (stride == 0 || height == 0 ||
      height > std::numeric_limits<std::size_t>::max() / stride) {
    return std::unexpected{std::format("{} 输入尺寸过大。", context)};
  }
  const auto byte_count = stride * height;
  if (static_cast<std::uint64_t>(byte_count) >
      encoding_defaults::max_input_file_bytes) {
    return std::unexpected{
        std::format("{} 图像 buffer 超过 20 GiB 运行时上限。", context)};
  }
  return byte_count;
}

std::expected<std::vector<std::byte>, std::string>
make_visual_quality_byte_buffer(std::size_t byte_count,
                                std::string_view context) {
  if (static_cast<std::uint64_t>(byte_count) >
      encoding_defaults::max_input_file_bytes) {
    return std::unexpected{
        std::format("{} 输出缓冲区超过 20 GiB 运行时上限。", context)};
  }
  std::vector<std::byte> buffer;
  try {
    buffer.resize(byte_count);
  } catch (const std::bad_alloc&) {
    return std::unexpected{std::format("{} 输出缓冲区内存不足。", context)};
  } catch (const std::length_error&) {
    return std::unexpected{
        std::format("{} 输出缓冲区尺寸超过运行时限制。", context)};
  }
  return buffer;
}

std::expected<std::vector<std::byte>, std::string> make_jxl_rgb8_input_cache(
    const ImageBuffer& image, const NativeEncodeSettings& settings) {
  if (settings.output_format != OutputFormat::jxl ||
      settings.applied_alpha == "kept") {
    return std::vector<std::byte>{};
  }
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
      image.planes.empty()) {
    return std::vector<std::byte>{};
  }
  if (image.width == 0 ||
      image.width > std::numeric_limits<std::size_t>::max() / 3) {
    return std::unexpected{"JXL visual_quality RGB 输入宽度无效。"};
  }
  const auto rgb_stride =
      checked_visual_quality_stride(image.width, 3, "JXL visual_quality RGB");
  if (!rgb_stride) {
    return std::unexpected{rgb_stride.error()};
  }
  const auto rgb_size = checked_visual_quality_image_bytes(
      *rgb_stride, image.height, "JXL visual_quality RGB");
  if (!rgb_size) {
    return std::unexpected{rgb_size.error()};
  }
  const auto rgba_stride =
      checked_visual_quality_stride(image.width, 4, "JXL visual_quality RGB");
  if (!rgba_stride) {
    return std::unexpected{rgba_stride.error()};
  }
  const auto& plane = image.planes.front();
  const auto rgba_size = checked_visual_quality_image_bytes(
      plane.stride, image.height, "JXL visual_quality RGB");
  if (!rgba_size) {
    return std::unexpected{rgba_size.error()};
  }
  if (plane.stride < *rgba_stride || plane.bytes.size() < *rgba_size) {
    return std::unexpected{
        "JXL visual_quality RGB 输入 RGBA buffer 尺寸无效。"};
  }

  auto rgb =
      make_visual_quality_byte_buffer(*rgb_size, "JXL visual_quality RGB");
  if (!rgb) {
    return std::unexpected{rgb.error()};
  }
  const auto* rgba_data =
      reinterpret_cast<const std::uint8_t*>(plane.bytes.data());
  auto* rgb_data = reinterpret_cast<std::uint8_t*>(rgb->data());
  for (std::size_t y = 0; y < image.height; ++y) {
    const auto* rgba_row = rgba_data + y * plane.stride;
    auto* rgb_row = rgb_data + y * *rgb_stride;
    for (std::size_t x = 0; x < image.width; ++x) {
      rgb_row[x * 3] = rgba_row[x * 4];
      rgb_row[x * 3 + 1] = rgba_row[x * 4 + 1];
      rgb_row[x * 3 + 2] = rgba_row[x * 4 + 2];
    }
  }
  return rgb;
}

std::expected<std::span<const std::byte>, std::string> make_jxl_rgb8_input_view(
    const ImageBuffer& image, const NativeEncodeSettings& settings,
    std::vector<std::byte>& cache) {
  if (settings.output_format != OutputFormat::jxl ||
      settings.applied_alpha == "kept") {
    return std::span<const std::byte>{};
  }
  if (cache.empty()) {
    auto prepared = make_jxl_rgb8_input_cache(image, settings);
    if (!prepared) {
      return std::unexpected{prepared.error()};
    }
    cache = std::move(*prepared);
  }
  return std::span<const std::byte>{cache};
}

}  // namespace native_visual_search_detail

struct NativeVisualQualitySearchResult {
  NativeEncodeResult encode_result{};
  VisualQualityCandidate candidate{};
  bool target_met{};
};

struct EvaluatedVisualQualityCandidate {
  VisualQualityCandidate candidate{};
  NativeEncodeResult encode_result{};
  EncodeTimingDiagnostics timing{};
};

export std::expected<EvaluatedVisualQualityCandidate, std::string>
evaluate_visual_quality_candidate(
    std::optional<LumaImage>& reference_luma,
    const ImageBuffer& reference_image, const ImageEncoder& encoder,
    const ImageDecoder& decoder, NativeEncodeSettings settings, int quality,
    const fs::path& candidate_path,
    AcceleratedVisualMetricSession* metric_session = nullptr,
    std::stop_token stop_token = {}) {
  auto timing = native_visual_search_detail::make_visual_quality_timing();
  timing.visual_quality_candidate_count = 1;
  settings.quality = quality;
  settings.visual_quality.reset();
  if (stop_token.stop_requested()) {
    return std::unexpected{"visual_quality 搜索已取消。"};
  }

  auto started = native_visual_search_detail::Clock::now();
  auto encoded = encoder.encode(reference_image, settings, stop_token);
  timing.visual_quality_candidate_encode_seconds +=
      native_visual_search_detail::elapsed_seconds(started);
  timing.encode_seconds += timing.visual_quality_candidate_encode_seconds;
  if (!encoded) {
    return std::unexpected{encoded.error()};
  }
  if (stop_token.stop_requested()) {
    return std::unexpected{"visual_quality 搜索已取消。"};
  }

  started = native_visual_search_detail::Clock::now();
  const auto candidate_name = path_to_utf8(candidate_path.filename());
  auto decoded = decoder.decode_memory(
      std::span<const std::byte>{encoded->encoded.bytes}, candidate_name);
  timing.visual_quality_candidate_decode_seconds +=
      native_visual_search_detail::elapsed_seconds(started);
  if (!decoded) {
    return std::unexpected{decoded.error()};
  }
  if (stop_token.stop_requested()) {
    return std::unexpected{"visual_quality 搜索已取消。"};
  }

  std::optional<VisualMetricResult> accelerated_metrics;
  bool metric_session_failed = false;
  if (metric_session != nullptr) {
    AcceleratedVisualMetricTiming accelerated_timing{};
    if (auto metrics = metric_session->calculate_candidate_metrics(
            decoded->image, &accelerated_timing)) {
      accelerated_metrics = std::move(*metrics);
      timing.visual_quality_luma_seconds += accelerated_timing.luma_seconds;
      timing.gmsd_seconds += accelerated_timing.gmsd_seconds;
      timing.ms_ssim_seconds += accelerated_timing.ms_ssim_seconds;
      timing.visual_quality_metric_seconds +=
          accelerated_timing.gmsd_seconds + accelerated_timing.ms_ssim_seconds;
    } else {
      metric_session_failed = true;
      ++timing.visual_quality_gpu_fallback_count;
      timing.visual_quality_luma_seconds += accelerated_timing.luma_seconds;
      timing.gmsd_seconds += accelerated_timing.gmsd_seconds;
      timing.ms_ssim_seconds += accelerated_timing.ms_ssim_seconds;
      timing.visual_quality_metric_seconds +=
          accelerated_timing.gmsd_seconds + accelerated_timing.ms_ssim_seconds;
    }
  }

  if (stop_token.stop_requested()) {
    return std::unexpected{"visual_quality 搜索已取消。"};
  }
  if (accelerated_metrics) {
    const auto encoded_bytes = encoded->encoded.bytes.size();
    auto encode_result = std::move(*encoded);
    encode_result.diagnostics.timing = timing;
    return EvaluatedVisualQualityCandidate{
        .candidate =
            VisualQualityCandidate{
                .quality = encode_result.final_quality,
                .bytes = encoded_bytes,
                .visual_score = accelerated_metrics->score.visual_score,
                .raw_gmsd = accelerated_metrics->raw_gmsd,
                .raw_ms_ssim = accelerated_metrics->raw_ms_ssim,
                .gmsd_quality_score =
                    accelerated_metrics->score.gmsd_quality_score,
                .msssim_quality_score =
                    accelerated_metrics->score.msssim_quality_score},
        .encode_result = std::move(encode_result),
        .timing = timing};
  }

  started = native_visual_search_detail::Clock::now();
  bool session_candidate_luma = false;
  auto candidate_luma = [&]() -> std::expected<LumaImage, std::string> {
    if (metric_session != nullptr && !metric_session_failed) {
      if (auto accelerated_luma =
              metric_session->make_candidate_luma(decoded->image)) {
        session_candidate_luma = true;
        return accelerated_luma;
      }
      return make_luma_image(decoded->image);
    }
    if (settings.visual_quality_gpu && !metric_session_failed) {
      return make_luma_image_accelerated(decoded->image);
    }
    return make_luma_image(decoded->image);
  }();
  decoded->image = ImageBuffer{};
  timing.visual_quality_luma_seconds +=
      native_visual_search_detail::elapsed_seconds(started);
  if (!candidate_luma) {
    return std::unexpected{candidate_luma.error()};
  }
  if (stop_token.stop_requested()) {
    return std::unexpected{"visual_quality 搜索已取消。"};
  }

  auto ensure_reference_luma =
      [&]() -> std::expected<const LumaImage*, std::string> {
    if (!reference_luma) {
      if (stop_token.stop_requested()) {
        return std::unexpected{"visual_quality 搜索已取消。"};
      }
      const auto luma_started = native_visual_search_detail::Clock::now();
      auto luma = make_luma_image(reference_image);
      timing.visual_quality_luma_seconds +=
          native_visual_search_detail::elapsed_seconds(luma_started);
      if (!luma) {
        return std::unexpected{luma.error()};
      }
      if (stop_token.stop_requested()) {
        return std::unexpected{"visual_quality 搜索已取消。"};
      }
      reference_luma = std::move(*luma);
    }
    return &*reference_luma;
  };

  auto record_gmsd_metric =
      [&](std::expected<double, std::string> result,
          native_visual_search_detail::Clock::time_point metric_started) {
        const auto metric_seconds =
            native_visual_search_detail::elapsed_seconds(metric_started);
        timing.gmsd_seconds += metric_seconds;
        timing.visual_quality_metric_seconds += metric_seconds;
        return result;
      };

  auto record_ms_ssim_metric =
      [&](std::expected<double, std::string> result,
          native_visual_search_detail::Clock::time_point metric_started) {
        const auto metric_seconds =
            native_visual_search_detail::elapsed_seconds(metric_started);
        timing.ms_ssim_seconds += metric_seconds;
        timing.visual_quality_metric_seconds += metric_seconds;
        return result;
      };

  auto gmsd = [&]() -> std::expected<double, std::string> {
    if (metric_session != nullptr && session_candidate_luma) {
      started = native_visual_search_detail::Clock::now();
      auto accelerated_gmsd =
          record_gmsd_metric(metric_session->compute_gmsd(), started);
      if (accelerated_gmsd) {
        return accelerated_gmsd;
      }
      auto current_reference_luma = ensure_reference_luma();
      if (!current_reference_luma) {
        return std::unexpected{current_reference_luma.error()};
      }
      started = native_visual_search_detail::Clock::now();
      return record_gmsd_metric(
          compute_gmsd(**current_reference_luma, *candidate_luma), started);
    }
    auto current_reference_luma = ensure_reference_luma();
    if (!current_reference_luma) {
      return std::unexpected{current_reference_luma.error()};
    }
    started = native_visual_search_detail::Clock::now();
    if (settings.visual_quality_gpu && !metric_session_failed) {
      return record_gmsd_metric(
          compute_gmsd_accelerated(**current_reference_luma, *candidate_luma),
          started);
    }
    return record_gmsd_metric(
        compute_gmsd(**current_reference_luma, *candidate_luma), started);
  }();
  if (!gmsd) {
    return std::unexpected{gmsd.error()};
  }
  if (stop_token.stop_requested()) {
    return std::unexpected{"visual_quality 搜索已取消。"};
  }

  auto current_reference_luma = ensure_reference_luma();
  if (!current_reference_luma) {
    return std::unexpected{current_reference_luma.error()};
  }
  auto ms_ssim = [&]() -> std::expected<double, std::string> {
    if (metric_session != nullptr && session_candidate_luma) {
      started = native_visual_search_detail::Clock::now();
      auto accelerated_ms_ssim =
          record_ms_ssim_metric(metric_session->compute_ms_ssim(), started);
      if (accelerated_ms_ssim) {
        return accelerated_ms_ssim;
      }
      started = native_visual_search_detail::Clock::now();
      return record_ms_ssim_metric(
          compute_ms_ssim(**current_reference_luma, *candidate_luma), started);
    }
    started = native_visual_search_detail::Clock::now();
    if (settings.visual_quality_gpu && !metric_session_failed) {
      return record_ms_ssim_metric(
          compute_ms_ssim_accelerated(**current_reference_luma,
                                      *candidate_luma),
          started);
    }
    return record_ms_ssim_metric(
        compute_ms_ssim(**current_reference_luma, *candidate_luma), started);
  }();
  if (!ms_ssim) {
    return std::unexpected{ms_ssim.error()};
  }
  if (stop_token.stop_requested()) {
    return std::unexpected{"visual_quality 搜索已取消。"};
  }

  started = native_visual_search_detail::Clock::now();
  const auto metrics = make_visual_metric_result(*gmsd, *ms_ssim);
  timing.visual_quality_metric_seconds +=
      native_visual_search_detail::elapsed_seconds(started);
  const auto encoded_bytes = encoded->encoded.bytes.size();
  auto encode_result = std::move(*encoded);
  encode_result.diagnostics.timing = timing;
  return EvaluatedVisualQualityCandidate{
      .candidate =
          VisualQualityCandidate{
              .quality = encode_result.final_quality,
              .bytes = encoded_bytes,
              .visual_score = metrics.score.visual_score,
              .raw_gmsd = metrics.raw_gmsd,
              .raw_ms_ssim = metrics.raw_ms_ssim,
              .gmsd_quality_score = metrics.score.gmsd_quality_score,
              .msssim_quality_score = metrics.score.msssim_quality_score},
      .encode_result = std::move(encode_result),
      .timing = timing};
}

export std::expected<NativeVisualQualitySearchResult, std::string>
encode_with_native_visual_quality_search(const ImageBuffer& reference_image,
                                         const ImageEncoder& encoder,
                                         const ImageDecoder& decoder,
                                         NativeEncodeSettings settings,
                                         const fs::path& candidate_path,
                                         std::stop_token stop_token = {}) {
  if (!settings.visual_quality) {
    auto encoded = encoder.encode(reference_image, settings, stop_token);
    if (!encoded) {
      return std::unexpected{encoded.error()};
    }
    return NativeVisualQualitySearchResult{.encode_result = std::move(*encoded),
                                           .target_met = true};
  }

  const int requested = std::clamp(*settings.visual_quality, 1, 100);
  const auto range = get_quality_search_range(requested);
  const auto search_started = native_visual_search_detail::Clock::now();
  auto timing = native_visual_search_detail::make_visual_quality_timing();
  if (range.lossless) {
    settings.quality = 100;
    const auto encode_started = native_visual_search_detail::Clock::now();
    auto encoded = encoder.encode(reference_image, settings, stop_token);
    timing.visual_quality_candidate_encode_seconds +=
        native_visual_search_detail::elapsed_seconds(encode_started);
    timing.encode_seconds += timing.visual_quality_candidate_encode_seconds;
    timing.visual_quality_candidate_count = 1;
    timing.visual_quality_search_seconds =
        native_visual_search_detail::elapsed_seconds(search_started);
    if (!encoded) {
      return std::unexpected{encoded.error()};
    }
    const auto encoded_bytes = encoded->encoded.bytes.size();
    encoded->final_quality = 100;
    encoded->lossless = true;
    encoded->search_attempt_count = 1;
    encoded->diagnostics.timing = timing;
    return NativeVisualQualitySearchResult{
        .encode_result = std::move(*encoded),
        .candidate = VisualQualityCandidate{.quality = 100,
                                            .bytes = encoded_bytes,
                                            .visual_score = 99.0,
                                            .raw_gmsd = 0.0,
                                            .raw_ms_ssim = 1.0,
                                            .gmsd_quality_score = 99.0,
                                            .msssim_quality_score = 99.0},
        .target_met = true};
  }

  std::optional<AcceleratedVisualMetricSession> metric_session;
  if (settings.visual_quality_gpu) {
    if (auto session =
            AcceleratedVisualMetricSession::create(reference_image)) {
      metric_session.emplace(std::move(*session));
    } else {
      ++timing.visual_quality_gpu_fallback_count;
    }
  }
  std::optional<LumaImage> reference_luma;

  std::optional<EvaluatedVisualQualityCandidate> closest_candidate_result;
  std::optional<EvaluatedVisualQualityCandidate> smallest_passing_result;
  std::vector<VisualQualityCandidate> candidates;
  std::vector<int> evaluated_qualities;

  auto candidate_matches = [](const VisualQualityCandidate& left,
                              const VisualQualityCandidate& right) noexcept {
    return left.quality == right.quality && left.bytes == right.bytes &&
           left.visual_score == right.visual_score;
  };

  auto retain_candidate_result =
      [&](EvaluatedVisualQualityCandidate&& evaluated) {
        const auto& candidate = evaluated.candidate;
        if (visual_quality_candidate_meets_target(candidate, requested)) {
          if (!smallest_passing_result ||
              candidate.bytes < smallest_passing_result->candidate.bytes ||
              (candidate.bytes == smallest_passing_result->candidate.bytes &&
               candidate.quality <
                   smallest_passing_result->candidate.quality)) {
            smallest_passing_result = std::move(evaluated);
          }
          return;
        }
        if (!closest_candidate_result ||
            candidate.visual_score >
                closest_candidate_result->candidate.visual_score ||
            (candidate.visual_score ==
                 closest_candidate_result->candidate.visual_score &&
             candidate.bytes < closest_candidate_result->candidate.bytes)) {
          closest_candidate_result = std::move(evaluated);
        }
      };

  auto find_evaluated_candidate =
      [&](int quality) -> std::optional<VisualQualityCandidate> {
    quality = std::clamp(quality, range.q_min, range.q_max);
    for (std::size_t index = 0; index < evaluated_qualities.size(); ++index) {
      if (evaluated_qualities[index] == quality) {
        return candidates[index];
      }
    }
    return std::nullopt;
  };

  std::vector<std::byte> jxl_rgb8_input_cache;

  auto evaluate_quality =
      [&](int quality) -> std::expected<VisualQualityCandidate, std::string> {
    quality = std::clamp(quality, range.q_min, range.q_max);
    if (const auto existing = find_evaluated_candidate(quality)) {
      return *existing;
    }
    if (stop_token.stop_requested()) {
      return std::unexpected{"visual_quality 搜索已取消。"};
    }
    auto candidate_settings = settings;
    auto jxl_rgb8_input = native_visual_search_detail::make_jxl_rgb8_input_view(
        reference_image, candidate_settings, jxl_rgb8_input_cache);
    if (!jxl_rgb8_input) {
      return std::unexpected{jxl_rgb8_input.error()};
    }
    candidate_settings.jxl_rgb8_input = *jxl_rgb8_input;
    auto candidate = evaluate_visual_quality_candidate(
        reference_luma, reference_image, encoder, decoder, candidate_settings,
        quality, candidate_path, metric_session ? &*metric_session : nullptr,
        stop_token);
    if (!candidate) {
      return std::unexpected{candidate.error()};
    }
    auto candidate_summary = candidate->candidate;
    try {
      candidates.push_back(candidate_summary);
      evaluated_qualities.push_back(quality);
    } catch (const std::bad_alloc&) {
      if (candidates.size() > evaluated_qualities.size()) {
        candidates.pop_back();
      }
      return std::unexpected{"visual_quality 候选记录内存不足。"};
    } catch (const std::length_error&) {
      if (candidates.size() > evaluated_qualities.size()) {
        candidates.pop_back();
      }
      return std::unexpected{"visual_quality 候选记录数量超过运行时限制。"};
    }
    native_visual_search_detail::accumulate_visual_quality_timing(
        timing, candidate->timing);
    retain_candidate_result(std::move(*candidate));
    return candidate_summary;
  };

  CandidateSelection selected{};
  bool target_met = false;

  auto low_candidate = evaluate_quality(range.q_min);
  if (!low_candidate) {
    return std::unexpected{low_candidate.error()};
  }

  if (visual_quality_candidate_meets_target(*low_candidate, requested)) {
    selected =
        select_smallest_passing_visual_quality_candidate(candidates, requested);
    target_met = selected.found;
  } else {
    auto high_candidate = evaluate_quality(range.q_max);
    if (!high_candidate) {
      return std::unexpected{high_candidate.error()};
    }

    if (!visual_quality_candidate_meets_target(*high_candidate, requested)) {
      if (!settings.visual_quality_fallback) {
        const auto closest =
            select_closest_visual_quality_candidate(candidates);
        if (closest.found) {
          return std::unexpected{std::format(
              "未找到达到 visual_quality={} 的候选，最接近候选为 q{}、VQ "
              "{:.2f}；请降低 visual_quality 或使用 visual_quality=100 无损。",
              requested, closest.candidate.quality,
              closest.candidate.visual_score)};
        }
        return std::unexpected{
            std::format("未找到达到 visual_quality={} 的候选，请降低 "
                        "visual_quality 或使用 visual_quality=100 无损。",
                        requested)};
      }
      selected = select_closest_visual_quality_candidate(candidates);
    } else {
      int low = range.q_min;
      int high = range.q_max;
      while (high - low > 1) {
        const int midpoint =
            native_visual_search_detail::midpoint_quality(low, high);
        auto midpoint_candidate = evaluate_quality(midpoint);
        if (!midpoint_candidate) {
          return std::unexpected{midpoint_candidate.error()};
        }
        if (visual_quality_candidate_meets_target(*midpoint_candidate,
                                                  requested)) {
          high = midpoint;
        } else {
          low = midpoint;
        }
      }
      if (high < range.q_max) {
        auto neighbor = evaluate_quality(high + 1);
        if (!neighbor) {
          return std::unexpected{neighbor.error()};
        }
      }
      selected = select_smallest_passing_visual_quality_candidate(candidates,
                                                                  requested);
      target_met = selected.found;
    }
  }

  if (!selected.found) {
    return std::unexpected{"visual_quality 搜索没有产生候选。"};
  }

  EvaluatedVisualQualityCandidate* selected_result = nullptr;
  if (smallest_passing_result &&
      candidate_matches(smallest_passing_result->candidate,
                        selected.candidate)) {
    selected_result = &*smallest_passing_result;
  } else if (closest_candidate_result &&
             candidate_matches(closest_candidate_result->candidate,
                               selected.candidate)) {
    selected_result = &*closest_candidate_result;
  }
  if (selected_result == nullptr) {
    return std::unexpected{"visual_quality 搜索无法匹配已选候选。"};
  }

  auto encoded = std::move(selected_result->encode_result);
  timing.visual_quality_search_seconds =
      native_visual_search_detail::elapsed_seconds(search_started);
  encoded.final_quality = selected.candidate.quality;
  encoded.lossless = false;
  encoded.search_attempt_count = static_cast<int>(candidates.size());
  encoded.raw_gmsd = selected.candidate.raw_gmsd;
  encoded.raw_ms_ssim = selected.candidate.raw_ms_ssim;
  encoded.visual_score = VisualScoreBreakdown{
      .visual_score = selected.candidate.visual_score,
      .gmsd_quality_score = selected.candidate.gmsd_quality_score,
      .msssim_quality_score = selected.candidate.msssim_quality_score};
  encoded.diagnostics.timing = timing;

  return NativeVisualQualitySearchResult{.encode_result = std::move(encoded),
                                         .candidate = selected.candidate,
                                         .target_met = target_met};
}

}  // namespace awj
