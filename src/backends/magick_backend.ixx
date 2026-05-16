module;

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <MagickWand/MagickWand.h>
#if __has_include(<heif/libheif/heif.h>)
#ifndef LIBHEIF_STATIC_BUILD
#define LIBHEIF_STATIC_BUILD 1
#endif
#include <heif/libheif/heif.h>
#define AVIF_STUDIO_HAS_LIBHEIF 1
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

export module avif.magick_backend;

import avif.config;
import avif.core;

export namespace avif {

struct MagickRuntime {
  fs::path root{};
  bool bundled{false};
};

namespace magick_detail {

std::optional<fs::path> absolute_directory(fs::path path) {
  std::error_code ec;
  path = fs::absolute(std::move(path), ec);
  if (ec) {
    return std::nullopt;
  }
  if (fs::is_regular_file(path, ec) && !ec) {
    path = path.parent_path();
  }
  if (fs::is_directory(path, ec) && !ec) {
    return path;
  }
  return std::nullopt;
}

bool looks_like_magick_runtime(const fs::path& root) {
  std::error_code ec;
  const bool has_wand_dll =
      (fs::exists(root / L"CORE_RL_MagickWand_.dll", ec) && !ec) ||
      (fs::exists(root / L"CORE_DB_MagickWand_.dll", ec) && !ec);
  const bool has_wand_lib =
      (fs::exists(root / L"lib" / L"CORE_RL_MagickWand_.lib", ec) && !ec) ||
      (fs::exists(root / L"lib" / L"CORE_DB_MagickWand_.lib", ec) && !ec);
  const bool has_include =
      fs::exists(root / L"include" / L"MagickWand" / L"MagickWand.h", ec) &&
      !ec;
  return has_wand_dll || has_wand_lib || has_include;
}

std::optional<fs::path> existing_runtime_root(fs::path path) {
  auto root = absolute_directory(std::move(path));
  if (!root || !looks_like_magick_runtime(*root)) {
    return std::nullopt;
  }
  return root;
}

void collect_ancestor_runtime_candidates(std::vector<fs::path>& candidates,
                                         fs::path start) {
  std::error_code ec;
  start = fs::absolute(start, ec);
  if (ec) {
    return;
  }

  for (auto current = start; !current.empty(); current = current.parent_path()) {
    candidates.push_back(current);
    candidates.push_back(current / L"third_party" /
                         L"imagemagick-runtime" / L"x64" / L"Release");
    if (current == current.root_path()) {
      break;
    }
  }
}

std::optional<fs::path> environment_runtime() {
  std::wstring buffer(32768, L'\0');
  const DWORD size =
      GetEnvironmentVariableW(L"AVIF_MAGICK", buffer.data(),
                              static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    return std::nullopt;
  }
  buffer.resize(size);
  return existing_runtime_root(buffer);
}

std::vector<fs::path> bundled_candidates() {
  std::vector<fs::path> candidates;
  collect_ancestor_runtime_candidates(candidates, executable_directory());
  return candidates;
}

void set_env_if_directory(std::wstring_view name, const fs::path& path) {
  std::error_code ec;
  if (fs::is_directory(path, ec) && !ec) {
    const auto value = path.native();
    SetEnvironmentVariableW(std::wstring{name}.c_str(), value.c_str());
  }
}

std::string magick_exception(MagickWand* wand, std::string_view fallback) {
  if (wand == nullptr) {
    return std::string{fallback};
  }

  ExceptionType severity{};
  char* raw = MagickGetException(wand, &severity);
  if (raw == nullptr) {
    return std::string{fallback};
  }

  std::string message{raw};
  MagickRelinquishMemory(raw);
  if (message.empty()) {
    return std::string{fallback};
  }
  return message;
}

struct WandDeleter {
  void operator()(MagickWand* wand) const noexcept {
    if (wand != nullptr) {
      DestroyMagickWand(wand);
    }
  }
};

using WandPtr = std::unique_ptr<MagickWand, WandDeleter>;

struct OperationMonitor {
  OperationMonitor(std::stop_token token, int timeout_minutes)
      : stop_token{std::move(token)} {
    if (timeout_minutes > 0) {
      has_deadline = true;
      deadline = std::chrono::steady_clock::now() +
                 std::chrono::minutes{timeout_minutes};
    }
  }

  bool stop_requested() const {
    return stop_token.stop_requested() || stopped.load();
  }

