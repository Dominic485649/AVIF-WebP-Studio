module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>

export module awj.native_backend;

import awj.avif_aom_codec;
import awj.avif_registry;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.image;
import awj.jxl_codec;
import awj.large_image_plan;
import awj.native_visual_search;
import awj.raw_image_io;
import awj.resource_planner;
import awj.svtav1hdr_codec;
import awj.visual_quality;
import awj.webp_codec;

export namespace awj {

namespace native_backend_detail {

class UnsupportedDecoder final : public ImageDecoder {
 public:
  explicit UnsupportedDecoder(std::string id) : id_{std::move(id)} {}

  [[nodiscard]] std::string_view id() const noexcept override { return id_; }
  [[nodiscard]] bool can_decode(const fs::path&) const override { return false; }
  std::expected<ImageDecodeResult, std::string> decode(const fs::path&) const override {
    return std::unexpected{std::format("native backend 当前不支持该输入解码器: {}", id_)};
  }

 private:
  std::string id_;
};

std::unique_ptr<ImageDecoder> decoder_for_output_format(OutputFormat format) {
  switch (format) {
    case OutputFormat::webp:
      return std::make_unique<WebPImageDecoder>();
    case OutputFormat::jxl:
      return std::make_unique<JXLImageDecoder>();
    case OutputFormat::avif:
      return std::make_unique<AvifImageDecoder>();
    default:
      return std::make_unique<UnsupportedDecoder>("avif");
  }
}

std::unique_ptr<ImageEncoder> encoder_for_output_format(OutputFormat format,
                                                        AvifEncoderMode avif_encoder) {
  switch (format) {
    case OutputFormat::webp:
      return std::make_unique<WebPImageEncoder>();
    case OutputFormat::jxl:
      return std::make_unique<JXLImageEncoder>();
    case OutputFormat::avif:
      if (avif_encoder == AvifEncoderMode::zenrav1e) {
        return std::make_unique<ZenravifImageEncoder>();
      }
      if (avif_encoder == AvifEncoderMode::svt) {
        return nullptr;
      }
      return std::make_unique<AvifLibavifImageEncoder>(avif_encoder);
    default:
      return nullptr;
  }
}

struct HandleDeleter {
  using pointer = HANDLE;
  void operator()(HANDLE value) const noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

std::string sanitize_stderr_message(std::string text) {
  text.erase(std::ranges::remove(text, '\r').begin(), text.end());
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  constexpr std::size_t max_length = 4096;
  if (text.size() > max_length) {
    text.resize(max_length);
    text += "...";
  }
  return text;
}

std::string read_pipe_available(HANDLE pipe) {
  std::string text;
  std::array<char, 4096> buffer{};
  while (true) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
      break;
    }
    DWORD bytes_read = 0;
    const DWORD to_read = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
    if (!ReadFile(pipe, buffer.data(), to_read, &bytes_read, nullptr) || bytes_read == 0) {
      break;
    }
    text.append(buffer.data(), bytes_read);
  }
  return text;
}

class TempFileCleanup {
 public:
  TempFileCleanup(fs::path input, fs::path output)
      : input_{std::move(input)}, output_{std::move(output)} {}
  ~TempFileCleanup() { cleanup(); }
  TempFileCleanup(const TempFileCleanup&) = delete;
  TempFileCleanup& operator=(const TempFileCleanup&) = delete;
  void cleanup() noexcept {
    std::error_code ec;
    if (!input_.empty()) {
      fs::remove(input_, ec);
    }
    ec.clear();
    if (!output_.empty()) {
      fs::remove(output_, ec);
    }
  }

 private:
  fs::path input_;
  fs::path output_;
};

class TempOutputFile {
 public:
  explicit TempOutputFile(fs::path path) : path_{std::move(path)} {}
  ~TempOutputFile() {
    if (active_) {
      std::error_code ec;
      fs::remove(path_, ec);
    }
  }
  TempOutputFile(const TempOutputFile&) = delete;
  TempOutputFile& operator=(const TempOutputFile&) = delete;
  [[nodiscard]] const fs::path& path() const noexcept { return path_; }
  void release() noexcept { active_ = false; }

 private:
  fs::path path_;
  bool active_{true};
};

std::expected<std::vector<std::byte>, std::string> read_file_bytes(const fs::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{std::format("无法读取 helper 输出文件: {}", path_to_utf8(path))};
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0) {
    return std::unexpected{std::format("helper 输出文件为空: {}", path_to_utf8(path))};
  }
  const auto file_size = static_cast<std::uint64_t>(size);
  if (file_size > encoding_defaults::max_input_file_bytes) {
    return std::unexpected{std::format("helper 输出文件超过 20 GiB 输入上限: {}", path_to_utf8(path))};
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) {
    return std::unexpected{std::format("读取 helper 输出文件失败: {}", path_to_utf8(path))};
  }
  return bytes;
}

