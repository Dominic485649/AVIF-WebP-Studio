module;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

export module awj.visual_quality;

export namespace awj {

inline constexpr double GMSD_BEST = 0.0025;
inline constexpr double GMSD_WORST = 0.22;
inline constexpr double GMSD_CURVE_GAMMA = 0.82;

inline constexpr double MSSSIM_BEST = 0.9998;
inline constexpr double MSSSIM_WORST = 0.82;
inline constexpr double MSSSIM_CURVE_GAMMA = 0.70;

inline constexpr double GMSD_WEIGHT = 0.45;
inline constexpr double MSSSIM_WEIGHT = 0.55;

inline constexpr int ENCODER_QUALITY_MIN = 1;
inline constexpr int ENCODER_QUALITY_MAX = 100;
inline constexpr double SEARCH_RANGE_MAX = 34.0;
inline constexpr double SEARCH_RANGE_MIN = 5.0;
inline constexpr double SEARCH_CENTER_BIAS = 5.0;
inline constexpr double SEARCH_CURVE_GAMMA = 1.28;

struct VisualScoreBreakdown {
  double visual_score{};
  double gmsd_quality_score{};
  double msssim_quality_score{};
};

struct QualitySearchRange {
  bool lossless{};
  int q_min{};
  int q_max{};
};

struct VisualQualityCandidate {
  int quality{};
  std::uintmax_t bytes{};
  double visual_score{};
  double raw_gmsd{};
  double raw_ms_ssim{};
  double gmsd_quality_score{};
  double msssim_quality_score{};
};

struct CandidateSelection {
  bool found{};
  VisualQualityCandidate candidate{};
};

namespace visual_quality_detail {

double clamp01(double value) noexcept {
  return std::clamp(value, 0.0, 1.0);
}

double lerp(double from, double to, double t) noexcept {
  return from + (to - from) * t;
}

}  // namespace visual_quality_detail

export bool visual_quality_weights_are_valid() noexcept {
  return std::abs((GMSD_WEIGHT + MSSSIM_WEIGHT) - 1.0) <= 1e-9;
}

export double normalize_gmsd_to_quality_score(double gmsd) noexcept {
  if (!std::isfinite(gmsd) || gmsd <= GMSD_BEST) {
    return 99.0;
  }
  if (gmsd >= GMSD_WORST) {
    return 1.0;
  }

  const double denominator = std::log(GMSD_WORST / GMSD_BEST);
  if (denominator <= 0.0 || !std::isfinite(denominator)) {
    return 1.0;
  }
  double t = std::log(GMSD_WORST / gmsd) / denominator;
  t = std::pow(visual_quality_detail::clamp01(t), GMSD_CURVE_GAMMA);
  return std::clamp(1.0 + 98.0 * t, 1.0, 99.0);
}

export double normalize_msssim_to_quality_score(double ms_ssim) noexcept {
  if (!std::isfinite(ms_ssim) || ms_ssim >= MSSSIM_BEST) {
    return 99.0;
  }
  if (ms_ssim <= MSSSIM_WORST) {
    return 1.0;
  }

  const double error = 1.0 - ms_ssim;
  const double best_error = 1.0 - MSSSIM_BEST;
  const double worst_error = 1.0 - MSSSIM_WORST;
  if (error <= best_error) {
    return 99.0;
  }
  if (error >= worst_error) {
    return 1.0;
  }

  const double denominator = std::log(worst_error / best_error);
  if (denominator <= 0.0 || !std::isfinite(denominator)) {
    return 1.0;
  }
  double t = std::log(worst_error / error) / denominator;
  t = std::pow(visual_quality_detail::clamp01(t), MSSSIM_CURVE_GAMMA);
  return std::clamp(1.0 + 98.0 * t, 1.0, 99.0);
}

export VisualScoreBreakdown calculate_visual_score(double gmsd,
                                                   double ms_ssim) noexcept {
  const double qg = normalize_gmsd_to_quality_score(gmsd);
  const double qm = normalize_msssim_to_quality_score(ms_ssim);
  // visual_score 是项目内置视觉质量估计，用于自动搜索编码参数，并非人类主观 MOS 分数。
  return VisualScoreBreakdown{.visual_score = GMSD_WEIGHT * qg + MSSSIM_WEIGHT * qm,
                              .gmsd_quality_score = qg,
                              .msssim_quality_score = qm};
}

export QualitySearchRange get_quality_search_range(int visual_quality) noexcept {
  if (visual_quality >= 100) {
    return QualitySearchRange{.lossless = true,
                              .q_min = ENCODER_QUALITY_MAX,
                              .q_max = ENCODER_QUALITY_MAX};
  }

  const int v = std::clamp(visual_quality, 1, 99);
  double t = static_cast<double>(v - 1) / 98.0;
  t = std::pow(visual_quality_detail::clamp01(t), SEARCH_CURVE_GAMMA);

  double center = visual_quality_detail::lerp(
      static_cast<double>(ENCODER_QUALITY_MIN),
      static_cast<double>(ENCODER_QUALITY_MAX), t);
  center += SEARCH_CENTER_BIAS * t;
  const double range = visual_quality_detail::lerp(SEARCH_RANGE_MAX,
                                                   SEARCH_RANGE_MIN, t);

  int q_min = static_cast<int>(std::lround(center - range));
  int q_max = static_cast<int>(std::lround(center + range));
  q_min = std::clamp(q_min, ENCODER_QUALITY_MIN, ENCODER_QUALITY_MAX);
  q_max = std::clamp(q_max, q_min, ENCODER_QUALITY_MAX);
  return QualitySearchRange{.lossless = false, .q_min = q_min, .q_max = q_max};
}

export bool visual_quality_candidate_meets_target(
    const VisualQualityCandidate& candidate,
    int requested_visual_quality) noexcept {
  return candidate.visual_score >= static_cast<double>(requested_visual_quality);
}

export CandidateSelection select_smallest_passing_visual_quality_candidate(
    std::span<const VisualQualityCandidate> candidates,
    int requested_visual_quality) noexcept {
  CandidateSelection selection{};
  for (const auto& candidate : candidates) {
    if (!visual_quality_candidate_meets_target(candidate, requested_visual_quality)) {
      continue;
    }
    if (!selection.found || candidate.bytes < selection.candidate.bytes ||
        (candidate.bytes == selection.candidate.bytes &&
         candidate.quality < selection.candidate.quality)) {
      selection = CandidateSelection{.found = true, .candidate = candidate};
    }
  }
  return selection;
}

export CandidateSelection select_closest_visual_quality_candidate(
    std::span<const VisualQualityCandidate> candidates) noexcept {
  CandidateSelection selection{};
  for (const auto& candidate : candidates) {
    if (!selection.found || candidate.visual_score > selection.candidate.visual_score ||
        (candidate.visual_score == selection.candidate.visual_score &&
         candidate.bytes < selection.candidate.bytes)) {
      selection = CandidateSelection{.found = true, .candidate = candidate};
    }
  }
  return selection;
}

}  // namespace awj
