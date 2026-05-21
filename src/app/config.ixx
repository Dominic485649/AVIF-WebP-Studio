module;

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <scn/scan.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module awj.config;

import awj.encoding_defaults;
import awj.visual_quality;

export namespace awj {

enum class Preset { fast, balanced, best, extreme };
enum class OutputFormat { avif, webp, jxl };
enum class CollisionMode { overwrite, skip, suffix_time, suffix_random };
enum class ChromaMode { auto_keep, yuv444, yuv422, yuv420 };
enum class AvifEncoderMode { automatic, svt, aom, zenrav1e };

int default_max_jobs() noexcept {
  // 自动并发不是“吃满 CPU”，而是给桌面、UI 线程和编码器内部线程预留余量。
  const auto hardware = std::thread::hardware_concurrency();
  if (hardware <= 1) {
    return 1;
  }
  if (hardware >= 12) {
    return static_cast<int>(hardware - 4);
  }
  if (hardware > 4) {
    return static_cast<int>(hardware - 2);
  }
  return static_cast<int>(hardware - 1);
}

constexpr int default_quality_for(OutputFormat format) noexcept {
  switch (format) {
    case OutputFormat::webp:
      return encoding_defaults::default_webp_quality;
    case OutputFormat::jxl:
      return encoding_defaults::default_jxl_quality;
    case OutputFormat::avif:
    default:
      return encoding_defaults::default_avif_quality;
  }
}

constexpr int default_speed_for(OutputFormat format) noexcept {
  switch (format) {
    case OutputFormat::jxl:
      return encoding_defaults::default_jxl_native_speed;
    case OutputFormat::avif:
      return encoding_defaults::default_avif_native_speed;
    case OutputFormat::webp:
    default:
      return encoding_defaults::default_native_speed;
  }
}

// 命令行只负责生成这个配置对象；后面的流水线不会再回头解析 argv。
struct AppConfig {
  std::filesystem::path input_path{std::wstring{encoding_defaults::default_input_path}};
  std::filesystem::path output_dir{};
  std::wstring output_template{encoding_defaults::default_output_template};
  Preset preset{Preset::best};
  OutputFormat output_format{OutputFormat::avif};
  CollisionMode collision_mode{CollisionMode::overwrite};
  ChromaMode chroma_mode{ChromaMode::auto_keep};
  AvifEncoderMode avif_encoder{AvifEncoderMode::automatic};
  bool enable_experimental_encoders{false};
  int quality{default_quality_for(output_format)};
  std::optional<int> visual_quality{};
  std::optional<int> bit_depth{};
  std::optional<int> speed{};
  int max_jobs{default_max_jobs()};
  std::uint64_t memory_limit_bytes{encoding_defaults::default_memory_limit_bytes};
  int max_resolution{encoding_defaults::default_max_resolution};
  int encode_timeout_minutes{encoding_defaults::preset_best_timeout_minutes};
  bool visual_quality_fallback{false};
  bool strip_metadata{false};
  bool write_summary{false};
  bool write_log{false};
};

AppConfig default_app_config() {
  return AppConfig{};
}

void apply_format_defaults(AppConfig& cfg, OutputFormat format) noexcept {
  cfg.output_format = format;
  cfg.quality = default_quality_for(format);
}


struct ParseResult {
  bool should_exit{false};
  int exit_code{0};
  AppConfig config{};
};

namespace config_detail {

std::wstring lower_copy(std::wstring_view text) {
  std::wstring out{text};
  std::ranges::transform(out, out.begin(),
                         [](wchar_t ch) { return std::towlower(ch); });
  return out;
}

std::string narrow_ascii(std::wstring_view text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t ch : text) {
    out.push_back(ch <= 0x7f ? static_cast<char>(ch) : '?');
  }
  return out;
}

template <class Number>
std::optional<Number> scan_number(std::wstring_view text) {
  auto normalized = lower_copy(text);
  if (!normalized.empty() && normalized.front() == L'q') {
    normalized.erase(normalized.begin());
  }

  const auto narrow = narrow_ascii(normalized);
  const auto parsed = scn::scan_value<Number>(std::string_view{narrow});
  if (!parsed) {
    return std::nullopt;
  }
  return parsed->value();
}

std::expected<int, std::string> parse_quality(std::wstring_view text) {
  // 同时接受 90、q90 和 0.9；内部统一归一化到 native 编码质量 1..100。
  const auto value = scan_number<double>(text);
  if (!value) {
    return std::unexpected{"质量参数必须是数字，例如 90 或 q90。"};
  }

  if (!std::isfinite(*value)) {
    return std::unexpected{"质量参数必须是有限数字。"};
  }

  const double normalized =
      (*value > 0.0 && *value <= 1.0) ? (*value * 100.0) : *value;
  const int quality = static_cast<int>(std::lround(normalized));
  if (quality < 1 || quality > 100) {
    return std::unexpected{"质量范围必须在 1 到 100 之间。"};
  }
  return quality;
}

std::expected<int, std::string> parse_int_range(std::wstring_view text,
                                                int min_value,
                                                int max_value,
                                                std::string_view name) {
  const auto value = scan_number<int>(text);
  if (!value) {
    return std::unexpected{std::format("{} 必须是整数。", name)};
  }
  if (*value < min_value || *value > max_value) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", name, min_value, max_value)};
  }
  return *value;
}