std::wstring quote_process_argument(std::wstring_view value) {
  std::wstring quoted{L"\""};
  for (const wchar_t ch : value) {
    if (ch == L'\"') {
      quoted += L"\\\"";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted += L"\"";
  return quoted;
}

std::wstring helper_encoder_argument(AvifEncoderMode mode) {
  return wide_from_utf8(avif_encoder_mode_name(mode));
}

std::wstring helper_chroma_argument(ChromaMode mode) {
  return wide_from_utf8(chroma_mode_name(mode));
}

bool avif_lossless_requested(const AppConfig& cfg) noexcept {
  return cfg.output_format == OutputFormat::avif &&
         (cfg.quality >= 100 || cfg.visual_quality == 100);
}

bool encode_settings_lossless_requested(const NativeEncodeSettings& settings) noexcept {
  return settings.quality >= 100 || settings.visual_quality == 100;
}

bool avif_lossless_passthrough_source(const fs::path& path) {
  static constexpr std::wstring_view avif_extensions[] = {L".avif"};
  return decoder_common::extension_is_one_of(path, avif_extensions);
}

ChromaMode chroma_from_source_pixel_format(PixelFormat pixel_format) noexcept {
  switch (pixel_format) {
    case PixelFormat::yuv420:
      return ChromaMode::yuv420;
    case PixelFormat::yuv422:
      return ChromaMode::yuv422;
    case PixelFormat::yuv444:
      return ChromaMode::yuv444;
    case PixelFormat::gray:
    case PixelFormat::rgb:
    case PixelFormat::rgba:
    case PixelFormat::unknown:
    default:
      return ChromaMode::auto_keep;
  }
}

std::string alpha_mode_name(AlphaMode mode) {
  switch (mode) {
    case AlphaMode::straight:
      return "straight";
    case AlphaMode::premultiplied:
      return "premultiplied";
    case AlphaMode::none:
    default:
      return "none";
  }
}

bool image_has_metadata(const ImageBuffer& image, MetadataKind kind) noexcept {
  return std::ranges::find_if(image.metadata, [kind](const MetadataBlock& block) {
           return block.kind == kind;
         }) != image.metadata.end();
}

std::string source_chroma_name(const ImageBuffer& image) {
  if (!image.source_info) {
    return "unknown";
  }
  const auto chroma = chroma_from_source_pixel_format(image.source_info->pixel_format);
  return chroma == ChromaMode::auto_keep ? "unknown" : chroma_mode_name(chroma);
}

ChromaMode lossless_source_chroma(const ImageBuffer& image) noexcept {
  if (image.source_info) {
    return chroma_from_source_pixel_format(image.source_info->pixel_format);
  }
  return ChromaMode::auto_keep;
}

std::optional<int> lossless_source_bit_depth(const ImageBuffer& image) noexcept {
  if (image.source_info && image.source_info->bit_depth > 0) {
    return image.source_info->bit_depth;
  }
  return image.bit_depth;
}

std::optional<int> source_bit_depth(const ImageBuffer& image) noexcept {
  if (image.source_info && image.source_info->bit_depth > 0) {
    return image.source_info->bit_depth;
  }
  return image.bit_depth > 0 ? std::optional<int>{image.bit_depth} : std::nullopt;
}

std::optional<int> choose_color_value(std::optional<int> user_value,
                                      std::optional<int> source_value) noexcept {
  return user_value ? user_value : source_value;
}

bool has_user_color_settings(const AppConfig& cfg) noexcept {
  return cfg.color_primaries || cfg.transfer_characteristics || cfg.matrix_coefficients ||
         cfg.color_range || !cfg.mastering_display.empty() || !cfg.content_light.empty();
}

bool encoder_supports_alpha(AvifEncoderMode mode) noexcept {
  return mode == AvifEncoderMode::aom || mode == AvifEncoderMode::zenrav1e;
}

bool alpha_must_be_preserved(AlphaModePolicy policy,
                             bool source_has_alpha_channel) noexcept {
  return policy == AlphaModePolicy::force && source_has_alpha_channel;
}

std::string applied_alpha_name(bool source_has_alpha_channel,
                               bool preserve_alpha,
                               bool supports_alpha) {
  if (!source_has_alpha_channel) {
    return "none";
  }
  if (preserve_alpha && supports_alpha) {
    return "kept";
  }
  return "stripped";
}

void populate_source_diagnostics(NativeEncodeSettings& settings,
                                 const ImageBuffer& image,
                                 AvifEncoderMode user_encoder) {
  settings.user_encoder_id = avif_encoder_mode_name(user_encoder);
  settings.user_chroma = chroma_mode_name(settings.requested_chroma_mode);
  settings.source_chroma = source_chroma_name(image);
  settings.source_bit_depth = source_bit_depth(image);
  settings.alpha_policy_name = alpha_mode_policy_name(settings.requested_alpha_policy);
  settings.source_has_alpha_channel = image.alpha_mode != AlphaMode::none;
  settings.source_alpha_mode = alpha_mode_name(image.alpha_mode);
  settings.source_has_icc = image_has_metadata(image, MetadataKind::icc);
  settings.source_has_hdr_metadata = image.source_info ? image.source_info->has_hdr_metadata : false;
  if (image.source_info) {
    settings.source_color_primaries = image.source_info->color_primaries;
    settings.source_transfer_characteristics = image.source_info->transfer_characteristics;
    settings.source_matrix_coefficients = image.source_info->matrix_coefficients;
    settings.source_color_range = image.source_info->color_range;
    if (!image.source_info->color_metadata_source.empty()) {
      settings.color_metadata_source = image.source_info->color_metadata_source;
    }
  }
}

void populate_color_decision(NativeEncodeSettings& settings, const AppConfig& cfg) {
  settings.applied_color_primaries = choose_color_value(cfg.color_primaries,
                                                        settings.source_color_primaries);
  settings.applied_transfer_characteristics = choose_color_value(cfg.transfer_characteristics,
                                                                 settings.source_transfer_characteristics);
  settings.applied_matrix_coefficients = choose_color_value(cfg.matrix_coefficients,
                                                            settings.source_matrix_coefficients);
  settings.applied_color_range = choose_color_value(cfg.color_range,
                                                    settings.source_color_range);
  settings.svtav1hdr.color_primaries = settings.applied_color_primaries;
  settings.svtav1hdr.transfer_characteristics = settings.applied_transfer_characteristics;
  settings.svtav1hdr.matrix_coefficients = settings.applied_matrix_coefficients;
  settings.svtav1hdr.color_range = settings.applied_color_range;

  if (cfg.strip_metadata) {
    settings.applied_icc = settings.source_has_icc ? "stripped" : "none";
    settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? "stripped" : "none";
    settings.color_metadata_source = "stripped";
    settings.color_reason = "metadata stripped by user request";
    return;
  }
  settings.applied_icc = settings.source_has_icc ? "not-written" : "none";
  settings.applied_hdr_metadata = settings.source_has_hdr_metadata ? "not-written" : "none";
  if (has_user_color_settings(cfg)) {
    settings.color_metadata_source = "user-svt-settings";
    settings.color_reason = "user color/HDR settings override source metadata";
  } else if (settings.applied_color_primaries || settings.applied_transfer_characteristics ||
             settings.applied_matrix_coefficients || settings.applied_color_range) {
    if (settings.color_metadata_source.empty()) {
      settings.color_metadata_source = "source-cicp";
    }
    settings.color_reason = "source CICP fields selected for encoder settings";
  } else if (settings.source_has_icc) {
    settings.color_metadata_source = "source-icc";
    settings.color_reason = "source ICC detected but current AVIF path does not write ICC yet";
  } else {
    settings.color_metadata_source = "encoder-default";
    settings.color_reason = "source color metadata unknown; encoder defaults apply";
  }
}

bool avif_lossless_bit_depth_supported(int bit_depth) noexcept {
  return bit_depth == 8 || bit_depth == 10 || bit_depth == 12;
}

std::wstring make_helper_command_line(const fs::path& helper,
                                      const fs::path& input,
                                      const fs::path& output,
                                      const NativeEncodeSettings& settings) {
  std::wstring command = quote_process_argument(helper.native());
  const auto append = [&](std::wstring_view value) {
    command.push_back(L' ');
    command += value;
  };
  append(L"--mode");
  append(L"encode");
  append(L"--encoder");
  append(helper_encoder_argument(settings.avif_encoder));
  append(L"--input");
  append(quote_process_argument(input.native()));
  append(L"--output");
  append(quote_process_argument(output.native()));
  append(L"--quality");
  append(std::format(L"{}", settings.quality));
  append(L"--speed");
  append(std::format(L"{}", settings.speed));
  if (!encode_settings_lossless_requested(settings)) {
    append(L"--bit-depth");
    append(std::format(L"{}", settings.bit_depth.value_or(8)));
    append(L"--chroma");
    append(helper_chroma_argument(settings.chroma_mode));
  }
  append(L"--threads");
  append(std::format(L"{}", std::max(1, settings.resources.encoder_threads_per_file)));
  if (settings.avif_encoder == AvifEncoderMode::zenrav1e) {
    append(L"--experimental-encoders");
  }
  return command;
}

std::expected<void, std::string> run_helper_command(std::wstring command_line,
                                                     std::chrono::minutes timeout,
                                                     std::stop_token stop_token) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE raw_read = nullptr;
  HANDLE raw_write = nullptr;
  if (!CreatePipe(&raw_read, &raw_write, &security, 0)) {
    return std::unexpected{std::format("创建 AVIF helper 输出管道失败: {}", win32_error_message(GetLastError()))};
  }
  UniqueHandle read_pipe{raw_read};
  UniqueHandle write_pipe{raw_write};
  SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe.get();
  startup.hStdError = write_pipe.get();
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return std::unexpected{std::format("启动 AVIF helper 失败: {}", win32_error_message(GetLastError()))};
  }
  UniqueHandle process_handle{process.hProcess};
  UniqueHandle thread_handle{process.hThread};
  write_pipe.reset();

  const auto started = std::chrono::steady_clock::now();
  std::string diagnostics;
  while (true) {
    diagnostics += read_pipe_available(read_pipe.get());
    const DWORD wait = WaitForSingleObject(process_handle.get(), 50);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait != WAIT_TIMEOUT) {
      return std::unexpected{std::format("等待 AVIF helper 失败: {}", win32_error_message(GetLastError()))};
    }
    if (stop_token.stop_requested()) {
      TerminateProcess(process_handle.get(), 130);
      WaitForSingleObject(process_handle.get(), 1000);
      diagnostics += read_pipe_available(read_pipe.get());
      const auto message = sanitize_stderr_message(std::move(diagnostics));
      return std::unexpected{message.empty() ? "AVIF helper 已取消。" : std::format("AVIF helper 已取消: {}", message)};
    }
    if (timeout.count() > 0 && std::chrono::steady_clock::now() - started > timeout) {
      TerminateProcess(process_handle.get(), 124);
      WaitForSingleObject(process_handle.get(), 1000);
      diagnostics += read_pipe_available(read_pipe.get());
      const auto message = sanitize_stderr_message(std::move(diagnostics));
      return std::unexpected{message.empty() ? std::format("AVIF helper 超时（{} 分钟）。", timeout.count())
                                            : std::format("AVIF helper 超时（{} 分钟）: {}", timeout.count(), message)};
    }
  }

  diagnostics += read_pipe_available(read_pipe.get());
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
    return std::unexpected{std::format("读取 AVIF helper 退出码失败: {}", win32_error_message(GetLastError()))};
  }
  if (exit_code != 0) {
    const auto message = sanitize_stderr_message(std::move(diagnostics));
    return std::unexpected{message.empty() ? std::format("AVIF helper 编码失败，退出码 {}。", exit_code)
                                          : std::format("AVIF helper 编码失败，退出码 {}: {}", exit_code, message)};
  }
  return {};
}

