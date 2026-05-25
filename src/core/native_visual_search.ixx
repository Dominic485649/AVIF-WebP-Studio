module;

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

export module awj.native_visual_search;

import awj.codec;
import awj.image;
import awj.visual_metrics;
import awj.visual_quality;

export namespace awj {

namespace native_visual_search_detail {

std::expected<void, std::string> write_bytes(const fs::path& path,
                                             std::span<const std::byte> bytes) {
  std::ofstream output{path, std::ios::binary};
  if (!output) {
    return std::unexpected{std::format("无法写入候选文件: {}", path.string())};
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    return std::unexpected{std::format("写入候选文件失败: {}", path.string())};
  }
  return {};
}

std::vector<int> build_quality_probe_order(QualitySearchRange range) {
  std::vector<int> order;
  auto add = [&](int quality) {
    quality = std::clamp(quality, range.q_min, range.q_max);
    if (!std::ranges::contains(order, quality)) {
      order.push_back(quality);
    }
  };

  add(range.q_min);
  add(range.q_max);
  const int span = range.q_max - range.q_min;
  if (span <= 1) {
    return order;
  }

  for (int i = 1; i <= 4; ++i) {
    add(range.q_min + (span * i) / 5);
  }

  int low = range.q_min;
  int high = range.q_max;
  while (high - low > 1) {
    const int mid = low + (high - low) / 2;
    add(mid);
    high = mid;
  }

  for (int delta = -3; delta <= 3; ++delta) {
    add(high + delta);
  }
  std::ranges::sort(order);
  return order;
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
};

export std::expected<EvaluatedVisualQualityCandidate, std::string> evaluate_visual_quality_candidate(
    const LumaImage& reference_luma,
    const ImageBuffer& reference_image,
    const ImageEncoder& encoder,
    const ImageDecoder& decoder,
    NativeEncodeSettings settings,
    int quality,
    const fs::path& candidate_path) {
  settings.quality = quality;
  settings.visual_quality.reset();
  auto encoded = encoder.encode(reference_image, settings);
  if (!encoded) {
    return std::unexpected{encoded.error()};
  }
  auto decoded = decoder.decode_memory(
      std::span<const std::byte>{encoded->encoded.bytes}, candidate_path.string());
  if (!decoded) {
    if (auto written = native_visual_search_detail::write_bytes(
            candidate_path, std::span<const std::byte>{encoded->encoded.bytes});
        !written) {
      return std::unexpected{written.error()};
    }
    decoded = decoder.decode(candidate_path);
  }
  if (!decoded) {
    return std::unexpected{decoded.error()};
  }

  auto candidate_luma = make_luma_image(decoded->image);
  if (!candidate_luma) {
    return std::unexpected{candidate_luma.error()};
  }
  auto gmsd = compute_gmsd(reference_luma, *candidate_luma);
  if (!gmsd) {
    return std::unexpected{gmsd.error()};
  }
  auto ms_ssim = compute_ms_ssim(reference_luma, *candidate_luma);
  if (!ms_ssim) {
    return std::unexpected{ms_ssim.error()};
  }
  const auto score = calculate_visual_score(*gmsd, *ms_ssim);
  const auto encoded_bytes = encoded->encoded.bytes.size();
  auto encode_result = std::move(*encoded);
  return EvaluatedVisualQualityCandidate{
      .candidate = VisualQualityCandidate{.quality = encode_result.final_quality,
                                          .bytes = encoded_bytes,
                                          .visual_score = score.visual_score,
                                          .raw_gmsd = *gmsd,
                                          .raw_ms_ssim = *ms_ssim,
                                          .gmsd_quality_score = score.gmsd_quality_score,
                                          .msssim_quality_score = score.msssim_quality_score},
      .encode_result = std::move(encode_result)};
}

export std::expected<NativeVisualQualitySearchResult, std::string>
encode_with_native_visual_quality_search(const ImageBuffer& reference_image,
                                         const ImageEncoder& encoder,
                                         const ImageDecoder& decoder,
                                         NativeEncodeSettings settings,
                                         const fs::path& candidate_path,
                                         std::stop_token stop_token = {}) {
  if (!settings.visual_quality) {
    auto encoded = encoder.encode(reference_image, settings);
    if (!encoded) {
      return std::unexpected{encoded.error()};
    }
    return NativeVisualQualitySearchResult{.encode_result = std::move(*encoded),
                                           .target_met = true};
  }

  const int requested = std::clamp(*settings.visual_quality, 1, 100);
  const auto range = get_quality_search_range(requested);
  if (range.lossless) {
    settings.quality = 100;
    auto encoded = encoder.encode(reference_image, settings);
    if (!encoded) {
      return std::unexpected{encoded.error()};
    }
    const auto encoded_bytes = encoded->encoded.bytes.size();
    encoded->final_quality = 100;
    encoded->lossless = true;
    encoded->search_attempt_count = 1;
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

  auto reference_luma = make_luma_image(reference_image);
  if (!reference_luma) {
    return std::unexpected{reference_luma.error()};
  }

  std::vector<EvaluatedVisualQualityCandidate> evaluated_candidates;
  std::vector<VisualQualityCandidate> candidates;
  for (int quality : native_visual_search_detail::build_quality_probe_order(range)) {
    if (stop_token.stop_requested()) {
      return std::unexpected{"visual_quality 搜索已取消。"};
    }
    auto candidate = evaluate_visual_quality_candidate(*reference_luma, reference_image, encoder, decoder,
                                                       settings, quality, candidate_path);
    if (!candidate) {
      return std::unexpected{candidate.error()};
    }
    candidates.push_back(candidate->candidate);
    evaluated_candidates.push_back(std::move(*candidate));
  }

  auto selected = select_smallest_passing_visual_quality_candidate(candidates, requested);
  bool target_met = selected.found;
  if (!selected.found) {
    if (!settings.visual_quality_fallback) {
      return std::unexpected{std::format(
          "未找到达到 visual_quality={} 的候选，请降低 visual_quality 或使用 visual_quality=100 无损。",
          requested)};
    }
    selected = select_closest_visual_quality_candidate(candidates);
  }
  if (!selected.found) {
    return std::unexpected{"visual_quality 搜索没有产生候选。"};
  }

  auto selected_result = std::ranges::find_if(
      evaluated_candidates,
      [&](const EvaluatedVisualQualityCandidate& candidate) {
        return candidate.candidate.quality == selected.candidate.quality &&
               candidate.candidate.bytes == selected.candidate.bytes &&
               candidate.candidate.visual_score == selected.candidate.visual_score;
      });
  if (selected_result == evaluated_candidates.end()) {
    return std::unexpected{"visual_quality 搜索无法匹配已选候选。"};
  }

  auto encoded = std::move(selected_result->encode_result);
  encoded.final_quality = selected.candidate.quality;
  encoded.lossless = false;
  encoded.search_attempt_count = static_cast<int>(candidates.size());
  encoded.raw_gmsd = selected.candidate.raw_gmsd;
  encoded.raw_ms_ssim = selected.candidate.raw_ms_ssim;
  encoded.visual_score = VisualScoreBreakdown{
      .visual_score = selected.candidate.visual_score,
      .gmsd_quality_score = selected.candidate.gmsd_quality_score,
      .msssim_quality_score = selected.candidate.msssim_quality_score};

  return NativeVisualQualitySearchResult{.encode_result = std::move(encoded),
                                         .candidate = selected.candidate,
                                         .target_met = target_met};
}

}  // namespace awj