  std::stop_token stop_token{};
  std::chrono::steady_clock::time_point deadline{};
  bool has_deadline{false};
  std::atomic_bool stopped{false};
  std::atomic_bool timed_out{false};
};

MagickBooleanType progress_monitor(const char*,
                                   const MagickOffsetType,
                                   const MagickSizeType,
                                   void* client_data) {
  auto* monitor = static_cast<OperationMonitor*>(client_data);
  if (monitor == nullptr) {
    return MagickTrue;
  }
  if (monitor->stop_token.stop_requested()) {
    monitor->stopped.store(true);
    return MagickFalse;
  }
  if (monitor->has_deadline &&
      std::chrono::steady_clock::now() >= monitor->deadline) {
    monitor->timed_out.store(true);
    return MagickFalse;
  }
  return MagickTrue;
}

std::optional<std::string> interruption_message(
    const OperationMonitor& monitor,
    int timeout_minutes) {
  if (monitor.stop_requested()) {
    return "已取消。";
  }
  if (monitor.timed_out.load()) {
    return std::format("单张图片处理超过 {} 分钟，已中止。", timeout_minutes);
  }
  return std::nullopt;
}

void ensure_magick_initialized() {
  // 全局资源上限防止单张异常大图把内存/临时盘吃满；不限制 Magick 内部线程数。
  static std::once_flag once;
  std::call_once(once, [] {
    MagickWandGenesis();
    MagickSetResourceLimit(MemoryResource, 1024ull * 1024ull * 1024ull);
    MagickSetResourceLimit(MapResource, 2ull * 1024ull * 1024ull * 1024ull);
    MagickSetResourceLimit(AreaResource, 512ull * 1024ull * 1024ull);
    MagickSetResourceLimit(DiskResource, 8ull * 1024ull * 1024ull * 1024ull);
    MagickSetResourceLimit(TimeResource, 60ull * 60ull);
  });
}

std::expected<void, std::string> check(MagickWand* wand,
                                       MagickBooleanType status,
                                       std::string_view action,
                                       const OperationMonitor* monitor = nullptr,
                                       int timeout_minutes = 0) {
  if (status != MagickFalse) {
    return {};
  }
  if (monitor != nullptr) {
    if (auto message = interruption_message(*monitor, timeout_minutes)) {
      return std::unexpected{*message};
    }
  }
  return std::unexpected{std::format("{}: {}", action,
                                     magick_exception(wand, "MagickWand 调用失败"))};
}

std::string with_path_context(std::string_view stage,
                              const fs::path& input,
                              const fs::path& output,
                              std::string_view detail) {
  return std::format("{}失败。输入：{}；输出：{}；原因：{}",
                     stage, path_to_utf8(input), path_to_utf8(output), detail);
}

std::string define_key(std::string_view define) {
  const auto pos = define.find('=');
  if (pos == std::string_view::npos) {
    return std::string{define};
  }
  return std::string{define.substr(0, pos)};
}

std::string define_value(std::string_view define) {
  const auto pos = define.find('=');
  if (pos == std::string_view::npos) {
    return "true";
  }
  return std::string{define.substr(pos + 1)};
}

bool is_sensitive_define_key(std::string key) {
  std::ranges::transform(key, key.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return key.find("token") != std::string::npos ||
         key.find("secret") != std::string::npos ||
         key.find("password") != std::string::npos ||
         key.find("credential") != std::string::npos ||
         key.find("api-key") != std::string::npos ||
         key.find("apikey") != std::string::npos;
}

std::string describe_define(std::string_view define) {
  const auto key = define_key(define);
  const auto pos = define.find('=');
  if (pos == std::string_view::npos || !is_sensitive_define_key(key)) {
    return std::string{define};
  }
  return std::format("{}=<redacted>", key);
}

}  // namespace magick_detail

void configure_magick_environment(const MagickRuntime& runtime) {
  const auto root = runtime.root.native();
  SetEnvironmentVariableW(L"MAGICK_HOME", root.c_str());
  SetEnvironmentVariableW(L"MAGICK_CONFIGURE_PATH", root.c_str());

  magick_detail::set_env_if_directory(
      L"MAGICK_CODER_MODULE_PATH", runtime.root / L"modules" / L"coders");
  magick_detail::set_env_if_directory(
      L"MAGICK_FILTER_MODULE_PATH", runtime.root / L"modules" / L"filters");
}

std::expected<MagickRuntime, std::string> resolve_magick_runtime(
    const AppConfig& cfg) {
  if (cfg.magick_path_overridden) {
    if (const auto direct =
            magick_detail::existing_runtime_root(cfg.magick_path)) {
      return MagickRuntime{.root = *direct, .bundled = false};
    }
    return std::unexpected{
        std::format("未找到指定的 ImageMagick 运行时目录: {}",
                    path_to_utf8(cfg.magick_path))};
  }

  if (const auto from_env = magick_detail::environment_runtime()) {
    return MagickRuntime{.root = *from_env, .bundled = false};
  }

  for (const auto& candidate : magick_detail::bundled_candidates()) {
    if (const auto bundled = magick_detail::existing_runtime_root(candidate)) {
      return MagickRuntime{.root = *bundled, .bundled = true};
    }
  }

  // Static builds can run with ImageMagick's built-in defaults. Treat the
  // executable directory as MAGICK_HOME so a copied single exe gets a chance to
  // run, while bundled config XML beside the exe is still picked up when present.
  return MagickRuntime{.root = executable_directory(), .bundled = true};
}

class MagickBackend {
 public:
  MagickBackend(const AppConfig& cfg,
                const MagickRuntime& runtime,
                FileLogger& logger)
      : cfg_{cfg}, runtime_{runtime}, logger_{logger} {
    (void)runtime_;
    magick_detail::ensure_magick_initialized();
  }