std::atomic<std::uint64_t> helper_counter{};

fs::path helper_executable_path() {
  const auto exe_dir = executable_directory();
  const auto colocated = exe_dir / L"AWJ-native-avif-helper.exe";
  std::error_code ec;
  if (fs::exists(colocated, ec) && !ec) {
    return colocated;
  }
  const auto internal = exe_dir.parent_path() / L"internal" / exe_dir.filename() /
                        L"AWJ-native-avif-helper.exe";
  if (fs::exists(internal, ec) && !ec) {
    return internal;
  }
  return colocated;
}

std::expected<NativeEncodeResult, std::string> encode_with_helper_process(
    const ImageBuffer& image,
    const NativeEncodeSettings& settings,
    std::chrono::minutes timeout,
    std::stop_token stop_token) {
  if (encode_settings_lossless_requested(settings)) {
    return std::unexpected{
        "AVIF helper/raw RGBA 无损编码不能保证继承全部源图参数；请使用内置 AOM 无损路径。"};
  }
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 || image.planes.empty()) {
    return std::unexpected{"AVIF helper 当前需要 8-bit RGBA ImageBuffer。"};
  }
  const auto temp_root = fs::temp_directory_path() / L"awjimage-avif-helper";
  std::error_code ec;
  fs::create_directories(temp_root, ec);
  if (ec) {
    return std::unexpected{std::format("无法创建 AVIF helper 临时目录: {}", ec.message())};
  }
  const auto id = helper_counter.fetch_add(1);
  const auto input_path = temp_root / std::format(L"input-{}-{}.awsraw", GetCurrentProcessId(), id);
  const auto output_path = temp_root / std::format(L"output-{}-{}.avif", GetCurrentProcessId(), id);
  TempFileCleanup cleanup{input_path, output_path};
  if (auto written = write_raw_image_file(input_path, image); !written) {
    return std::unexpected{written.error()};
  }
  const auto helper = helper_executable_path();
  if (!fs::exists(helper, ec) || ec) {
    return std::unexpected{"AVIF helper 缺失；请重新安装或重新构建 AWJimage。"};
  }
  auto command_line = make_helper_command_line(helper, input_path, output_path, settings);
  if (auto ran = run_helper_command(std::move(command_line), timeout, stop_token); !ran) {
    return std::unexpected{ran.error()};
  }
  auto bytes = read_file_bytes(output_path);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }

  const auto speed_mapping = avif_speed_mapping_for(AvifEncoderSelection{
      .applied_encoder = settings.avif_encoder,
      .speed = settings.speed});
  return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(*bytes),
                                                    .codec_name = avif_encoder_mode_name(settings.avif_encoder)},
                            .diagnostics = EncodeDiagnostics{
                                .encoder_id = avif_encoder_mode_name(settings.avif_encoder),
                                .requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder),
                                .requested_chroma = chroma_mode_name(settings.requested_chroma_mode),
                                .applied_chroma = chroma_mode_name(settings.chroma_mode),
                                .requested_bit_depth = settings.requested_bit_depth,
                                .applied_bit_depth = settings.bit_depth,
                                .bit_depth_reason = settings.bit_depth_reason,
                                .fallback_reason = settings.encoder_fallback_reason,
                                .encoder_experimental = settings.avif_encoder == AvifEncoderMode::zenrav1e,
                                .encoder_license = settings.avif_encoder == AvifEncoderMode::zenrav1e
                                                       ? "AGPL-3.0-only OR LicenseRef-Imazen-Commercial"
                                                       : "BSD-2-Clause",
                                .speed_mapping = speed_mapping,
                                .encoder_threads = settings.resources.encoder_threads_per_file,
                                .memory_budget_bytes = settings.resources.memory_limit_bytes},
                            .final_quality = settings.quality,
                            .lossless = settings.quality >= 100,
                            .search_attempt_count = 1};
}

std::wstring collision_suffix(CollisionMode mode) {
  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  switch (mode) {
    case CollisionMode::suffix_time:
      return std::format(L"-{:%Y%m%d-%H%M%S}", now);
    case CollisionMode::suffix_random: {
      const auto value = helper_counter.fetch_add(1);
      return std::format(L"-{:08x}", static_cast<unsigned int>((value ^ GetTickCount64()) & 0xffffffffu));
    }
    case CollisionMode::overwrite:
    case CollisionMode::skip:
    default:
      return {};
  }
}

