module;

#include <cstdint>
#include <string_view>

export module awj.encoding_defaults;

export namespace awj::encoding_defaults {

inline constexpr std::string_view default_input_path_text = "input";
inline constexpr std::wstring_view default_input_path = L"input";
inline constexpr std::string_view default_output_template_text = "{name}";
inline constexpr std::wstring_view default_output_template = L"{name}";
inline constexpr std::string_view default_memory_limit_text = "auto";

inline constexpr int default_avif_quality = 90;
inline constexpr int default_webp_quality = 95;
inline constexpr int default_jxl_quality = 95;
inline constexpr int default_webp_bit_depth = 8;
inline constexpr int default_quality = default_avif_quality;
inline constexpr int default_visual_quality = 90;

inline constexpr int preset_fast_quality = 75;
inline constexpr int preset_balanced_quality = 85;
inline constexpr int preset_best_quality = default_avif_quality;
inline constexpr int preset_extreme_quality = 95;

inline constexpr int preset_fast_timeout_minutes = 10;
inline constexpr int preset_balanced_timeout_minutes = 20;
inline constexpr int preset_best_timeout_minutes = 30;
inline constexpr int preset_extreme_timeout_minutes = 60;

inline constexpr int default_max_resolution = 0;
inline constexpr std::uint64_t default_memory_limit_bytes = 0;

inline constexpr int default_native_speed = 5;
inline constexpr int default_avif_native_speed = 4;
inline constexpr int default_jxl_native_speed = 4;
inline constexpr int default_jxl_effort = 7;
inline constexpr bool default_jxl_effort_is_explicit = false;
inline constexpr int default_aom_cpu_used = 6;
inline constexpr int default_zenrav1e_preset = 6;
inline constexpr int default_svt_preset = 6;

inline constexpr std::uint64_t large_image_threshold_pixels = 20'000'000ull;
inline constexpr std::uint64_t svt_safe_max_pixels = 35'000'000ull;
inline constexpr int default_grid_overlap_pixels = 0;

inline constexpr bool default_avif_tune_iq = true;
inline constexpr bool default_zenrav1e_enable_qm = true;
inline constexpr bool default_zenrav1e_enable_trellis = false;
inline constexpr bool default_zenrav1e_enable_vae_or_vaq = false;

}  // namespace awj::encoding_defaults