std::expected<int, std::string> parse_auto_jobs(std::wstring_view text) {
  // CLI 和 UI 都允许“自动/auto/jthread”，避免用户必须手算预留线程数。
  const auto lower = lower_copy(text);
  if (lower == L"auto" || lower == L"jthread" || lower == L"自动") {
    return default_max_jobs();
  }
  return parse_int_range(text, 1, 128, "并发数量");
}

std::expected<std::uint64_t, std::string> parse_memory_limit(std::wstring_view text) {
  const auto lower = lower_copy(text);
  if (lower == L"auto" || lower == L"自动") {
    return 0ull;
  }
  std::wstring number_part{lower};
  std::uint64_t multiplier = 1;
  const auto consume_suffix = [&](std::wstring_view suffix,
                                  std::uint64_t value) {
    if (number_part.ends_with(suffix)) {
      number_part.resize(number_part.size() - suffix.size());
      multiplier = value;
      return true;
    }
    return false;
  };
  consume_suffix(L"gib", 1024ull * 1024ull * 1024ull) ||
      consume_suffix(L"gb", 1000ull * 1000ull * 1000ull) ||
      consume_suffix(L"mib", 1024ull * 1024ull) ||
      consume_suffix(L"mb", 1000ull * 1000ull) ||
      consume_suffix(L"kib", 1024ull) ||
      consume_suffix(L"kb", 1000ull) || consume_suffix(L"b", 1ull);
  const auto value = scan_number<double>(number_part);
  if (!value || !std::isfinite(*value) || *value < 0.0) {
    return std::unexpected{"memory-limit 必须是 auto 或非负数字，可带 KiB/MiB/GiB 后缀。"};
  }
  return static_cast<std::uint64_t>(*value * static_cast<double>(multiplier));
}

std::expected<double, std::string> parse_double_range(std::wstring_view text,
                                                      double min_value,
                                                      double max_value,
                                                      std::string_view name) {
  const auto value = scan_number<double>(text);
  if (!value) {
    return std::unexpected{std::format("{} 必须是数字。", name)};
  }
  if (!std::isfinite(*value)) {
    return std::unexpected{std::format("{} 必须是有限数字。", name)};
  }
  if (*value < min_value || *value > max_value) {
    return std::unexpected{
        std::format("{} 范围必须在 {:.3f} 到 {:.3f} 之间。",
                    name, min_value, max_value)};
  }
  return *value;
}

std::expected<void, std::string> validate_native_option(std::wstring_view value) {
  constexpr std::size_t max_option_length = 512;
  if (value.empty()) {
    return std::unexpected{"--option 不能为空。"};
  }
  if (value.size() > max_option_length) {
    return std::unexpected{"--option 长度不能超过 512 个字符。"};
  }
  const auto pos = value.find(L'=');
  const auto key = pos == std::wstring_view::npos ? value : value.substr(0, pos);
  if (key.empty()) {
    return std::unexpected{"--option 的 key 不能为空"};
  }
  for (const wchar_t ch : value) {
    if (ch < 0x20 || ch == 0x7f) {
      return std::unexpected{"--option 不能包含控制字符。"};
    }
  }
  return {};
}

std::optional<Preset> parse_preset(std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"fast") {
    return Preset::fast;
  }
  if (lower == L"balanced") {
    return Preset::balanced;
  }
  if (lower == L"best") {
    return Preset::best;
  }
  if (lower == L"extreme") {
    return Preset::extreme;
  }
  return std::nullopt;
}