fs::path resolve_collision_output_path(const fs::path& planned,
                                       CollisionMode mode) {
  if (mode != CollisionMode::suffix_time && mode != CollisionMode::suffix_random) {
    return planned;
  }
  std::error_code ec;
  if (!fs::exists(planned, ec) && !ec) {
    return planned;
  }
  const auto parent = planned.parent_path();
  const auto stem = planned.stem().wstring();
  const auto extension = planned.extension().wstring();
  const auto suffix = collision_suffix(mode);
  const auto planned_key = normalized_lower_path_key(planned);
  for (int attempt = 0; attempt < 1000; ++attempt) {
    auto candidate = parent / (stem + suffix + (attempt == 0 ? std::wstring{} : std::format(L"-{}", attempt)) + extension);
    if (normalized_lower_path_key(candidate) == planned_key) {
      continue;
    }
    if (!fs::exists(candidate, ec) && !ec) {
      return candidate;
    }
    ec.clear();
  }
  return parent / (stem + suffix + std::format(L"-{}", GetCurrentProcessId()) + extension);
}

std::expected<void, std::string> write_output_bytes(const fs::path& path,
                                                    std::span<const std::byte> bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    return std::unexpected{std::format("无法创建输出目录 {}: {}",
                                       path_to_utf8(path.parent_path()), ec.message())};
  }
  const auto temp_path = path.parent_path() /
                         std::format(L"{}.tmp-{}-{}", path.filename().wstring(),
                                     GetCurrentProcessId(), helper_counter.fetch_add(1));
  if (normalized_lower_path_key(temp_path) == normalized_lower_path_key(path)) {
    return std::unexpected{std::format("临时输出路径与目标路径冲突: {}", path_to_utf8(path))};
  }
  TempOutputFile temp{temp_path};
  {
    std::ofstream output{temp.path(), std::ios::binary | std::ios::trunc};
    if (!output) {
      return std::unexpected{std::format("无法写入临时输出文件: {}", path_to_utf8(temp.path()))};
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      return std::unexpected{std::format("写入临时输出文件失败: {}", path_to_utf8(temp.path()))};
    }
  }
  if (!MoveFileExW(temp.path().c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return std::unexpected{std::format("替换输出文件失败 {}: {}", path_to_utf8(path),
                                       win32_error_message(GetLastError()))};
  }
  temp.release();
  return {};
}

NativeEncodeSettings settings_from_config(const AppConfig& cfg, ResourcePlan resources) {
  return NativeEncodeSettings{.output_format = cfg.output_format,
                              .quality = cfg.quality,
                              .visual_quality = cfg.visual_quality,
                              .speed = cfg.speed.value_or(default_speed_for(cfg.output_format)),
                              .speed_explicit = cfg.speed.has_value(),
                              .bit_depth = cfg.bit_depth,
                              .bit_depth_explicit = cfg.bit_depth.has_value(),
                              .chroma_mode = cfg.chroma_mode,
                              .avif_encoder = cfg.avif_encoder,
                              .alpha_policy = cfg.alpha_policy,
                              .requested_chroma_mode = cfg.chroma_mode,
                              .requested_avif_encoder = cfg.avif_encoder,
                              .requested_alpha_policy = cfg.alpha_policy,
                              .requested_bit_depth = cfg.bit_depth,
                              .strip_metadata = cfg.strip_metadata,
                              .visual_quality_fallback = cfg.visual_quality_fallback,
                              .jxl_jpeg_lossless_candidate = false,
                              .avif_tune_iq = encoding_defaults::default_avif_tune_iq,
                              .svtav1hdr = SvtAv1HdrSettings{.crf = cfg.svtav1hdr_crf,
                                                            .preset = cfg.svtav1hdr_preset.value_or(encoding_defaults::default_svtav1hdr_preset),
                                                            .tune = cfg.svtav1hdr_tune,
                                                            .keyint = cfg.svtav1hdr_keyint.value_or(encoding_defaults::default_svtav1hdr_keyint),
                                                            .avif = encoding_defaults::default_svtav1hdr_avif,
                                                            .params = cfg.svtav1hdr_params,
                                                            .color_primaries = cfg.color_primaries,
                                                            .transfer_characteristics = cfg.transfer_characteristics,
                                                            .matrix_coefficients = cfg.matrix_coefficients,
                                                            .color_range = cfg.color_range,
                                                            .mastering_display = cfg.mastering_display,
                                                            .content_light = cfg.content_light},
                              .resources = resources};
}

void copy_native_result(const NativeEncodeResult& native, EncodeResult& result) {
  result.output_bytes = native.encoded.bytes.size();
  result.final_encoder_quality = native.final_quality;
  result.search_attempt_count = native.search_attempt_count;
  result.lossless = native.lossless;
  result.speed = native.diagnostics.speed_mapping.user_speed;
  result.decoder_id = native.diagnostics.decoder_id;
  result.encoder_id = native.diagnostics.encoder_id;
  result.requested_encoder_id = native.diagnostics.requested_encoder_id;
  result.user_encoder_id = native.diagnostics.user_encoder_id;
  result.user_chroma = native.diagnostics.user_chroma;
  result.source_chroma = native.diagnostics.source_chroma;
  result.requested_chroma = native.diagnostics.requested_chroma;
  result.applied_chroma = native.diagnostics.applied_chroma;
  result.chroma_reason = native.diagnostics.chroma_reason;
  result.source_bit_depth = native.diagnostics.source_bit_depth;
  result.requested_bit_depth = native.diagnostics.requested_bit_depth;
  result.applied_bit_depth = native.diagnostics.applied_bit_depth;
  result.bit_depth_reason = native.diagnostics.bit_depth_reason;
  result.alpha_policy = native.diagnostics.alpha_policy;
  result.source_has_alpha_channel = native.diagnostics.source_has_alpha_channel;
  result.source_alpha_mode = native.diagnostics.source_alpha_mode;
  result.has_non_opaque_alpha = native.diagnostics.has_non_opaque_alpha;
  result.encoder_supports_alpha = native.diagnostics.encoder_supports_alpha;
  result.applied_alpha = native.diagnostics.applied_alpha;
  result.alpha_reason = native.diagnostics.alpha_reason;
  result.source_color_primaries = native.diagnostics.source_color_primaries;
  result.source_transfer_characteristics = native.diagnostics.source_transfer_characteristics;
  result.source_matrix_coefficients = native.diagnostics.source_matrix_coefficients;
  result.source_color_range = native.diagnostics.source_color_range;
  result.applied_color_primaries = native.diagnostics.applied_color_primaries;
  result.applied_transfer_characteristics = native.diagnostics.applied_transfer_characteristics;
  result.applied_matrix_coefficients = native.diagnostics.applied_matrix_coefficients;
  result.applied_color_range = native.diagnostics.applied_color_range;
  result.source_has_icc = native.diagnostics.source_has_icc;
  result.applied_icc = native.diagnostics.applied_icc;
  result.source_has_hdr_metadata = native.diagnostics.source_has_hdr_metadata;
  result.applied_hdr_metadata = native.diagnostics.applied_hdr_metadata;
  result.color_metadata_source = native.diagnostics.color_metadata_source;
  result.color_reason = native.diagnostics.color_reason;
  result.fallback_reason = native.diagnostics.fallback_reason;
  result.used_decoder_fallback = native.diagnostics.used_decoder_fallback;
  result.encoder_experimental = native.diagnostics.encoder_experimental;
  result.encoder_license = native.diagnostics.encoder_license;
  result.integration_mode = native.diagnostics.integration_mode;
  result.svtav1hdr_helper_path = native.diagnostics.svtav1hdr_helper_path;
  result.svtav1hdr_crf = native.diagnostics.svtav1hdr_crf;
  result.svtav1hdr_preset = native.diagnostics.svtav1hdr_preset;
  result.svtav1hdr_tune = native.diagnostics.svtav1hdr_tune;
  result.svtav1hdr_keyint = native.diagnostics.svtav1hdr_keyint;
  result.svtav1hdr_hdr_metadata = native.diagnostics.svtav1hdr_hdr_metadata;
  result.svtav1hdr_note = native.diagnostics.svtav1hdr_note;
  result.speed_parameter_kind = native.diagnostics.speed_mapping.codec_key;
  result.applied_speed = native.diagnostics.speed_mapping.codec_value;
  result.encoder_threads = native.diagnostics.encoder_threads;
  result.memory_budget_bytes = native.diagnostics.memory_budget_bytes;
  if (native.visual_score) {
    result.visual_score = native.visual_score->visual_score;
    result.gmsd_quality_score = native.visual_score->gmsd_quality_score;
    result.msssim_quality_score = native.visual_score->msssim_quality_score;
  }
  result.raw_gmsd = native.raw_gmsd;
  result.raw_ms_ssim = native.raw_ms_ssim;
  result.gmsd_weight = GMSD_WEIGHT;
  result.msssim_weight = MSSSIM_WEIGHT;
  result.command = std::format("native:{}:{} q{} speed={}",
                               result.decoder_id,
                               native.diagnostics.encoder_id,
                               native.final_quality,
                               native.diagnostics.speed_mapping.user_speed);
}

