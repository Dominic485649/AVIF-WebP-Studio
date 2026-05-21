module;

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module awj.codec;

import awj.config;
import awj.encoding_defaults;
import awj.image;
import awj.resource_planner;
import awj.visual_quality;

export namespace awj {

namespace fs = std::filesystem;

enum class BackendId {
  native,
};

enum class CodecFeature : std::uint32_t {
  none = 0,
  lossless = 1u << 0,
  alpha = 1u << 1,
  thread_control = 1u << 2,
  visual_quality_search = 1u << 3,
};

constexpr CodecFeature operator|(CodecFeature left, CodecFeature right) noexcept {
  return static_cast<CodecFeature>(static_cast<std::uint32_t>(left) |
                                   static_cast<std::uint32_t>(right));
}

constexpr bool has_feature(CodecFeature features, CodecFeature feature) noexcept {
  return (static_cast<std::uint32_t>(features) &
          static_cast<std::uint32_t>(feature)) != 0;
}

struct CodecCapabilities {
  OutputFormat output_format{};
  CodecFeature features{CodecFeature::none};
  int min_quality{1};
  int max_quality{100};
  int min_speed{0};
  int max_speed{10};
  std::vector<int> bit_depths{};
};

struct SpeedMapping {
  int user_speed{};
  int codec_value{};
  std::string codec_key{};
};

struct EncodeDiagnostics {
  std::string decoder_id{};
  std::string encoder_id{};
  std::string requested_encoder_id{};
  std::string requested_chroma{};
  std::string applied_chroma{};
  std::optional<int> requested_bit_depth{};
  std::optional<int> applied_bit_depth{};
  std::string bit_depth_reason{};
  std::string fallback_reason{};
  bool encoder_experimental{};
  std::string encoder_license{};
  SpeedMapping speed_mapping{};
  int encoder_threads{};
  std::uint64_t memory_budget_bytes{};
  bool used_decoder_fallback{};
};

struct NativeEncodeSettings {
  OutputFormat output_format{OutputFormat::avif};
  int quality{default_quality_for(output_format)};
  std::optional<int> visual_quality{};
  int speed{default_speed_for(output_format)};
  std::optional<int> bit_depth{};
  ChromaMode chroma_mode{ChromaMode::auto_keep};
  AvifEncoderMode avif_encoder{AvifEncoderMode::automatic};
  ChromaMode requested_chroma_mode{ChromaMode::auto_keep};
  AvifEncoderMode requested_avif_encoder{AvifEncoderMode::automatic};
  std::optional<int> requested_bit_depth{};
  std::string bit_depth_reason{};
  std::string encoder_fallback_reason{};
  bool strip_metadata{};
  bool visual_quality_fallback{};
  bool avif_tune_iq{encoding_defaults::default_avif_tune_iq};
  ResourcePlan resources{};
};

struct NativeEncodeResult {
  EncodedImage encoded{};
  EncodeDiagnostics diagnostics{};
  int final_quality{};
  bool lossless{};
  int search_attempt_count{};
  std::optional<VisualScoreBreakdown> visual_score{};
  double raw_gmsd{};
  double raw_ms_ssim{};
};

class ImageDecoder {
 public:
  virtual ~ImageDecoder() = default;
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  [[nodiscard]] virtual bool can_decode(const fs::path& path) const = 0;
  virtual std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const = 0;
};

class ImageEncoder {
 public:
  virtual ~ImageEncoder() = default;
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  [[nodiscard]] virtual CodecCapabilities capabilities() const = 0;
  virtual std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings) const = 0;
};

class CodecBackend {
 public:
  virtual ~CodecBackend() = default;
  [[nodiscard]] virtual BackendId backend_id() const noexcept = 0;
  virtual std::expected<NativeEncodeResult, std::string> convert(
      const fs::path& input,
      const NativeEncodeSettings& settings) const = 0;
};

export SpeedMapping map_avif_speed_to_svt_preset(int speed) {
  speed = std::clamp(speed, 0, 10);
  return SpeedMapping{.user_speed = speed,
                      .codec_value = std::clamp(10 - speed, 0, 10),
                      .codec_key = "svt:preset"};
}

export SpeedMapping map_webp_speed_to_method(int speed) {
  speed = std::clamp(speed, 0, 10);
  const int method = std::clamp(6 - (speed * 6 + 5) / 10, 0, 6);
  return SpeedMapping{.user_speed = speed,
                      .codec_value = method,
                      .codec_key = "webp:method"};
}

export SpeedMapping map_jxl_speed_to_effort(int speed) {
  speed = std::clamp(speed, 0, 10);
  const int effort = std::clamp(9 - (speed * 6 + 5) / 10, 3, 9);
  return SpeedMapping{.user_speed = speed,
                      .codec_value = effort,
                      .codec_key = "jxl:effort"};
}

export SpeedMapping map_speed_for_format(OutputFormat format, int speed) {
  switch (format) {
    case OutputFormat::webp:
      return map_webp_speed_to_method(speed);
    case OutputFormat::jxl:
      return map_jxl_speed_to_effort(speed);
    case OutputFormat::avif:
    default:
      return map_avif_speed_to_svt_preset(speed);
  }
}

}  // namespace awj