std::optional<OutputFormat> parse_output_format(std::wstring_view value) {
  auto lower = lower_copy(value);
  if (!lower.empty() && lower.front() == L'.') {
    lower.erase(lower.begin());
  }
  if (lower == L"avif" || lower == L"heic") {
    return OutputFormat::avif;
  }
  if (lower == L"webp") {
    return OutputFormat::webp;
  }
  if (lower == L"jxl") {
    return OutputFormat::jxl;
  }
  return std::nullopt;
}

std::optional<CollisionMode> parse_collision(std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"overwrite" || lower == L"replace" || lower == L"覆盖") {
    return CollisionMode::overwrite;
  }
  if (lower == L"skip" || lower == L"skip-existing" || lower == L"跳过") {
    return CollisionMode::skip;
  }
  if (lower == L"time" || lower == L"suffix-time" || lower == L"时间") {
    return CollisionMode::suffix_time;
  }
  if (lower == L"random" || lower == L"suffix-random" || lower == L"随机") {
    return CollisionMode::suffix_random;
  }
  return std::nullopt;
}

std::optional<ChromaMode> parse_chroma(std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"auto" || lower == L"keep" || lower == L"保持" ||
      lower == L"自动") {
    return ChromaMode::auto_keep;
  }
  if (lower == L"444" || lower == L"4:4:4") {
    return ChromaMode::yuv444;
  }
  if (lower == L"422" || lower == L"4:2:2") {
    return ChromaMode::yuv422;
  }
  if (lower == L"420" || lower == L"4:2:0") {
    return ChromaMode::yuv420;
  }
  return std::nullopt;
}

std::string chroma_name(ChromaMode mode) {
  switch (mode) {
    case ChromaMode::yuv444:
      return "444";
    case ChromaMode::yuv422:
      return "422";
    case ChromaMode::yuv420:
      return "420";
    case ChromaMode::auto_keep:
    default:
      return "auto";
  }
}

std::optional<AvifEncoderMode> parse_avif_encoder(std::wstring_view value) {
  const auto lower = lower_copy(value);
  if (lower == L"auto" || lower == L"automatic" || lower == L"自动") {
    return AvifEncoderMode::automatic;
  }
  if (lower == L"svt" || lower == L"svt-av1" || lower == L"svt-av1-hdr") {
    return AvifEncoderMode::svt;
  }
  if (lower == L"aom" || lower == L"libaom") {
    return AvifEncoderMode::aom;
  }
  if (lower == L"zenrav1e") {
    return AvifEncoderMode::zenrav1e;
  }
  return std::nullopt;
}

std::string avif_encoder_name(AvifEncoderMode mode) {
  switch (mode) {
    case AvifEncoderMode::svt:
      return "svt";
    case AvifEncoderMode::aom:
      return "aom";
    case AvifEncoderMode::zenrav1e:
      return "zenrav1e";
    case AvifEncoderMode::automatic:
    default:
      return "auto";
  }
}

bool avif_encoder_is_experimental(AvifEncoderMode mode) noexcept {
  return mode == AvifEncoderMode::zenrav1e;
}

bool avif_encoder_is_svt_compatible_chroma(ChromaMode mode) noexcept {
  return mode == ChromaMode::auto_keep || mode == ChromaMode::yuv420;
}

bool avif_bit_depth_supported(int bit_depth) noexcept {
  return bit_depth == 8 || bit_depth == 10 || bit_depth == 12;
}

void apply_preset(AppConfig& cfg, Preset preset) {
  cfg.preset = preset;
  switch (preset) {
    case Preset::fast:
      cfg.quality = encoding_defaults::preset_fast_quality;
      cfg.encode_timeout_minutes = encoding_defaults::preset_fast_timeout_minutes;
      break;
    case Preset::balanced:
      cfg.quality = encoding_defaults::preset_balanced_quality;
      cfg.encode_timeout_minutes = encoding_defaults::preset_balanced_timeout_minutes;
      break;
    case Preset::best:
      cfg.quality = encoding_defaults::preset_best_quality;
      cfg.encode_timeout_minutes = encoding_defaults::preset_best_timeout_minutes;
      break;
    case Preset::extreme:
      cfg.quality = encoding_defaults::preset_extreme_quality;
      cfg.encode_timeout_minutes = encoding_defaults::preset_extreme_timeout_minutes;
      break;
  }
}

}  // namespace config_detail