void copy_settings_diagnostics(const NativeEncodeSettings& settings, EncodeResult& result) {
  const auto diagnostics = diagnostics_from_settings(settings);
  result.encoder_id = avif_encoder_mode_name(settings.avif_encoder);
  result.requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder);
  result.user_encoder_id = diagnostics.user_encoder_id;
  result.user_chroma = diagnostics.user_chroma;
  result.source_chroma = diagnostics.source_chroma;
  result.requested_chroma = chroma_mode_name(settings.requested_chroma_mode);
  result.applied_chroma = chroma_mode_name(settings.chroma_mode);
  result.chroma_reason = diagnostics.chroma_reason;
  result.source_bit_depth = diagnostics.source_bit_depth;
  result.requested_bit_depth = settings.requested_bit_depth;
  result.applied_bit_depth = settings.bit_depth;
  result.bit_depth_reason = settings.bit_depth_reason;
  result.alpha_policy = diagnostics.alpha_policy;
  result.source_has_alpha_channel = diagnostics.source_has_alpha_channel;
  result.source_alpha_mode = diagnostics.source_alpha_mode;
  result.has_non_opaque_alpha = diagnostics.has_non_opaque_alpha;
  result.encoder_supports_alpha = diagnostics.encoder_supports_alpha;
  result.applied_alpha = diagnostics.applied_alpha;
  result.alpha_reason = diagnostics.alpha_reason;
  result.source_color_primaries = diagnostics.source_color_primaries;
  result.source_transfer_characteristics = diagnostics.source_transfer_characteristics;
  result.source_matrix_coefficients = diagnostics.source_matrix_coefficients;
  result.source_color_range = diagnostics.source_color_range;
  result.applied_color_primaries = diagnostics.applied_color_primaries;
  result.applied_transfer_characteristics = diagnostics.applied_transfer_characteristics;
  result.applied_matrix_coefficients = diagnostics.applied_matrix_coefficients;
  result.applied_color_range = diagnostics.applied_color_range;
  result.source_has_icc = diagnostics.source_has_icc;
  result.applied_icc = diagnostics.applied_icc;
  result.source_has_hdr_metadata = diagnostics.source_has_hdr_metadata;
  result.applied_hdr_metadata = diagnostics.applied_hdr_metadata;
  result.color_metadata_source = diagnostics.color_metadata_source;
  result.color_reason = diagnostics.color_reason;
  result.fallback_reason = settings.encoder_fallback_reason;
  result.speed = settings.speed;
  result.encoder_threads = settings.resources.encoder_threads_per_file;
  result.memory_budget_bytes = settings.resources.memory_limit_bytes;
}

}  // namespace native_backend_detail

export class NativeBackend final {
  struct EncodeOverrides {
    std::optional<AvifEncoderMode> avif_encoder{};
    std::optional<GridPlan> avif_grid_plan{};
  };

 public:
  NativeBackend(const AppConfig& cfg, FileLogger& logger, ResourcePlan resources)
      : cfg_{cfg}, logger_{logger}, resources_{resources} {}

  EncodeResult encode(const ImageFile& image,
                      std::stop_token stop_token = {}) const {
    return encode_with_overrides(image, EncodeOverrides{}, stop_token);
  }

  EncodeResult encode_avif_grid(const ImageFile& image,
                                GridPlan plan,
                                std::stop_token stop_token = {}) const {
    EncodeOverrides overrides{};
    overrides.avif_encoder = AvifEncoderMode::aom;
    overrides.avif_grid_plan = std::move(plan);
    return encode_with_overrides(image, std::move(overrides), stop_token);
  }

  EncodeResult encode_avif_zenrav1e(const ImageFile& image,
                                    std::stop_token stop_token = {}) const {
    EncodeOverrides overrides{};
    overrides.avif_encoder = AvifEncoderMode::zenrav1e;
    return encode_with_overrides(image, std::move(overrides), stop_token);
  }

 private:
  using EncodeStartedAt = std::chrono::steady_clock::time_point;

  struct DecodedInput {
    ImageDecodeResult decoded{};
    bool decoder_used_fallback{};
  };

  struct PreparedEncoding {
    NativeEncodeSettings settings{};
    std::unique_ptr<ImageEncoder> encoder{};
    std::string avif_bit_depth_reason{};
  };

  struct PrepareError {
    std::string message{};
    NativeEncodeSettings settings{};
  };

  static std::unexpected<PrepareError> prepare_failed(std::string message,
                                                       const NativeEncodeSettings& settings) {
    return std::unexpected{PrepareError{.message = std::move(message), .settings = settings}};
  }

  [[nodiscard]] EncodeResult initialize_result(const ImageFile& image) const {
    return EncodeResult{.index = image.index,
                        .input_path = image.path,
                        .output_path = native_backend_detail::resolve_collision_output_path(
                            output_path_for(cfg_, image), cfg_.collision_mode),
                        .original_bytes = image.bytes,
                        .quality = cfg_.quality,
                        .requested_visual_quality = cfg_.visual_quality,
                        .gmsd_weight = GMSD_WEIGHT,
                        .msssim_weight = MSSSIM_WEIGHT,
                        .final_encoder_quality = cfg_.quality,
                        .speed = cfg_.speed.value_or(default_speed_for(cfg_.output_format)),
                        .quality_overridden_by_visual_quality = cfg_.visual_quality.has_value()};
  }