  EncodeResult encode(const ImageFile& image,
                      std::stop_token stop_token = {}) const {
    const auto start = std::chrono::steady_clock::now();
    auto output = output_path_for(cfg_, image);
    EncodeResult result{.index = image.index,
                        .input_path = image.path,
                        .output_path = output,
                        .original_bytes = image.bytes,
                        .output_bytes = 0,
                        .quality = cfg_.quality,
                        .speed = cfg_.magick_speed.value_or(-1)};
    magick_detail::OperationMonitor monitor{stop_token,
                                            cfg_.encode_timeout_minutes};

    try {
      if (monitor.stop_requested()) {
        result.canceled = true;
        result.message = "已取消。";
        return finish(result, start);
      }

      output = resolve_output_path(image, output);
      result.output_path = output;

      std::error_code ec;
      fs::create_directories(output.parent_path(), ec);
      if (ec) {
        result.message = std::format(
            "创建输出目录失败。输入：{}；输出目录：{}；系统错误：{}。请确认目标磁盘存在、目录有写入权限且路径未被占用。",
            path_to_utf8(image.path), path_to_utf8(output.parent_path()), ec.message());
        return finish(result, start);
      }

      if (cfg_.collision_mode == CollisionMode::skip) {
        ec.clear();
        if (fs::exists(output, ec) && !ec) {
          const auto existing_size = fs::file_size(output, ec);
          if (!ec && existing_size > 0) {
            result.ok = true;
            result.skipped = true;
            result.output_bytes = existing_size;
            result.message = "已存在，跳过。";
            return finish(result, start);
          }
        }
      }

      auto wand = magick_detail::WandPtr{NewMagickWand()};
      if (!wand) {
        result.message = "无法创建 MagickWand。";
        logger_.error(result.message);
        return finish(result, start);
      }
      MagickSetProgressMonitor(wand.get(), magick_detail::progress_monitor,
                               &monitor);

      if (auto ok = read_and_prepare(*wand, image, output, monitor); !ok) {
        result.message = ok.error();
        result.canceled = monitor.stop_requested();
        logger_.error(result.message);
        return finish(result, start);
      }

      const auto selected_quality = write_output(*wand, output, monitor, image);
      if (!selected_quality) {
        result.message = magick_detail::with_path_context(
            "写入编码结果", image.path, output, selected_quality.error());
        result.canceled = monitor.stop_requested();
        logger_.error(result.message);
        return finish(result, start);
      }
      result.quality = *selected_quality;
      result.command = command_description(image, output, result.quality);

      if (!fs::exists(output, ec) || fs::file_size(output, ec) == 0 || ec) {
        result.message = "MagickWand 已结束，但没有生成有效输出文件。";
        logger_.error(result.message);
        return finish(result, start);
      }

      result.output_bytes = fs::file_size(output, ec);
      result.ok = true;
      result.message = "完成。";
      logger_.info(std::format("encode: {}", result.command));
      return finish(result, start);
    } catch (const std::exception& ex) {
      result.message = std::format("异常: {}", ex.what());
      logger_.error(result.message);
      return finish(result, start);
    } catch (...) {
      result.message = "未知异常。";
      logger_.error(result.message);
      return finish(result, start);
    }
  }

 private:
  static EncodeResult finish(EncodeResult result,
                             std::chrono::steady_clock::time_point start) {
    const auto end = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(end - start).count();
    result.processed = true;
    return result;
  }

  fs::path resolve_output_path(const ImageFile& image, fs::path output) const {
    if (cfg_.collision_mode != CollisionMode::suffix_time &&
        cfg_.collision_mode != CollisionMode::suffix_random) {
      return output;
    }

    const auto extension = output.extension().wstring();
    const auto stem = output.stem().wstring();
    const auto suffix =
        cfg_.collision_mode == CollisionMode::suffix_time
            ? std::format(L"{}-{:04}", image.datetime_token, image.index + 1)
            : image.random_token;

    auto candidate = output;
    candidate.replace_filename(stem + L"-" + suffix + extension);

    std::error_code ec;
    for (int counter = 2; fs::exists(candidate, ec) && !ec; ++counter) {
      candidate = output;
      candidate.replace_filename(
          stem + L"-" + suffix + std::format(L"-{}", counter) + extension);
    }
    return candidate;
  }

  std::expected<void, std::string> read_and_prepare(MagickWand& wand,
                                                    const ImageFile& image,
                                                    const fs::path& output,
                                                    magick_detail::OperationMonitor&
                                                        monitor) const {
    if (auto message =
            magick_detail::interruption_message(monitor,
                                                cfg_.encode_timeout_minutes)) {
      return std::unexpected{*message};
    }

    const auto input = path_to_utf8(image.path);
    if (auto ok = magick_detail::check(&wand, MagickReadImage(&wand, input.c_str()),
                                       "读取图片", &monitor,
                                       cfg_.encode_timeout_minutes);
        !ok) {
      return std::unexpected{magick_detail::with_path_context(
          "读取图片", image.path, output, ok.error())};
    }

    if (cfg_.output_format == OutputFormat::avif &&
        cfg_.chroma_mode == ChromaMode::auto_keep &&
        !has_define("heic:chroma")) {
      if (const auto chroma = source_chroma_from_properties(image.path, wand)) {
        if (auto ok = set_option(wand, "heic:chroma", *chroma,
                                 "设置 heic:chroma(auto) 失败", monitor);
            !ok) {
          return ok;
        }
      }
    }
    MagickSetImageProgressMonitor(&wand, magick_detail::progress_monitor,
                                  &monitor);

    if (auto ok = magick_detail::check(&wand, MagickAutoOrientImage(&wand),
                                       "自动旋转图片", &monitor,
                                       cfg_.encode_timeout_minutes);
        !ok) {
      return std::unexpected{magick_detail::with_path_context(
          "自动旋转图片", image.path, output, ok.error())};
    }

    if (auto ok = resize_if_needed(wand, monitor); !ok) {
      return std::unexpected{magick_detail::with_path_context(
          "缩放图片", image.path, output, ok.error())};
    }

    if (cfg_.strip_metadata) {
      if (auto ok = magick_detail::check(&wand, MagickStripImage(&wand),
                                         "去除元数据", &monitor,
                                         cfg_.encode_timeout_minutes);
          !ok) {
        return std::unexpected{magick_detail::with_path_context(
            "去除元数据", image.path, output, ok.error())};
      }
    }

    return {};
  }