std::string chroma_mode_name(ChromaMode mode) {
  return config_detail::chroma_name(mode);
}

std::string avif_encoder_mode_name(AvifEncoderMode mode) {
  return config_detail::avif_encoder_name(mode);
}

std::expected<void, std::string> validate_native_option(std::wstring_view value) {
  return config_detail::validate_native_option(value);
}

std::expected<std::uint64_t, std::string> parse_memory_limit(
    std::wstring_view text) {
  return config_detail::parse_memory_limit(text);
}

std::expected<void, std::string> validate_config(const AppConfig& cfg) {
  // 路径存在性在 scan_images 中校验；这里专注于格式自身不能违反的编码约束。
  switch (cfg.output_format) {
    case OutputFormat::avif:
      if (cfg.bit_depth &&
          !config_detail::avif_bit_depth_supported(*cfg.bit_depth)) {
        return std::unexpected{
            "当前 native AVIF 输出仅支持 8、10、12-bit 位深；留空表示自动选择。"};
      }
      break;
    case OutputFormat::webp:
      if (cfg.bit_depth && *cfg.bit_depth != 8) {
        return std::unexpected{
            "WebP bitstream 只支持 8-bit；请把位深设为 8，或留空保持原片。"};
      }
      if (cfg.chroma_mode != ChromaMode::auto_keep) {
        return std::unexpected{
            "WebP 不支持手动选择 444/422/420；有损 WebP 为 8-bit 4:2:0，"
            "无损 WebP 为 8-bit ARGB。"};
      }
      break;
    case OutputFormat::jxl:
      if (cfg.chroma_mode != ChromaMode::auto_keep) {
        return std::unexpected{
            "JXL 不支持手动选择 444/422/420；请将 chroma 设为 auto。"};
      }
      break;
  }
  if (cfg.output_format == OutputFormat::avif &&
      cfg.avif_encoder == AvifEncoderMode::svt &&
      !config_detail::avif_encoder_is_svt_compatible_chroma(cfg.chroma_mode)) {
    return std::unexpected{
        "SVT AVIF encoder only supports 420 chroma; use --chroma 420/auto or --avif-encoder aom."};
  }
  if (!visual_quality_weights_are_valid()) {
    return std::unexpected{"视觉质量权重配置无效：GMSD_WEIGHT + MSSSIM_WEIGHT 必须等于 1。"};
  }
  if (cfg.visual_quality && (*cfg.visual_quality < 1 || *cfg.visual_quality > 100)) {
    return std::unexpected{"visual-quality 范围必须在 1 到 100 之间。"};
  }
  if (cfg.visual_quality_fallback && !cfg.visual_quality) {
    return std::unexpected{"--visual-quality-fallback 只能与 --visual-quality 一起使用。"};
  }
  if (cfg.memory_limit_bytes > 0 && cfg.memory_limit_bytes < 64ull * 1024ull * 1024ull) {
    return std::unexpected{"memory-limit 不能低于 64MiB；使用 auto 可由程序自动估算。"};
  }
  return {};
}

std::expected<void, std::string> finalize_config_defaults(AppConfig& cfg,
                                                        bool quality_was_set,
                                                        bool preset_was_set) {
  if (!quality_was_set && !preset_was_set) {
    cfg.quality = default_quality_for(cfg.output_format);
  }
  if (cfg.output_template.empty()) {
    cfg.output_template = encoding_defaults::default_output_template;
  }
  return validate_config(cfg);
}

