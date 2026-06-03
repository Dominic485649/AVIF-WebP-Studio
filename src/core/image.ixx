module;

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module awj.image;

export namespace awj {

namespace fs = std::filesystem;

enum class PixelFormat {
  unknown,
  gray,
  rgb,
  rgba,
  yuv444,
  yuv422,
  yuv420,
};

enum class AlphaMode {
  none,
  straight,
  premultiplied,
};

enum class MetadataKind {
  icc,
  exif,
  xmp,
};

struct MetadataBlock {
  MetadataKind kind{};
  std::vector<std::byte> bytes{};
};

struct ImagePlane {
  std::vector<std::byte> bytes{};
  std::size_t stride{};
};

struct HdrContentLightMetadata {
  std::uint16_t max_cll{};
  std::uint16_t max_pall{};
};

struct ImageSourceInfo {
  PixelFormat pixel_format{PixelFormat::unknown};
  int bit_depth{};
  std::optional<int> color_primaries{};
  std::optional<int> transfer_characteristics{};
  std::optional<int> matrix_coefficients{};
  std::optional<int> color_range{};
  std::optional<HdrContentLightMetadata> content_light{};
  bool has_hdr_metadata{};
  std::string color_metadata_source{};
};

struct ImageBuffer {
  std::size_t width{};
  std::size_t height{};
  PixelFormat pixel_format{PixelFormat::unknown};
  AlphaMode alpha_mode{AlphaMode::none};
  int bit_depth{8};
  std::optional<ImageSourceInfo> source_info{};
  std::vector<ImagePlane> planes{};
  std::vector<MetadataBlock> metadata{};

  [[nodiscard]] bool empty() const noexcept {
    return width == 0 || height == 0 || planes.empty();
  }
};

struct EncodedImage {
  std::vector<std::byte> bytes{};
  std::string codec_name{};
};

struct ImageDecodeResult {
  ImageBuffer image{};
  std::string decoder_id{};
  bool used_fallback{};
};

struct ImageWriteTarget {
  fs::path path{};
  bool atomic_replace{true};
};

}  // namespace awj