  std::expected<void, std::string> resize_if_needed(
      MagickWand& wand,
      const magick_detail::OperationMonitor& monitor) const {
    constexpr std::size_t webp_max_dimension = 16383;
    // WebP 编码器有 16383 像素边长上限；即使用户不缩放，也要自动兜底。
    auto target_longest = cfg_.max_resolution > 0
                              ? static_cast<std::size_t>(cfg_.max_resolution)
                              : std::numeric_limits<std::size_t>::max();
    if (cfg_.output_format == OutputFormat::webp) {
      target_longest = std::min(target_longest, webp_max_dimension);
    }
    if (target_longest == std::numeric_limits<std::size_t>::max()) {
      return {};
    }
    if (auto message =
            magick_detail::interruption_message(monitor,
                                                cfg_.encode_timeout_minutes)) {
      return std::unexpected{*message};
    }

    const auto width = MagickGetImageWidth(&wand);
    const auto height = MagickGetImageHeight(&wand);
    const auto longest = std::max(width, height);
    if (longest <= target_longest) {
      return {};
    }

    const double scale =
        static_cast<double>(target_longest) / static_cast<double>(longest);
    const auto next_width =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(width * scale)));
    const auto next_height =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(height * scale)));
    return magick_detail::check(&wand,
                                MagickResizeImage(&wand, next_width, next_height,
                                                  LanczosFilter),
                                "缩放图片失败", &monitor,
                                cfg_.encode_timeout_minutes);
  }

  struct CandidateResult {
    int quality{};
    fs::path path{};
    std::uintmax_t bytes{};
    double xpsnr{};
  };

  class CandidateFileCleanup {
   public:
    void track(const fs::path& path) { paths_.push_back(path); }

    void release(const fs::path& path) {
      std::erase(paths_, path);
    }

    ~CandidateFileCleanup() {
      std::error_code ec;
      for (const auto& path : paths_) {
        fs::remove(path, ec);
      }
    }

   private:
    std::vector<fs::path> paths_;
  };

  struct MagickStringDeleter {
    void operator()(char* value) const noexcept {
      if (value != nullptr) {
        (void) MagickRelinquishMemory(value);
      }
    }
  };

  static std::optional<std::string> image_property(MagickWand& wand,
                                                   const char* key) {
    std::unique_ptr<char, MagickStringDeleter> value{
        MagickGetImageProperty(&wand, key)};
    if (!value || value.get()[0] == '\0') {
      return std::nullopt;
    }
    return std::string{value.get()};
  }

  static std::optional<std::string> chroma_from_sampling_factor(
      std::string value) {
    std::erase_if(value, [](unsigned char ch) {
      return std::isspace(ch) != 0;
    });
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });

    if (value.starts_with("1x1,1x1,1x1") ||
        value.starts_with("1x1x1,1x1x1,1x1x1") ||
        value.starts_with("4:4:4")) {
      return "444";
    }
    if (value.starts_with("2x1,1x1,1x1") || value.starts_with("4:2:2")) {
      return "422";
    }
    if (value.starts_with("2x2,1x1,1x1") || value.starts_with("4:2:0")) {
      return "420";
    }
    return std::nullopt;
  }

#ifdef AVIF_STUDIO_HAS_LIBHEIF
  struct HeifContextDeleter {
    void operator()(heif_context* context) const noexcept {
      if (context != nullptr) {
        heif_context_free(context);
      }
    }
  };

  struct HeifImageHandleDeleter {
    void operator()(heif_image_handle* handle) const noexcept {
      if (handle != nullptr) {
        heif_image_handle_release(handle);
      }
    }
  };

  static std::optional<std::string> chroma_from_heif_chroma(
      heif_chroma chroma) {
    switch (chroma) {
      case heif_chroma_444:
      case heif_chroma_interleaved_RGB:
      case heif_chroma_interleaved_RGBA:
      case heif_chroma_interleaved_RRGGBB_BE:
      case heif_chroma_interleaved_RRGGBBAA_BE:
      case heif_chroma_interleaved_RRGGBB_LE:
      case heif_chroma_interleaved_RRGGBBAA_LE:
        return "444";
      case heif_chroma_422:
        return "422";
      case heif_chroma_420:
        return "420";
      default:
        return std::nullopt;
    }
  }

  static std::optional<std::string> source_chroma_from_heif(
      const fs::path& path) {
    const auto ext = path.extension().wstring();
    const auto lower_ext = [&] {
      auto value = ext;
      std::ranges::transform(value, value.begin(),
                             [](wchar_t ch) { return std::towlower(ch); });
      return value;
    }();
    if (lower_ext != L".avif" && lower_ext != L".heic" &&
        lower_ext != L".heif") {
      return std::nullopt;
    }

    std::unique_ptr<heif_context, HeifContextDeleter> context{
        heif_context_alloc()};
    if (!context) {
      return std::nullopt;
    }
    if (const auto error =
            heif_context_read_from_file(context.get(),
                                        path_to_utf8(path).c_str(), nullptr);
        error.code != heif_error_Ok) {
      return std::nullopt;
    }

    heif_image_handle* raw_handle = nullptr;
    if (const auto error =
            heif_context_get_primary_image_handle(context.get(), &raw_handle);
        error.code != heif_error_Ok || raw_handle == nullptr) {
      return std::nullopt;
    }
    std::unique_ptr<heif_image_handle, HeifImageHandleDeleter> handle{
        raw_handle};

    heif_colorspace colorspace = heif_colorspace_undefined;
    heif_chroma chroma = heif_chroma_undefined;
    if (const auto error =
            heif_image_handle_get_preferred_decoding_colorspace(
                handle.get(), &colorspace, &chroma);
        error.code != heif_error_Ok) {
      return std::nullopt;
    }
    return chroma_from_heif_chroma(chroma);
  }