std::string help_text() {
  std::string help = R"(AWJimage C++23
=======================

默认后端：内置 native（libavif/zenrav1e/WebP/JXL）
默认质量：AVIF q@AVIF_QUALITY@，WebP q@WEBP_QUALITY@，JXL q@JXL_QUALITY@
质量范围：q1..q100，q100 为无损

用法:
  AWJ-cli.exe [选项]

常用选项:
  -i, --input <路径>          输入文件或目录，默认 @INPUT_PATH@
  -o, --output <目录>         输出目录；默认与输入同目录
  -f, --format <avif|webp|jxl> 输出格式，默认 avif
  -q, --quality <1-100>       编码质量，AVIF 默认 @AVIF_QUALITY@，WebP/JXL 默认 @WEBP_QUALITY@；100 为无损。也接受 q90 或 0.9
  --visual-quality <1-100>   视觉质量目标；存在时覆盖 --quality，100 直接无损，1..99 自动搜索最小体积达标候选
  --visual-quality-fallback  visual_quality 搜索未达标时输出最接近目标的候选
  -d, --bit-depth <位深>      AVIF 支持 8/10/12；JXL 不填保持原片，WebP 固定 @WEBP_BIT_DEPTH@
  --chroma <auto|444|422|420> AVIF 色度采样；SVT 仅支持 420，JXL/WebP 不支持手动采样；也可用 --444 / --422 / --420
  --avif-encoder <auto|svt|aom|zenrav1e> AVIF 编码器选择，默认 auto；auto 只选择当前构建真实可用的稳定编码器
  --experimental-encoders   启用实验 AVIF 编码器；开启后可显式使用 zenrav1e
  -p, --preset <名称>         fast / balanced / best / extreme，默认 best
  -t, --threads <数量>        并发数量，默认 CPU 线程数
  --memory-limit <auto|大小>  内存限制；auto 为总内存一半与可用内存 80% 的较小值，可用 4GiB/4096MiB
  -m, --template <模板>       输出命名，默认 @OUTPUT_TEMPLATE@
  --max-resolution <像素>     限制最长边；0 表示不缩放，默认 @MAX_RESOLUTION@
  --speed <0-10>             统一速度参数；WebP method / JXL effort / AVIF encoder speed-preset
  --option <key=value>       预留 native 高级选项，可重复；当前版本只记录与校验，不调用外部 Magick/ffmpeg
  --collision <策略>          overwrite / skip / time / random，默认 overwrite
  --timeout-encode <分钟>     单张图片编码超时，默认 @ENCODE_TIMEOUT@
  --strip                    去除 EXIF/ICC 等元数据，通常更小且更隐私
  --summary                  生成 summary.csv
  --log                      生成 log\awj-cli.log
  --skip-existing            已有输出时跳过
  --overwrite                已有输出时覆盖，默认行为
  --suffix-time              输出名追加时间后缀
  --suffix-random            输出名追加随机后缀
  --help                     显示帮助

模板变量:
  {index}  图片序号，从 1 开始
  {name}   原文件名，不含扩展名
  {ext}    原扩展名，不含点
  {date}   扫描日期，例如 20260514
  {time}   扫描时间，例如 193005
  {datetime} 日期时间，例如 20260514-193005
  {unix}   Unix 时间戳
  {rand}   每张图一个随机 8 位十六进制
  {hash}   文件内容 FNV-1a 64 位哈希
  {hash8}  文件内容哈希前 8 位
  {params} 编码参数，例如 q95t5 或 q95t5_444_10

示例:
  AWJ-cli.exe -i "D:\图片" -o Avifoutput -q q90
  AWJ-cli.exe -i input --format webp --template "{name}-{date}"
  AWJ-cli.exe -i input.png -o output.jxl --format jxl -q 90
  AWJ-cli.exe -i input --visual-quality 92 --summary
  AWJ-cli.exe -i input --chroma 444 --bit-depth 10
)";
  const auto replace_all = [](std::string& text,
                              std::string_view from,
                              std::string_view to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
      text.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replace_all(help, "@AVIF_QUALITY@", std::format("{}", default_quality_for(OutputFormat::avif)));
  replace_all(help, "@WEBP_QUALITY@", std::format("{}", default_quality_for(OutputFormat::webp)));
  replace_all(help, "@JXL_QUALITY@", std::format("{}", default_quality_for(OutputFormat::jxl)));
  replace_all(help, "@INPUT_PATH@", encoding_defaults::default_input_path_text);
  replace_all(help, "@WEBP_BIT_DEPTH@", std::format("{}", encoding_defaults::default_webp_bit_depth));
  replace_all(help, "@OUTPUT_TEMPLATE@", encoding_defaults::default_output_template_text);
  replace_all(help, "@MAX_RESOLUTION@", std::format("{}", encoding_defaults::default_max_resolution));
  replace_all(help, "@ENCODE_TIMEOUT@", std::format("{}", encoding_defaults::preset_best_timeout_minutes));
  return help;
}

void print_help() {
  const auto help = help_text();
  std::fputs(help.c_str(), stdout);
  std::fputc('\n', stdout);
}