  static void mark_failed(EncodeResult& result, std::string message) {
    result.processed = true;
    result.message = std::move(message);
  }

  static bool cancel_if_requested(EncodeResult& result, std::stop_token stop_token) {
    if (!stop_token.stop_requested()) {
      return false;
    }
    result.canceled = true;
    result.message = "任务已取消。";
    return true;
  }

  [[nodiscard]] std::optional<EncodeResult> try_avif_lossless_passthrough(
      const ImageFile& image,
      const EncodeResult& base_result,
      EncodeStartedAt started,
      std::stop_token stop_token) const {
    if (!native_backend_detail::avif_lossless_requested(cfg_) ||
        !native_backend_detail::avif_lossless_passthrough_source(image.path)) {
      return std::nullopt;
    }

    auto result = base_result;
    auto probe = AvifImageDecoder{}.probe_dimensions(image.path);
    if (!probe) {
      mark_failed(result, probe.error());
      return result;
    }
    auto bytes = decoder_common::read_file_bytes(image.path, "AVIF");
    if (!bytes) {
      mark_failed(result, bytes.error());
      return result;
    }
    if (cancel_if_requested(result, stop_token)) {
      return result;
    }
    if (auto written = native_backend_detail::write_output_bytes(
            result.output_path, std::span<const std::byte>{*bytes});
        !written) {
      mark_failed(result, written.error());
      return result;
    }

    result.output_bytes = bytes->size();
    result.final_encoder_quality = 100;
    result.search_attempt_count = 1;
    result.lossless = true;
    result.decoder_id = "libavif-parse";
    result.encoder_id = "avif-passthrough";
    result.requested_encoder_id = avif_encoder_mode_name(cfg_.avif_encoder);
    result.integration_mode = "avif-lossless-passthrough";
    result.encoder_threads = 1;
    result.memory_budget_bytes = resources_.memory_limit_bytes;
    result.command = "native:avif-passthrough lossless";
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    result.processed = true;
    result.ok = true;
    result.message = "OK";
    logger_.info(std::format("native avif lossless passthrough ok: {} -> {}",
                             path_to_utf8(result.input_path.filename()),
                             path_to_utf8(result.output_path.filename())));
    return result;
  }

  [[nodiscard]] std::expected<DecodedInput, std::string> decode_input(
      const ImageFile& image) const {
    auto decoder_selection = select_decoder_for_path(
        image.path, DecoderRegistryOptions{.allow_wic_fallback = cfg_.allow_wic_fallback});
    if (!decoder_selection) {
      return std::unexpected{decoder_selection.error()};
    }

    auto decoded = decoder_selection->decoder->decode(image.path);
    if (!decoded) {
      return std::unexpected{decoded.error()};
    }

    const auto decoder_used_fallback = decoded->used_fallback || decoder_selection->fallback;
    return DecodedInput{.decoded = std::move(*decoded),
                        .decoder_used_fallback = decoder_used_fallback};
  }