#endif

  static std::optional<std::string> source_chroma_from_properties(
      const fs::path& path,
      MagickWand& wand) {
#ifdef AVIF_STUDIO_HAS_LIBHEIF
    if (const auto chroma = source_chroma_from_heif(path)) {
      return chroma;
    }
#endif
    if (const auto sampling = image_property(wand, "jpeg:sampling-factor")) {
      return chroma_from_sampling_factor(*sampling);
    }
    return std::nullopt;
  }

  bool has_define(std::string_view expected_key) const {
    for (const auto& define : cfg_.magick_defines) {
      auto key = magick_detail::define_key(utf8_from_wide(define));
      std::ranges::transform(key, key.begin(),
                             [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                             });
      if (key == expected_key) {
        return true;
      }
    }
    return false;
  }

  std::expected<void, std::string> set_option(
      MagickWand& wand,
      std::string_view key,
      std::string_view value,
      std::string_view action,
      magick_detail::OperationMonitor& monitor) const {
    return magick_detail::check(
        &wand,
        MagickSetOption(&wand, std::string{key}.c_str(),
                        std::string{value}.c_str()),
        action, &monitor, cfg_.encode_timeout_minutes);
  }

  std::expected<void, std::string> apply_encoder_options(
      MagickWand& wand,
      int quality,
      magick_detail::OperationMonitor& monitor,
      bool optimizer_candidate) const {
    if (auto message =
            magick_detail::interruption_message(monitor,
                                                cfg_.encode_timeout_minutes)) {
      return std::unexpected{*message};
    }

    const auto magick_quality = static_cast<std::size_t>(quality);
    if (auto ok = magick_detail::check(
            &wand, MagickSetCompressionQuality(&wand, magick_quality),
            "设置 wand 质量失败", &monitor, cfg_.encode_timeout_minutes);
        !ok) {
      return ok;
    }
    if (auto ok = magick_detail::check(
            &wand, MagickSetImageCompressionQuality(&wand, magick_quality),
            "设置图片质量失败", &monitor, cfg_.encode_timeout_minutes);
        !ok) {
      return ok;
    }
    if (cfg_.bit_depth) {
      if (auto ok = magick_detail::check(
              &wand, MagickSetImageDepth(&wand,
                                         static_cast<std::size_t>(*cfg_.bit_depth)),
              "设置图片位深失败", &monitor, cfg_.encode_timeout_minutes);
          !ok) {
        return ok;
      }
    }

    if (cfg_.output_format == OutputFormat::avif) {
      if (cfg_.magick_speed) {
        const auto value = std::to_string(*cfg_.magick_speed);
        if (auto ok = set_option(wand, "heic:speed", value,
                                 "设置 heic:speed 失败", monitor);
            !ok) {
          return ok;
        }
      } else if (optimizer_candidate && !has_define("heic:speed")) {
        // 自动搜索模式优先体积，AVIF 候选使用 libheif/aom 的最慢压缩档。
        if (auto ok = set_option(wand, "heic:speed", "0",
                                 "设置 heic:speed 失败", monitor);
            !ok) {
          return ok;
        }
      }
      if (cfg_.chroma_mode != ChromaMode::auto_keep &&
          !has_define("heic:chroma")) {
        if (auto ok = set_option(wand, "heic:chroma",
                                 chroma_mode_name(cfg_.chroma_mode),
                                 "设置 heic:chroma 失败", monitor);
            !ok) {
          return ok;
        }
      }
    } else if (cfg_.output_format == OutputFormat::webp &&
               optimizer_candidate && !has_define("webp:method")) {
      if (auto ok = set_option(wand, "webp:method", "6",
                               "设置 webp:method 失败", monitor);
          !ok) {
        return ok;
      }
    }

    for (const auto& define : cfg_.magick_defines) {
      const auto define_utf8 = utf8_from_wide(define);
      const auto key = magick_detail::define_key(define_utf8);
      const auto value = magick_detail::define_value(define_utf8);
      if (auto valid = validate_magick_define(define); !valid) {
        return std::unexpected{valid.error()};
      }
      if (auto ok = magick_detail::check(&wand,
                                         MagickSetOption(&wand, key.c_str(),
                                                         value.c_str()),
                                         std::format("设置 define {} 失败", key),
                                         &monitor, cfg_.encode_timeout_minutes);
          !ok) {
        return ok;
      }
    }

    const auto format = output_format_name(cfg_.output_format);
    if (auto ok = magick_detail::check(
            &wand, MagickSetImageFormat(&wand, format.c_str()),
            std::format("设置 {} 格式失败", format), &monitor,
            cfg_.encode_timeout_minutes);
        !ok) {
      return ok;
    }

    return {};
  }

  std::expected<void, std::string> write_direct_output(
      MagickWand& wand,
      const fs::path& output,
      int quality,
      magick_detail::OperationMonitor& monitor,
      bool optimizer_candidate,
      std::size_t image_index) const {
    if (auto ok = apply_encoder_options(wand, quality, monitor,
                                        optimizer_candidate);
        !ok) {
      return ok;
    }

    const auto format = output_format_name(cfg_.output_format);
    auto write_path = output;
    if (!optimizer_candidate) {
      auto candidate = candidate_path(output, quality, image_index);
      if (!candidate) {
        return std::unexpected{candidate.error()};
      }
      write_path = *candidate;
    }

    const auto output_utf8 = path_to_utf8(write_path);
    if (auto ok =
            magick_detail::check(&wand,
                                 MagickWriteImage(&wand, output_utf8.c_str()),
                                 std::format("写入 {} 失败", format), &monitor,
                                 cfg_.encode_timeout_minutes);
        !ok) {
      std::error_code ec;
      if (!optimizer_candidate) {
        fs::remove(write_path, ec);
      }
      return ok;
    }
    if (optimizer_candidate) {
      return {};
    }
    return replace_with_candidate(write_path, output);
  }

  fs::path candidate_path_with_nonce(const fs::path& output,
                                     int quality,
                                     std::size_t image_index,
                                     std::uint64_t nonce) const {
    const auto suffix = std::format(L".avif-webp-studio-{}-{}-q{}-{:016x}{}",
                                    GetCurrentProcessId(), image_index + 1, quality,
                                    nonce, output.extension().wstring());
    return output.parent_path() / (output.stem().wstring() + suffix);
  }

  std::expected<fs::path, std::string> candidate_path(
      const fs::path& output,
      int quality,
      std::size_t image_index) const {
    // 先写随机临时文件，再原子替换目标，避免失败时留下半截输出。
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    std::error_code ec;
    for (int attempt = 0; attempt < 64; ++attempt) {
      const auto candidate = candidate_path_with_nonce(output, quality, image_index, rng());
      if (!fs::exists(candidate, ec) && !ec) {
        return candidate;
      }
      ec.clear();
    }
    return std::unexpected{"无法创建唯一临时输出文件。"};
  }

  std::expected<void, std::string> replace_with_candidate(
      const fs::path& candidate,
      const fs::path& output) const {
    if (MoveFileExW(candidate.c_str(), output.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
      return {};
    }

    const auto error = GetLastError();
    std::error_code ec;
    fs::remove(candidate, ec);
    return std::unexpected{std::format(
        "替换输出文件失败: {}。临时文件：{}；目标文件：{}。请检查目标文件是否正被图片查看器占用、目录是否可写，或杀毒软件是否拦截。",
        win32_error_message(error), path_to_utf8(candidate), path_to_utf8(output))};
  }

  std::expected<std::vector<double>, std::string> export_luma_row(
      MagickWand& wand,
      std::size_t width,
      std::size_t y,
      magick_detail::OperationMonitor& monitor,
      std::string_view label) const {
    if (auto message =
            magick_detail::interruption_message(monitor,
                                                cfg_.encode_timeout_minutes)) {
      return std::unexpected{*message};
    }

    std::vector<Quantum> rgb(width * 3);
    if (auto ok = magick_detail::check(
            &wand,
            MagickExportImagePixels(&wand, 0, static_cast<ssize_t>(y), width, 1,
                                    "RGB", QuantumPixel, rgb.data()),
            std::format("导出 {} 像素失败", label), &monitor,
            cfg_.encode_timeout_minutes);
        !ok) {
      return std::unexpected{ok.error()};
    }

    constexpr double red_weight = 0.2126;
    constexpr double green_weight = 0.7152;
    constexpr double blue_weight = 0.0722;
    const double quantum_scale = 1.0 / static_cast<double>(QuantumRange);
    std::vector<double> luma(width);
    for (std::size_t x = 0; x < width; ++x) {
      const auto base = x * 3;
      const double r = static_cast<double>(rgb[base]) * quantum_scale;
      const double g = static_cast<double>(rgb[base + 1]) * quantum_scale;
      const double b = static_cast<double>(rgb[base + 2]) * quantum_scale;
      luma[x] = red_weight * r + green_weight * g + blue_weight * b;
    }
    return luma;
  }

  std::expected<double, std::string> compare_xpsnr(
      MagickWand& reference,
      const fs::path& candidate,
      magick_detail::OperationMonitor& monitor) const {
    if (auto message =
            magick_detail::interruption_message(monitor,
                                                cfg_.encode_timeout_minutes)) {
      return std::unexpected{*message};
    }

    auto decoded = magick_detail::WandPtr{NewMagickWand()};
    if (!decoded) {
      return std::unexpected{"无法创建用于质量比较的 MagickWand。"};
    }
    MagickSetProgressMonitor(decoded.get(), magick_detail::progress_monitor,
                             &monitor);

    const auto candidate_utf8 = path_to_utf8(candidate);
    if (auto ok = magick_detail::check(
            decoded.get(),
            MagickReadImage(decoded.get(), candidate_utf8.c_str()),
            "读取候选文件失败", &monitor, cfg_.encode_timeout_minutes);
        !ok) {
      return std::unexpected{ok.error()};
    }

    const auto reference_width = MagickGetImageWidth(&reference);
    const auto reference_height = MagickGetImageHeight(&reference);
    const auto candidate_width = MagickGetImageWidth(decoded.get());
    const auto candidate_height = MagickGetImageHeight(decoded.get());
    if (reference_width == 0 || reference_height == 0 ||
        reference_width != candidate_width ||
        reference_height != candidate_height) {
      return std::unexpected{
          "XPSNR 比较失败：候选图尺寸与参考图不一致。"};
    }

    // ImageMagick 没有内建 XPSNR。这里使用 XPSNR 的核心思想：
    // 以亮度平面为主，并按局部纹理活动度降低误差权重，让搜索偏向
    // “可感知质量/体积”而不是纯逐像素 PSNR。
    auto previous_reference =
        export_luma_row(reference, reference_width, 0, monitor, "参考图");
    if (!previous_reference) {
      return std::unexpected{previous_reference.error()};
    }
    auto current_reference = *previous_reference;
    auto next_reference =
        reference_height > 1
            ? export_luma_row(reference, reference_width, 1, monitor, "参考图")
            : std::expected<std::vector<double>, std::string>{current_reference};
    if (!next_reference) {
      return std::unexpected{next_reference.error()};
    }

    double weighted_error = 0.0;
    double total_weight = 0.0;
    for (std::size_t y = 0; y < reference_height; ++y) {
      const auto candidate_luma =
          export_luma_row(*decoded, reference_width, y, monitor, "候选图");
      if (!candidate_luma) {
        return std::unexpected{candidate_luma.error()};
      }

      const auto& up = y == 0 ? current_reference : *previous_reference;
      const auto& down =
          y + 1 >= reference_height ? current_reference : *next_reference;
      for (std::size_t x = 0; x < reference_width; ++x) {
        const double center = current_reference[x];
        const double left = x == 0 ? center : current_reference[x - 1];
        const double right =
            x + 1 >= reference_width ? center : current_reference[x + 1];
        const double activity =
            (std::abs(center - left) + std::abs(center - right) +
             std::abs(center - up[x]) + std::abs(center - down[x])) /
            4.0;
        const double weight = 1.0 / (1.0 + 24.0 * activity);
        const double error = center - (*candidate_luma)[x];
        weighted_error += error * error * weight;
        total_weight += weight;
      }

      if (y + 1 < reference_height) {
        previous_reference = std::move(current_reference);
        current_reference = std::move(*next_reference);
        if (y + 2 < reference_height) {
          next_reference =
              export_luma_row(reference, reference_width, y + 2, monitor,
                              "参考图");
          if (!next_reference) {
            return std::unexpected{next_reference.error()};
          }
        } else {
          next_reference = current_reference;
        }
      }
    }

    if (total_weight <= 0.0) {
      return std::unexpected{"XPSNR 比较失败：没有可比较的像素。"};
    }

    const double mse = weighted_error / total_weight;
    if (mse <= std::numeric_limits<double>::epsilon()) {
      return 99.99;
    }
    return std::clamp(10.0 * std::log10(1.0 / mse), 0.0, 99.99);
  }

  std::expected<CandidateResult, std::string> encode_candidate(
      MagickWand& reference,
      const fs::path& path,
      int quality,
      magick_detail::OperationMonitor& monitor,
      std::size_t image_index) const {
    auto candidate = magick_detail::WandPtr{CloneMagickWand(&reference)};
    if (!candidate) {
      return std::unexpected{"无法克隆用于自动搜索的 MagickWand。"};
    }
    MagickSetProgressMonitor(candidate.get(), magick_detail::progress_monitor,
                             &monitor);
    MagickSetImageProgressMonitor(candidate.get(),
                                  magick_detail::progress_monitor, &monitor);

    if (auto ok = write_direct_output(*candidate, path, quality, monitor, true,
                                      image_index);
        !ok) {
      return std::unexpected{ok.error()};
    }

    std::error_code ec;
    const auto bytes = fs::file_size(path, ec);
    if (ec || bytes == 0) {
      return std::unexpected{
          std::format("候选文件无效: {}", path_to_utf8(path))};
    }

    const auto xpsnr = compare_xpsnr(reference, path, monitor);
    if (!xpsnr) {
      return std::unexpected{xpsnr.error()};
    }

    return CandidateResult{.quality = quality,
                           .path = path,
                           .bytes = bytes,
                           .xpsnr = *xpsnr};
  }

  std::expected<CandidateResult, std::string> write_optimized_output(
      MagickWand& wand,
      const fs::path& output,
      magick_detail::OperationMonitor& monitor,
      const ImageFile& image) const {
    constexpr double metric_margin_db = 0.05;
    const int min_quality =
        std::clamp(std::min(cfg_.optimize_min_quality, cfg_.quality), 1,
                   cfg_.quality);
    const int max_quality = std::clamp(cfg_.quality, 1, 100);
    const double requested_target =
        std::clamp(cfg_.optimize_target_xpsnr, 0.0, 120.0);
    const double target =
        std::clamp(requested_target + metric_margin_db, 0.0, 120.0);

    std::vector<CandidateResult> candidates;
    CandidateFileCleanup candidate_cleanup;

    auto try_quality =
        [&](int quality) -> std::expected<CandidateResult, std::string> {
      quality = std::clamp(quality, min_quality, max_quality);
      for (const auto& candidate : candidates) {
        if (candidate.quality == quality) {
          return candidate;
        }
      }
      auto path = candidate_path(output, quality, image.index);
      if (!path) {
        return std::unexpected{path.error()};
      }
      candidate_cleanup.track(*path);
      auto candidate = encode_candidate(wand, *path, quality,
                                        monitor, image.index);
      if (!candidate) {
        return std::unexpected{candidate.error()};
      }
      logger_.info(std::format(
          "optimize candidate: {} q{} {} XPSNR {:.2f} dB",
          path_to_utf8(output.filename()), candidate->quality,
          format_size(candidate->bytes), candidate->xpsnr));
      candidates.push_back(*candidate);
      return *candidate;
    };

    auto best = try_quality(max_quality);
    if (!best) {
      return std::unexpected{best.error()};
    }

    if (best->xpsnr >= target) {
      int low = min_quality;
      int high = max_quality - 1;
      int lowest_passing = best->quality;

      while (low <= high) {
        const int mid = low + (high - low) / 2;
        auto candidate = try_quality(mid);
        if (!candidate) {
          return std::unexpected{candidate.error()};
        }
        if (candidate->xpsnr >= target) {
          lowest_passing = mid;
          high = mid - 1;
        } else {
          low = mid + 1;
        }
      }

      for (int q = std::max(min_quality, lowest_passing - 2);
           q <= std::min(max_quality, lowest_passing + 2); ++q) {
        auto candidate = try_quality(q);
        if (!candidate) {
          return std::unexpected{candidate.error()};
        }
      }
    }

    auto selected = candidates.front();
    bool found_target = false;
    for (const auto& candidate : candidates) {
      const bool meets_target = candidate.xpsnr >= target;
      if (meets_target &&
          (!found_target || candidate.bytes < selected.bytes ||
           (candidate.bytes == selected.bytes &&
            candidate.quality < selected.quality))) {
        selected = candidate;
        found_target = true;
      } else if (!found_target &&
                 (candidate.xpsnr > selected.xpsnr ||
                  (candidate.xpsnr == selected.xpsnr &&
                   candidate.bytes < selected.bytes))) {
        selected = candidate;
      }
    }

    if (auto ok = replace_with_candidate(selected.path, output); !ok) {
      return std::unexpected{ok.error()};
    }
    candidate_cleanup.release(selected.path);

    if (!found_target) {
      logger_.info(std::format(
          "optimize target not reached: {} best q{} XPSNR {:.2f} dB, target {:.2f} dB",
          path_to_utf8(output.filename()), selected.quality, selected.xpsnr,
          requested_target));
    }
    logger_.info(std::format(
        "optimize selected: {} q{} {} XPSNR {:.2f} dB target {:.2f} dB",
        path_to_utf8(output.filename()), selected.quality,
        format_size(selected.bytes), selected.xpsnr, requested_target));
    return selected;
  }

  std::expected<int, std::string> write_output(
      MagickWand& wand,
      const fs::path& output,
      magick_detail::OperationMonitor& monitor,
      const ImageFile& image) const {
    if (!cfg_.optimize_output) {
      if (auto ok = write_direct_output(wand, output, cfg_.quality, monitor,
                                        false, image.index);
          !ok) {
        return std::unexpected{ok.error()};
      }
      return cfg_.quality;
    }

    const auto selected = write_optimized_output(wand, output, monitor, image);
    if (!selected) {
      return std::unexpected{selected.error()};
    }
    return selected->quality;
  }

  std::string command_description(const ImageFile& image,
                                  const fs::path& output,
                                  int selected_quality) const {
    std::string text =
        std::format("MagickWand {}: {} -> {} -quality {}",
                    output_format_name(cfg_.output_format),
                    path_to_utf8(image.path), path_to_utf8(output),
                    selected_quality);
    if (cfg_.output_format == OutputFormat::avif && cfg_.magick_speed) {
      text += std::format(" -define heic:speed={}", *cfg_.magick_speed);
    }
    if (cfg_.bit_depth) {
      text += std::format(" -depth {}", *cfg_.bit_depth);
    }
    if (cfg_.output_format == OutputFormat::avif &&
        cfg_.chroma_mode != ChromaMode::auto_keep) {
      text += std::format(" -define heic:chroma={}",
                          chroma_mode_name(cfg_.chroma_mode));
    }
    if (cfg_.optimize_output) {
      text += std::format(" --optimize --target-xpsnr {:.2f}",
                          cfg_.optimize_target_xpsnr);
    }
    for (const auto& define : cfg_.magick_defines) {
      text += std::format(" -define {}",
                          magick_detail::describe_define(utf8_from_wide(define)));
    }
    if (cfg_.max_resolution > 0) {
      text += std::format(" -resize {}x{}>", cfg_.max_resolution,
                          cfg_.max_resolution);
    }
    if (cfg_.strip_metadata) {
      text += " -strip";
    }
    return text;
  }

  const AppConfig& cfg_;
  const MagickRuntime& runtime_;
  FileLogger& logger_;
};

}  // namespace avif