std::expected<ParseResult, std::string> parse_arguments(
    const std::vector<std::wstring>& args) {
  // CLI 参数直接落到 AppConfig，确保命令行和 Slint UI 走同一套核心逻辑。
  AppConfig cfg = default_app_config();
  bool preset_was_set = false;
  bool quality_was_set = false;

  auto require_value = [&](std::size_t& index,
                           std::wstring_view option)
      -> std::expected<std::wstring, std::string> {
    if (index + 1 >= args.size()) {
      return std::unexpected{std::format(
          "{} 需要一个参数。请在该选项后补充具体值，例如路径、数字或枚举值。",
          config_detail::narrow_ascii(option))};
    }
    ++index;
    return args[index];
  };

  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto lower = config_detail::lower_copy(args[i]);

    if (lower == L"-h" || lower == L"--help") {
      print_help();
      return ParseResult{.should_exit = true, .exit_code = 0, .config = cfg};
    }

    if (lower == L"-i" || lower == L"--input") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.input_path = *value;
      continue;
    }

    if (lower == L"-o" || lower == L"--output") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.output_dir = *value;
      continue;
    }

    if (lower == L"-f" || lower == L"--format" || lower == L"--output-format") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto format = config_detail::parse_output_format(*value);
      if (!format) {
        return std::unexpected{std::format(
            "输出格式不支持: {}。可选值：avif、webp、jxl。",
            config_detail::narrow_ascii(*value))};
      }
      cfg.output_format = *format;
      continue;
    }

    if (lower == L"--magick") {
      return std::unexpected{"内置 Magick 后端已移除；当前版本只使用 native 后端，外部 Magick/ffmpeg 集成暂未启用。"};
    }

    if (lower == L"--backend") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto backend = config_detail::lower_copy(*value);
      if (backend == L"native") {
        continue;
      }
      return std::unexpected{"内置 Magick 后端已移除；当前版本只支持 native 后端。"};
    }

    if (lower == L"-m" || lower == L"--template" ||
        lower == L"--output-template") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.output_template = *value;
      continue;
    }

    if (lower == L"-p" || lower == L"--preset") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto preset = config_detail::parse_preset(*value);
      if (!preset) {
        return std::unexpected{std::format(
            "预设不支持: {}。可选值：fast、balanced、best、extreme。",
            config_detail::narrow_ascii(*value))};
      }
      config_detail::apply_preset(cfg, *preset);
      preset_was_set = true;
      continue;
    }

    if (lower == L"-q" || lower == L"--quality") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto quality = config_detail::parse_quality(*value);
      if (!quality) {
        return std::unexpected{quality.error()};
      }
      cfg.quality = *quality;
      quality_was_set = true;
      continue;
    }

    if (lower == L"--visual-quality" || lower == L"--visual_quality") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto visual_quality =
          config_detail::parse_int_range(*value, 1, 100, "visual-quality");
      if (!visual_quality) {
        return std::unexpected{visual_quality.error()};
      }
      cfg.visual_quality = *visual_quality;
      continue;
    }

    if (lower == L"--visual-quality-fallback" ||
        lower == L"--visual_quality_fallback") {
      cfg.visual_quality_fallback = true;
      continue;
    }

    if (lower == L"--no-visual-quality-fallback" ||
        lower == L"--no_visual_quality_fallback") {
      cfg.visual_quality_fallback = false;
      continue;
    }

    if (lower == L"-d" || lower == L"--depth" ||
        lower == L"--bit-depth" || lower == L"--bitdepth") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto bit_depth =
          config_detail::parse_int_range(*value, 1, 16, "bit-depth");
      if (!bit_depth) {
        return std::unexpected{bit_depth.error()};
      }
      cfg.bit_depth = std::optional<int>{*bit_depth};
      continue;
    }

    if (lower == L"--chroma" || lower == L"--sampling" ||
        lower == L"--subsampling") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto chroma = config_detail::parse_chroma(*value);
      if (!chroma) {
        return std::unexpected{std::format(
            "chroma 不支持: {}。可选值：auto、444、422、420。",
            config_detail::narrow_ascii(*value))};
      }
      cfg.chroma_mode = *chroma;
      continue;
    }

    if (lower == L"--avif-encoder" || lower == L"--avif_encoder") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto encoder = config_detail::parse_avif_encoder(*value);
      if (!encoder) {
        return std::unexpected{std::format(
            "AVIF encoder 不支持: {}。可选值：auto、svt、aom、zenrav1e。",
            config_detail::narrow_ascii(*value))};
      }
      cfg.avif_encoder = *encoder;
      continue;
    }

    if (lower == L"--experimental-encoders" ||
        lower == L"--enable-experimental-encoders") {
      cfg.enable_experimental_encoders = true;
      continue;
    }

    if (lower == L"--no-experimental-encoders") {
      cfg.enable_experimental_encoders = false;
      continue;
    }

    if (lower == L"--444" || lower == L"--yuv444") {
      cfg.chroma_mode = ChromaMode::yuv444;
      continue;
    }

    if (lower == L"--422" || lower == L"--yuv422") {
      cfg.chroma_mode = ChromaMode::yuv422;
      continue;
    }

    if (lower == L"--420" || lower == L"--yuv420") {
      cfg.chroma_mode = ChromaMode::yuv420;
      continue;
    }

    if (lower == L"-t" || lower == L"-j" || lower == L"--threads" ||
        lower == L"--jobs" || lower == L"--max-jobs") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto jobs = config_detail::parse_auto_jobs(*value);
      if (!jobs) {
        return std::unexpected{jobs.error()};
      }
      cfg.max_jobs = *jobs;
      continue;
    }

    if (lower == L"--memory-limit" || lower == L"--memory") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto memory_limit = config_detail::parse_memory_limit(*value);
      if (!memory_limit) {
        return std::unexpected{memory_limit.error()};
      }
      cfg.memory_limit_bytes = *memory_limit;
      continue;
    }

    if (lower == L"--speed") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto speed = config_detail::parse_int_range(*value, 0, 10, "speed");
      if (!speed) {
        return std::unexpected{speed.error()};
      }
      cfg.speed = *speed;
      continue;
    }

    if (lower == L"--max-resolution") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto max_resolution =
          config_detail::parse_int_range(*value, 0, 100000, "max-resolution");
      if (!max_resolution) {
        return std::unexpected{max_resolution.error()};
      }
      cfg.max_resolution = *max_resolution;
      continue;
    }

    if (lower == L"--timeout-encode") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto timeout =
          config_detail::parse_int_range(*value, 1, 24 * 60, "timeout-encode");
      if (!timeout) {
        return std::unexpected{timeout.error()};
      }
      cfg.encode_timeout_minutes = *timeout;
      continue;
    }

    if (lower == L"--option") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      if (auto valid = config_detail::validate_native_option(*value); !valid) {
        return std::unexpected{valid.error()};
      }
      continue;
    }

    if (lower == L"--define") {
      return std::unexpected{"--define 是旧 Magick 选项，内置 Magick 已移除；当前版本不再接收 Magick define。"};
    }

    if (lower == L"--collision") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto collision = config_detail::parse_collision(*value);
      if (!collision) {
        return std::unexpected{std::format(
            "冲突策略不支持: {}。可选值：overwrite、skip、time、random。",
            config_detail::narrow_ascii(*value))};
      }
      cfg.collision_mode = *collision;
      continue;
    }

    if (lower == L"--strip") {
      cfg.strip_metadata = true;
      continue;
    }

    if (lower == L"--keep-metadata") {
      cfg.strip_metadata = false;
      continue;
    }

    if (lower == L"--overwrite") {
      cfg.collision_mode = CollisionMode::overwrite;
      continue;
    }

    if (lower == L"--skip-existing") {
      cfg.collision_mode = CollisionMode::skip;
      continue;
    }

    if (lower == L"--suffix-time") {
      cfg.collision_mode = CollisionMode::suffix_time;
      continue;
    }

    if (lower == L"--suffix-random") {
      cfg.collision_mode = CollisionMode::suffix_random;
      continue;
    }

    if (lower == L"--summary") {
      cfg.write_summary = true;
      continue;
    }

    if (lower == L"--no-summary") {
      cfg.write_summary = false;
      continue;
    }

    if (lower == L"--log") {
      cfg.write_log = true;
      continue;
    }

    if (lower == L"--no-log") {
      cfg.write_log = false;
      continue;
    }

    return std::unexpected{
        std::format("未知参数: {}", config_detail::narrow_ascii(args[i]))};
  }

  if (auto valid = finalize_config_defaults(cfg, quality_was_set, preset_was_set); !valid) {
    return std::unexpected{valid.error()};
  }

  return ParseResult{.should_exit = false, .exit_code = 0, .config = cfg};
}

}  // namespace awj