  [[nodiscard]] std::expected<PreparedEncoding, PrepareError> prepare_encoding(
      const ImageFile& image,
      const ImageDecodeResult& decoded,
      EncodeOverrides overrides) const {
    PreparedEncoding prepared{};
    prepared.settings = native_backend_detail::settings_from_config(cfg_, resources_);
    const auto requested_avif_encoder = overrides.avif_encoder.value_or(cfg_.avif_encoder);
    prepared.settings.requested_avif_encoder = requested_avif_encoder;
    prepared.settings.requested_chroma_mode = cfg_.chroma_mode;
    prepared.settings.requested_alpha_policy = cfg_.alpha_policy;
    prepared.settings.requested_bit_depth = cfg_.bit_depth;
    if (overrides.avif_grid_plan) {
      prepared.settings.avif_grid_plan = std::move(overrides.avif_grid_plan);
    }
    if (cfg_.output_format == OutputFormat::jxl) {
      static constexpr std::wstring_view jpeg_extensions[] = {L".jpg", L".jpeg"};
      prepared.settings.jxl_jpeg_lossless_candidate =
          decoder_common::extension_is_one_of(image.path, jpeg_extensions);
    }

    if (cfg_.output_format == OutputFormat::avif) {
      native_backend_detail::populate_source_diagnostics(prepared.settings,
                                                         decoded.image,
                                                         requested_avif_encoder);
      native_backend_detail::populate_color_decision(prepared.settings, cfg_);
      if (decoded.image.width > std::numeric_limits<std::uint64_t>::max() / decoded.image.height) {
        return prepare_failed("AVIF encoder 输入尺寸过大。", prepared.settings);
      }
      const auto pixel_count = static_cast<std::uint64_t>(decoded.image.width) *
                               static_cast<std::uint64_t>(decoded.image.height);
      const auto has_non_opaque_alpha = decoder_common::has_non_opaque_alpha(decoded.image,
                                                                             "AVIF encoder");
      if (!has_non_opaque_alpha) {
        return prepare_failed(has_non_opaque_alpha.error(), prepared.settings);
      }
      prepared.settings.has_non_opaque_alpha = *has_non_opaque_alpha;

      const bool avif_lossless = native_backend_detail::avif_lossless_requested(cfg_);
      const auto source_chroma = native_backend_detail::lossless_source_chroma(decoded.image);
      const bool explicit_svt = requested_avif_encoder == AvifEncoderMode::svt;
      if (avif_lossless && explicit_svt) {
        prepared.settings.avif_encoder = AvifEncoderMode::svt;
        prepared.settings.chroma_mode = ChromaMode::yuv420;
        prepared.settings.chroma_reason = "explicit SVT cannot preserve AVIF lossless source parameters";
        prepared.settings.bit_depth_reason = "lossless requires source bit-depth preservation";
        return prepare_failed(
            "svt-av1-hdr 不能保证 AVIF 无损模式继承源图参数；请使用 --avif-encoder auto/aom。",
            prepared.settings);
      }

      const auto selection_requested_encoder =
          avif_lossless && requested_avif_encoder == AvifEncoderMode::automatic
              ? AvifEncoderMode::aom
              : requested_avif_encoder;
      ChromaMode selection_requested_chroma = ChromaMode::auto_keep;
      if (avif_lossless) {
        selection_requested_chroma = source_chroma;
        prepared.settings.chroma_reason = source_chroma == ChromaMode::auto_keep
                                             ? "lossless kept chroma auto because source YUV chroma is unknown"
                                             : "lossless inherited source YUV chroma";
      } else if (explicit_svt) {
        selection_requested_chroma = ChromaMode::yuv420;
        prepared.settings.chroma_reason = cfg_.chroma_mode == ChromaMode::yuv420
                                             ? "explicit SVT uses 420 chroma"
                                             : "explicit SVT lossy encoding forced chroma to 420";
      } else if (cfg_.chroma_mode != ChromaMode::auto_keep) {
        selection_requested_chroma = cfg_.chroma_mode;
        prepared.settings.chroma_reason = "user requested chroma";
      } else {
        selection_requested_chroma = source_chroma;
        prepared.settings.chroma_reason = source_chroma == ChromaMode::auto_keep
                                             ? "chroma auto; source YUV chroma unknown"
                                             : "chroma auto inherited source YUV chroma for encoder selection";
      }

      std::optional<int> selection_requested_bit_depth{};
      if (avif_lossless) {
        selection_requested_bit_depth = native_backend_detail::lossless_source_bit_depth(decoded.image);
        prepared.settings.bit_depth_reason = "lossless inherited source bit-depth";
      } else if (cfg_.bit_depth) {
        selection_requested_bit_depth = cfg_.bit_depth;
        prepared.settings.bit_depth_reason = "explicit bit-depth requested";
      } else if (prepared.settings.source_bit_depth && *prepared.settings.source_bit_depth >= 10) {
        selection_requested_bit_depth = prepared.settings.source_bit_depth;
        prepared.settings.bit_depth_reason = std::format(
            "lossy preserved source {}-bit depth", *prepared.settings.source_bit_depth);
      }
      if (!avif_lossless && !selection_requested_bit_depth &&
          prepared.settings.source_bit_depth && *prepared.settings.source_bit_depth == 8) {
        prepared.settings.bit_depth_reason = "lossy auto may upconvert 8-bit source to encoder preferred bit-depth";
      }
      if (avif_lossless && selection_requested_bit_depth &&
          !native_backend_detail::avif_lossless_bit_depth_supported(*selection_requested_bit_depth)) {
        return prepare_failed(std::format(
                                  "AVIF 无损模式无法保持源图 {}-bit 位深；libavif AOM 当前仅支持 8、10、12-bit 输出。",
                                  *selection_requested_bit_depth),
                              prepared.settings);
      }

      const bool must_preserve_alpha = native_backend_detail::alpha_must_be_preserved(
          cfg_.alpha_policy, prepared.settings.source_has_alpha_channel);
      const auto selection = select_avif_encoder_for_current_build(AvifEncoderSelectionRequest{
          .requested_encoder = selection_requested_encoder,
          .requested_chroma = selection_requested_chroma,
          .requested_bit_depth = selection_requested_bit_depth,
          .requested_bit_depth_reason = prepared.settings.bit_depth_reason,
          .has_alpha = prepared.settings.source_has_alpha_channel,
          .must_preserve_alpha = must_preserve_alpha,
          .visual_quality_search = cfg_.visual_quality.has_value(),
          .speed_explicit = cfg_.speed.has_value(),
          .allow_zenrav1e_alpha = false,
          .pixel_count = pixel_count,
          .width = static_cast<std::uint32_t>(decoded.image.width),
          .height = static_cast<std::uint32_t>(decoded.image.height),
          .speed = prepared.settings.speed},
          cfg_.enable_experimental_encoders);
      if (!selection) {
        prepared.settings.requested_avif_encoder = selection_requested_encoder;
        prepared.settings.requested_chroma_mode = selection_requested_chroma;
        prepared.settings.requested_bit_depth = selection_requested_bit_depth;
        prepared.settings.encoder_supports_alpha = native_backend_detail::encoder_supports_alpha(
            selection_requested_encoder);
        prepared.settings.applied_alpha = native_backend_detail::applied_alpha_name(
            prepared.settings.source_has_alpha_channel, false, prepared.settings.encoder_supports_alpha);
        if (cfg_.alpha_policy == AlphaModePolicy::force &&
            prepared.settings.source_has_alpha_channel && !prepared.settings.encoder_supports_alpha) {
          prepared.settings.alpha_reason = "force requested alpha preservation but selected encoder does not support alpha";
        }
        return prepare_failed(selection.error(), prepared.settings);
      }
      prepared.settings.avif_encoder = selection->applied_encoder;
      prepared.settings.chroma_mode = selection->applied_chroma;
      prepared.settings.bit_depth = selection->applied_bit_depth;
      prepared.settings.speed = selection->speed;
      prepared.settings.requested_avif_encoder = selection->requested_encoder;
      prepared.settings.requested_chroma_mode = selection->requested_chroma;
      prepared.settings.requested_bit_depth = selection->requested_bit_depth;
      if (prepared.settings.bit_depth_reason.empty()) {
        prepared.settings.bit_depth_reason = selection->bit_depth_reason;
      }
      prepared.settings.encoder_fallback_reason = selection->fallback_reason;
      prepared.settings.encoder_supports_alpha = native_backend_detail::encoder_supports_alpha(
          selection->applied_encoder);
      const bool preserve_alpha = prepared.settings.source_has_alpha_channel &&
                                  ((cfg_.alpha_policy == AlphaModePolicy::force &&
                                    prepared.settings.encoder_supports_alpha) ||
                                   (cfg_.alpha_policy == AlphaModePolicy::automatic &&
                                    *has_non_opaque_alpha &&
                                    prepared.settings.encoder_supports_alpha));
      prepared.settings.applied_alpha = native_backend_detail::applied_alpha_name(
          prepared.settings.source_has_alpha_channel, preserve_alpha,
          prepared.settings.encoder_supports_alpha);
      if (!prepared.settings.source_has_alpha_channel) {
        prepared.settings.alpha_reason = "source has no alpha channel";
      } else if (cfg_.alpha_policy == AlphaModePolicy::off) {
        prepared.settings.alpha_reason = "alpha stripped by user request";
      } else if (cfg_.alpha_policy == AlphaModePolicy::automatic && !*has_non_opaque_alpha) {
        prepared.settings.alpha_reason = "auto stripped fully opaque alpha";
      } else if (cfg_.alpha_policy == AlphaModePolicy::automatic &&
                 prepared.settings.encoder_supports_alpha) {
        prepared.settings.alpha_reason = "auto kept non-opaque alpha because selected encoder supports alpha";
      } else if (cfg_.alpha_policy == AlphaModePolicy::automatic) {
        prepared.settings.alpha_reason = "auto stripped alpha because selected encoder does not support alpha";
      } else if (prepared.settings.encoder_supports_alpha) {
        prepared.settings.alpha_reason = "force kept source alpha channel";
      } else {
        prepared.settings.alpha_reason = "force requested alpha preservation but selected encoder does not support alpha";
      }
      prepared.avif_bit_depth_reason = prepared.settings.bit_depth_reason;
      logger_.info(std::format(
          "AVIF decision: input={} user_encoder={} user_chroma={} source_chroma={} source_bit_depth={} alpha_policy={} source_alpha={} non_opaque_alpha={} selected_encoder={} requested_chroma={} applied_chroma={} requested_bit_depth={} applied_bit_depth={} applied_alpha={} fallback={} chroma_reason={} alpha_reason={} bit_depth_reason={} color_source={}",
          path_to_utf8(image.path.filename()),
          prepared.settings.user_encoder_id,
          prepared.settings.user_chroma,
          prepared.settings.source_chroma,
          prepared.settings.source_bit_depth ? std::format("{}", *prepared.settings.source_bit_depth) : std::string{""},
          prepared.settings.alpha_policy_name,
          prepared.settings.source_alpha_mode,
          *has_non_opaque_alpha ? "true" : "false",
          avif_encoder_mode_name(selection->applied_encoder),
          chroma_mode_name(selection->requested_chroma),
          chroma_mode_name(selection->applied_chroma),
          selection->requested_bit_depth ? std::format("{}", *selection->requested_bit_depth) : std::string{""},
          selection->applied_bit_depth ? std::format("{}", *selection->applied_bit_depth) : std::string{""},
          prepared.settings.applied_alpha,
          selection->fallback_reason,
          prepared.settings.chroma_reason,
          prepared.settings.alpha_reason,
          prepared.settings.bit_depth_reason,
          prepared.settings.color_metadata_source));
      if (selection->applied_encoder != AvifEncoderMode::svt) {
        prepared.encoder = native_backend_detail::encoder_for_output_format(
            cfg_.output_format, selection->applied_encoder);
      }
    } else {
      prepared.encoder = native_backend_detail::encoder_for_output_format(
          cfg_.output_format, requested_avif_encoder);
    }

    if (!prepared.encoder && !(cfg_.output_format == OutputFormat::avif &&
                               prepared.settings.avif_encoder == AvifEncoderMode::svt)) {
      return prepare_failed(std::format("native backend 暂不支持输出格式: {}",
                                        output_format_name(cfg_.output_format)),
                            prepared.settings);
    }
    return std::move(prepared);
  }

