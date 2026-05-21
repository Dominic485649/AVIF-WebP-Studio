#include <cmath>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

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

}  // namespace

int main() {
  if (!awj::visual_quality_weights_are_valid()) {
    return fail("视觉质量权重无效。");
  }
  if (!nearly_equal(awj::normalize_gmsd_to_quality_score(awj::GMSD_BEST), 99.0)) {
    return fail("GMSD best 未映射到 99。");
  }
  if (!nearly_equal(awj::normalize_gmsd_to_quality_score(awj::GMSD_WORST), 1.0)) {
    return fail("GMSD worst 未映射到 1。");
  }
  if (!nearly_equal(awj::normalize_msssim_to_quality_score(awj::MSSSIM_BEST), 99.0)) {
    return fail("MS-SSIM best 未映射到 99。");
  }
  if (!nearly_equal(awj::normalize_msssim_to_quality_score(awj::MSSSIM_WORST), 1.0)) {
    return fail("MS-SSIM worst 未映射到 1。");
  }

  const auto score = awj::calculate_visual_score(0.01, 0.98);
  const double expected = awj::GMSD_WEIGHT * score.gmsd_quality_score +
                          awj::MSSSIM_WEIGHT * score.msssim_quality_score;
  if (!nearly_equal(score.visual_score, expected)) {
    return fail("visual_score 加权公式错误。");
  }

  const awj::VisualQualityCandidate fail_candidate{.quality = 80,
                                                    .bytes = 1000,
                                                    .visual_score = 89.99};
  const awj::VisualQualityCandidate pass_large{.quality = 91,
                                                .bytes = 2000,
                                                .visual_score = 93.0};
  const awj::VisualQualityCandidate pass_small{.quality = 92,
                                                .bytes = 1200,
                                                .visual_score = 90.0};
  if (awj::visual_quality_candidate_meets_target(fail_candidate, 90)) {
    return fail("未达标候选被错误接受。");
  }
  if (!awj::visual_quality_candidate_meets_target(pass_small, 90)) {
    return fail("达标候选被错误拒绝。");
  }

  std::vector candidates{fail_candidate, pass_large, pass_small};
  const auto selected = awj::select_smallest_passing_visual_quality_candidate(
      std::span<const awj::VisualQualityCandidate>{candidates}, 90);
  if (!selected.found || selected.candidate.bytes != pass_small.bytes) {
    return fail("多个达标候选未选择最小体积。");
  }

  const auto low = awj::get_quality_search_range(20);
  const auto high = awj::get_quality_search_range(95);
  if (low.lossless || high.lossless || high.q_min <= low.q_min ||
      (high.q_max - high.q_min) >= (low.q_max - low.q_min)) {
    return fail("视觉质量搜索区间映射不符合高质量更高且更窄的要求。");
  }
  const auto lossless = awj::get_quality_search_range(100);
  if (!lossless.lossless || lossless.q_min != awj::ENCODER_QUALITY_MAX ||
      lossless.q_max != awj::ENCODER_QUALITY_MAX) {
    return fail("visual_quality=100 未映射到无损。");
  }

  return 0;
}
