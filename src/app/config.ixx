module;

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <cstdio>
#include <scn/scan.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module avif.config;

export namespace avif {

enum class Preset { fast, balanced, best, extreme };
enum class OutputFormat { avif, webp };
enum class CollisionMode { overwrite, skip, suffix_time, suffix_random };
enum class ChromaMode { auto_keep, yuv444, yuv422, yuv420 };

int default_max_jobs() noexcept {
  // 自动并发不是“吃满 CPU”，而是给桌面、UI 线程和 ImageMagick 内部线程预留余量。
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
  return format == OutputFormat::webp ? 95 : 90;
}

// 命令行只负责生成这个配置对象；后面的流水线不会再回头解析 argv。
struct AppConfig {
  std::filesystem::path input_path{L"input"};
  std::filesystem::path output_dir{};
  std::filesystem::path magick_path{};
  std::wstring output_template{L"{name}"};
  std::vector<std::wstring> magick_defines{};
  Preset preset{Preset::best};
  OutputFormat output_format{OutputFormat::avif};
  CollisionMode collision_mode{CollisionMode::overwrite};
  ChromaMode chroma_mode{ChromaMode::auto_keep};
  int quality{default_quality_for(OutputFormat::avif)};
  std::optional<int> bit_depth{};
  std::optional<int> magick_speed{};
  int max_jobs{default_max_jobs()};
  int max_resolution{0};
  int encode_timeout_minutes{30};
  bool optimize_output{false};
  double optimize_target_xpsnr{42.0};
  int optimize_min_quality{50};
  bool strip_metadata{false};
  bool write_summary{false};
  bool write_log{false};
  bool magick_path_overridden{false};
};

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
  // 同时接受 90、q90 和 0.9；内部统一归一化到 ImageMagick 的 1..100 质量值。
  const auto value = scan_number<double>(text);
  if (!value) {
    return std::unexpected{"质量参数必须是数字，例如 90 或 q90。"};
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

std::expected<double, std::string> parse_double_range(std::wstring_view text,
                                                      double min_value,
                                                      double max_value,
                                                      std::string_view name) {
  const auto value = scan_number<double>(text);
  if (!value) {
    return std::unexpected{std::format("{} 必须是数字。", name)};
  }
  if (*value < min_value || *value > max_value) {
    return std::unexpected{
        std::format("{} 范围必须在 {:.3f} 到 {:.3f} 之间。",
                    name, min_value, max_value)};
  }
  return *value;
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

bool avif_bit_depth_supported(int bit_depth) noexcept {
  return bit_depth == 8 || bit_depth == 10 || bit_depth == 12;
}

void apply_preset(AppConfig& cfg, Preset preset) {
  cfg.preset = preset;
  switch (preset) {
    case Preset::fast:
      cfg.quality = 75;
      cfg.encode_timeout_minutes = 10;
      break;
    case Preset::balanced:
      cfg.quality = 85;
      cfg.encode_timeout_minutes = 20;
      break;
    case Preset::best:
      cfg.quality = 90;
      cfg.encode_timeout_minutes = 30;
      break;
    case Preset::extreme:
      cfg.quality = 95;
      cfg.encode_timeout_minutes = 60;
      break;
  }
}

}  // namespace config_detail

std::string chroma_mode_name(ChromaMode mode) {
  return config_detail::chroma_name(mode);
}

std::expected<void, std::string> validate_config(const AppConfig& cfg) {
  // 路径存在性在 scan_images 中校验；这里专注于格式自身不能违反的编码约束。
  if (cfg.output_format == OutputFormat::webp) {
    if (cfg.bit_depth && *cfg.bit_depth != 8) {
      return std::unexpected{
          "WebP bitstream 只支持 8-bit；请把位深设为 8，或留空保持原片。"};
    }
    if (cfg.chroma_mode != ChromaMode::auto_keep) {
      return std::unexpected{
          "WebP 不支持手动选择 444/422/420；有损 WebP 为 8-bit 4:2:0，"
          "无损 WebP 为 8-bit ARGB。"};
    }
  } else if (cfg.bit_depth &&
             !config_detail::avif_bit_depth_supported(*cfg.bit_depth)) {
    return std::unexpected{
        "当前 ImageMagick/libheif AVIF 输出仅支持 8、10、12-bit 位深；留空表示保持原片。"};
  }
  return {};
}

void print_help() {
  constexpr std::string_view help = R"(AVIF-WebP-Studio C++23
=======================

默认后端：ImageMagick MagickWand
默认质量：AVIF q90，WebP q95
质量范围：q1..q100，q100 为无损

用法:
  AVIF-WebP-Cli.exe [选项]

常用选项:
  -i, --input <路径>          输入文件或目录，默认 input
  -o, --output <目录>         输出目录；默认与输入同目录
  -f, --format <avif|webp>    输出格式，默认 avif
  -q, --quality <1-100>       ImageMagick 质量，AVIF 默认 90，WebP 默认 95；100 为无损。也接受 q90 或 0.9
  -d, --bit-depth <位深>      AVIF 支持 8/10/12；不填保持原片，WebP 固定 8
  --chroma <auto|444|422|420> AVIF 色度采样，默认 auto 会尽量保持源采样；也可用 --444 / --422 / --420
  -p, --preset <名称>         fast / balanced / best / extreme，默认 best
  -t, --threads <数量>        并发数量，默认 CPU 线程数
  -m, --template <模板>       输出命名，默认 {name}
  --max-resolution <像素>     限制最长边；0 表示不缩放，默认 0
  --speed <0-10>             可选：传给 ImageMagick heic:speed；默认使用 Magick 自身默认值
  --define <key=value>        额外传给 MagickWand 的 define，可重复
  --collision <策略>          overwrite / skip / time / random，默认 overwrite
  --backend magick            后端占位参数；当前仅支持 magick
  --magick <路径>             指定 ImageMagick 运行时目录
  --timeout-encode <分钟>     单张图片编码超时，默认 30
  --optimize                 自动搜索达到目标 XPSNR 的最小体积版本
  --target-xpsnr <dB>        optimize 的最低 XPSNR，默认 42.0 dB
  --min-quality <1-100>      optimize 的最低搜索质量，默认 50
  --strip                    去除 EXIF/ICC 等元数据，通常更小且更隐私
  --summary                  生成 summary.csv
  --log                      生成 log\avif-console.log
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
  AVIF-WebP-Cli.exe -i "D:\图片" -o Avifoutput -q q90
  AVIF-WebP-Cli.exe -i input --format webp --template "{name}-{date}"
  AVIF-WebP-Cli.exe -i input --chroma 444 --bit-depth 10 --optimize
)";
  std::fwrite(help.data(), 1, help.size(), stdout);
  std::fputc('\n', stdout);
}

std::expected<ParseResult, std::string> parse_arguments(
    const std::vector<std::wstring>& args) {
  // CLI 参数直接落到 AppConfig，确保命令行和 Slint UI 走同一套核心逻辑。
  AppConfig cfg;
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
            "输出格式不支持: {}。可选值：avif、webp。",
            config_detail::narrow_ascii(*value))};
      }
      cfg.output_format = *format;
      continue;
    }

    if (lower == L"--magick") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.magick_path = *value;
      cfg.magick_path_overridden = true;
      continue;
    }

    if (lower == L"--backend") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto backend = config_detail::lower_copy(*value);
      if (backend != L"magick" && backend != L"imagemagick") {
        return std::unexpected{"当前版本仅支持 magick / imagemagick 后端。"};
      }
      continue;
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

    if (lower == L"--speed") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto speed = config_detail::parse_int_range(*value, 0, 10, "speed");
      if (!speed) {
        return std::unexpected{speed.error()};
      }
      cfg.magick_speed = *speed;
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

    if (lower == L"--optimize" || lower == L"--search-best") {
      cfg.optimize_output = true;
      continue;
    }

    if (lower == L"--no-optimize") {
      cfg.optimize_output = false;
      continue;
    }

    if (lower == L"--target-xpsnr" || lower == L"--optimize-xpsnr") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto target =
          config_detail::parse_double_range(*value, 0.0, 120.0, "target-xpsnr");
      if (!target) {
        return std::unexpected{target.error()};
      }
      cfg.optimize_target_xpsnr = *target;
      continue;
    }

    if (lower == L"--target-ssim" || lower == L"--optimize-ssim") {
      return std::unexpected{
          "自动搜索已改为 XPSNR，请使用 --target-xpsnr <dB>。"};
    }

    if (lower == L"--min-quality" || lower == L"--optimize-min-quality") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      const auto minimum =
          config_detail::parse_int_range(*value, 1, 100, "min-quality");
      if (!minimum) {
        return std::unexpected{minimum.error()};
      }
      cfg.optimize_min_quality = *minimum;
      continue;
    }

    if (lower == L"--define") {
      const auto value = require_value(i, args[i]);
      if (!value) {
        return std::unexpected{value.error()};
      }
      cfg.magick_defines.push_back(*value);
      continue;
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

  if (!quality_was_set && !preset_was_set) {
    cfg.quality = default_quality_for(cfg.output_format);
  }
  if (auto valid = validate_config(cfg); !valid) {
    return std::unexpected{valid.error()};
  }

  return ParseResult{.should_exit = false, .exit_code = 0, .config = cfg};
}

}  // namespace avif