  [[nodiscard]] std::expected<NativeEncodeResult, std::string> execute_encode(
      const ImageDecodeResult& decoded,
      ImageEncoder* encoder,
      const NativeEncodeSettings& settings,
      const fs::path& output_path,
      std::stop_token stop_token) const {
    const bool use_svtav1hdr = cfg_.output_format == OutputFormat::avif &&
                               settings.avif_encoder == AvifEncoderMode::svt;
    if (cfg_.visual_quality) {
      auto output_decoder = native_backend_detail::decoder_for_output_format(cfg_.output_format);
      const auto candidate_path = output_path.parent_path() /
                                  (output_path.filename().wstring() + L".candidate");
      if (use_svtav1hdr) {
        class SvtAv1HdrImageEncoder final : public ImageEncoder {
         public:
          [[nodiscard]] std::string_view id() const noexcept override { return "svt-av1-hdr"; }
          [[nodiscard]] CodecCapabilities capabilities() const override {
            return CodecCapabilities{.output_format = OutputFormat::avif,
                                     .features = CodecFeature::thread_control |
                                                 CodecFeature::visual_quality_search,
                                     .bit_depths = {8, 10}};
          }
          std::expected<NativeEncodeResult, std::string> encode(
              const ImageBuffer& image,
              const NativeEncodeSettings& settings) const override {
            return encode_svtav1hdr_in_process(image, settings);
          }
        } svt_encoder;
        auto search = encode_with_native_visual_quality_search(decoded.image, svt_encoder,
                                                               *output_decoder, settings,
                                                               candidate_path, stop_token);
        std::error_code ec;
        fs::remove(candidate_path, ec);
        if (!search) {
          return std::unexpected{search.error()};
        }
        return std::move(search->encode_result);
      }

      auto search = encode_with_native_visual_quality_search(decoded.image, *encoder,
                                                             *output_decoder, settings,
                                                             candidate_path, stop_token);
      std::error_code ec;
      fs::remove(candidate_path, ec);
      if (!search) {
        return std::unexpected{search.error()};
      }
      return std::move(search->encode_result);
    }

    if (use_svtav1hdr) {
      return encode_svtav1hdr_in_process(decoded.image, settings);
    }
    return encoder->encode(decoded.image, settings);
  }

  [[nodiscard]] EncodeResult finalize_result(EncodeResult result,
                                             NativeEncodeResult encoded,
                                             const ImageDecodeResult& decoded,
                                             bool decoder_used_fallback,
                                             std::string avif_bit_depth_reason,
                                             EncodeStartedAt started) const {
    if (auto written = native_backend_detail::write_output_bytes(
            result.output_path, std::span<const std::byte>{encoded.encoded.bytes});
        !written) {
      mark_failed(result, written.error());
      return result;
    }

    encoded.diagnostics.decoder_id = decoded.decoder_id;
    encoded.diagnostics.used_decoder_fallback = decoder_used_fallback;
    native_backend_detail::copy_native_result(encoded, result);
    if (!decoded.decoder_id.empty()) {
      result.decoder_id = decoded.decoder_id;
      result.command = std::format("native:{}:{} q{} speed={}",
                                   result.decoder_id,
                                   result.encoder_id,
                                   result.final_encoder_quality,
                                   result.speed);
    }
    if (!avif_bit_depth_reason.empty()) {
      result.bit_depth_reason = avif_bit_depth_reason;
    }
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();
    result.processed = true;
    result.ok = true;
    result.message = "OK";
    logger_.info(std::format("native encode ok: {} -> {}",
                             path_to_utf8(result.input_path.filename()),
                             path_to_utf8(result.output_path.filename())));
    return result;
  }

  EncodeResult encode_with_overrides(const ImageFile& image,
                                     EncodeOverrides overrides,
                                     std::stop_token stop_token = {}) const {
    const auto started = std::chrono::steady_clock::now();
    auto result = initialize_result(image);

    if (cancel_if_requested(result, stop_token)) {
      return result;
    }

    if (cfg_.collision_mode == CollisionMode::skip && fs::exists(result.output_path)) {
      result.processed = true;
      result.ok = true;
      result.skipped = true;
      result.message = "输出已存在，已跳过。";
      return result;
    }

    if (auto passthrough = try_avif_lossless_passthrough(image, result, started, stop_token)) {
      return std::move(*passthrough);
    }

    auto decoded_input = decode_input(image);
    if (!decoded_input) {
      mark_failed(result, decoded_input.error());
      return result;
    }

    if (cancel_if_requested(result, stop_token)) {
      return result;
    }

    auto prepared = prepare_encoding(image, decoded_input->decoded, std::move(overrides));
    if (!prepared) {
      native_backend_detail::copy_settings_diagnostics(prepared.error().settings, result);
      mark_failed(result, prepared.error().message);
      return result;
    }

    if (cancel_if_requested(result, stop_token)) {
      return result;
    }

    auto encoded = execute_encode(decoded_input->decoded, prepared->encoder.get(),
                                  prepared->settings, result.output_path, stop_token);
    if (!encoded) {
      native_backend_detail::copy_settings_diagnostics(prepared->settings, result);
      mark_failed(result, encoded.error());
      return result;
    }

    if (cancel_if_requested(result, stop_token)) {
      return result;
    }

    return finalize_result(std::move(result), std::move(*encoded), decoded_input->decoded,
                           decoded_input->decoder_used_fallback,
                           std::move(prepared->avif_bit_depth_reason), started);
  }

 private:
  const AppConfig& cfg_;
  FileLogger& logger_;
  ResourcePlan resources_;
};

}  // namespace awj
