module;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module awj.visual_quality;

import awj.encoding_defaults;

export namespace awj {

inline constexpr double GMSD_BEST = encoding_defaults::visual_gmsd_best;
inline constexpr double GMSD_WORST = encoding_defaults::visual_gmsd_worst;
inline constexpr double GMSD_CURVE_GAMMA =
    encoding_defaults::visual_gmsd_curve_gamma;

inline constexpr double MSSSIM_BEST = encoding_defaults::visual_msssim_best;
inline constexpr double MSSSIM_WORST = encoding_defaults::visual_msssim_worst;
inline constexpr double MSSSIM_CURVE_GAMMA =
    encoding_defaults::visual_msssim_curve_gamma;

inline constexpr double GMSD_WEIGHT = encoding_defaults::visual_gmsd_weight;
inline constexpr double MSSSIM_WEIGHT =
    encoding_defaults::visual_msssim_weight;

inline constexpr int ENCODER_QUALITY_MIN =
    encoding_defaults::visual_encoder_quality_min;
inline constexpr int ENCODER_QUALITY_MAX =
    encoding_defaults::visual_encoder_quality_max;
inline constexpr double SEARCH_RANGE_MAX =
    encoding_defaults::visual_search_range_max;
inline constexpr double SEARCH_RANGE_MIN =
    encoding_defaults::visual_search_range_min;
inline constexpr double SEARCH_CENTER_BIAS =
    encoding_defaults::visual_search_center_bias;
inline constexpr double SEARCH_CURVE_GAMMA =
    encoding_defaults::visual_search_curve_gamma;

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

struct VisualQualitySearchProbe {
  int quality{};
  std::uintmax_t bytes{};
  double visual_score{};
  bool target_met{};
  std::string decision{};
};

struct VisualQualitySearchTrace {
  int requested_visual_quality{};
  int q_min{};
  int q_max{};
  int predicted_quality{};
  bool fallback_used{};
  std::string selection_reason{};
  std::vector<VisualQualitySearchProbe> probes{};
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
  double penalty = std::log(error / best_error) / denominator;
  penalty = std::pow(visual_quality_detail::clamp01(penalty), MSSSIM_CURVE_GAMMA);
  return std::clamp(99.0 - 98.0 * penalty, 1.0, 99.0);
}

export VisualScoreBreakdown calculate_visual_score(double gmsd,
                                                   double ms_ssim) noexcept {
  const double qg = normalize_gmsd_to_quality_score(gmsd);
  const double qm = normalize_msssim_to_quality_score(ms_ssim);
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
