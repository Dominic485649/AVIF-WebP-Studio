#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <dwmapi.h>
#include <scn/scan.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <slint.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "awj_studio.h"

import awj.avif_aom_codec;
import awj.avif_registry;
import awj.config;
import awj.core;
import awj.decoder_registry;
import awj.encoding_defaults;
import awj.large_image_plan;
import awj.native_backend;
import awj.pipeline;
import awj.resource_planner;

namespace {
awj::LargeImageDecision manual_large_image_decision(
    awj::ImageDimensions dimensions, bool grid_available,
    bool zenrav1e_available) {
  auto decision =
      awj::classify_large_image(dimensions, grid_available, zenrav1e_available);
  decision.klass = awj::LargeImageClass::large_mode_required;
  if (decision.reason == awj::LargeImageReason::none) {
    decision.reason_text = "用户手动加入大图队列。";
  }
  return decision;
}

struct Win32HandleDeleter {
  using pointer = HANDLE;
  void operator()(HANDLE value) const noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
};

using UniqueWin32Handle = std::unique_ptr<void, Win32HandleDeleter>;

constexpr DWORD kStudioWorkerForceStopExitCode = 130;

struct StudioChildProcess {
  UniqueWin32Handle process{};
  UniqueWin32Handle thread{};
  UniqueWin32Handle job{};
  UniqueWin32Handle cancel_event{};
  UniqueWin32Handle output_read{};
  std::wstring command_line{};
  DWORD process_id{};
  std::atomic_bool cancel_requested{};
  std::atomic_bool force_terminated{};

  void request_cancel() noexcept {
    cancel_requested.store(true, std::memory_order_release);
    if (cancel_event != nullptr) {
      SetEvent(cancel_event.get());
    }
  }

  bool terminate(DWORD exit_code = kStudioWorkerForceStopExitCode) noexcept {
    force_terminated.store(true, std::memory_order_release);
    request_cancel();
    bool terminated = false;
    if (job != nullptr) {
      terminated = TerminateJobObject(job.get(), exit_code) != FALSE;
    }
    if (!terminated && process != nullptr) {
      terminated = TerminateProcess(process.get(), exit_code) != FALSE;
    }
    return terminated;
  }
};

struct UiState {
  std::jthread worker{};
  std::shared_ptr<slint::VectorModel<TaskRow>> task_rows{};
  std::shared_ptr<slint::VectorModel<LargeImageRow>> large_image_rows{};
  std::vector<awj::BatchLargeImageItem> large_image_items{};
  slint::Timer theme_timer{};
  slint::Timer update_timer{};
  std::uint64_t run_id{};
  std::mutex mutex{};
  std::vector<awj::BatchProgress> pending_events{};
  std::shared_ptr<StudioChildProcess> active_child{};
  bool worker_active{};
};

struct DropBridge {
  slint::ComponentWeakHandle<AwjStudio> weak{};
  std::shared_ptr<UiState> state{};
  WNDPROC previous_proc{};
};

LargeImageRow make_large_image_row(const awj::BatchLargeImageItem& item,
                                   std::string_view status);
int preferred_large_image_action_index(
    const awj::BatchLargeImageItem& item) noexcept;

LRESULT CALLBACK drop_bridge_window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                         LPARAM lparam);

struct ComApartment {
  ComApartment()
      : init{CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)} {}

  ~ComApartment() {
    if (should_uninitialize()) {
      CoUninitialize();
    }
  }

  [[nodiscard]] bool usable() const noexcept {
    return SUCCEEDED(init) || init == RPC_E_CHANGED_MODE;
  }

  [[nodiscard]] bool should_uninitialize() const noexcept {
    return SUCCEEDED(init);
  }

  HRESULT init{};
};

template <class Interface>
struct ComReleaseDeleter {
  void operator()(Interface* value) const noexcept {
    if (value != nullptr) {
      value->Release();
    }
  }
};

struct CoTaskMemDeleter {
  void operator()(wchar_t* value) const noexcept {
    if (value != nullptr) {
      CoTaskMemFree(value);
    }
  }
};

std::string shared_to_string(const slint::SharedString& value) {
  return std::string{value.data(), value.size()};
}

slint::SharedString to_shared(std::string_view text);

std::expected<void, std::string> add_manual_large_image_path(
    UiState& state, const std::filesystem::path& path, bool allow_wic_fallback,
    bool grid_available, bool zenrav1e_available) {
  try {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      return std::unexpected{std::format("不是可添加的文件: {}。",
                                         awj::display_path_for_user(path))};
    }
    if (!awj::is_supported_image_extension(path)) {
      return std::unexpected{std::format("文件格式不受支持: {}。支持 {}。",
                                         awj::display_path_for_user(path),
                                         awj::kSupportedImageExtensionsText)};
    }
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec) {
      return std::unexpected{std::format("读取文件大小失败: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
    const auto dimensions = awj::probe_image_dimensions_for_path(
        path,
        awj::DecoderRegistryOptions{.allow_wic_fallback = allow_wic_fallback});
    if (!dimensions) {
      return std::unexpected{dimensions.error()};
    }
    const auto index = state.large_image_items.size();
    awj::ImageFile file{.index = index, .path = path, .bytes = bytes};
    auto decision = manual_large_image_decision(*dimensions, grid_available,
                                                zenrav1e_available);
    auto item = awj::BatchLargeImageItem{.file = std::move(file),
                                         .dimensions = *dimensions,
                                         .decision = std::move(decision)};
    auto row = make_large_image_row(item, "等待选择");
    state.large_image_items.push_back(std::move(item));
    state.large_image_rows->push_back(std::move(row));
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"添加大图任务时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"添加大图任务时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"添加大图任务时文件系统访问失败。"};
  }
}
std::string trim_copy(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  const auto first = std::ranges::find_if_not(value, is_space);
  const auto last =
      std::ranges::find_if_not(value | std::views::reverse, is_space).base();
  if (first >= last) {
    return {};
  }
  return std::string{first, last};
}

bool output_dir_is_empty(const AwjStudio& app) {
  return trim_copy(shared_to_string(app.get_output_dir())).empty();
}

void set_input_path_preserving_output(AwjStudio& app,
                                      const std::filesystem::path& path) {
  const bool should_fill_output = output_dir_is_empty(app);
  app.set_input_path(to_shared(awj::path_to_utf8(path)));
  if (should_fill_output) {
    app.set_output_dir(
        to_shared(awj::path_to_utf8(awj::default_output_dir_for(path))));
  }
}

std::expected<int, std::string> parse_int_field(std::string text,
                                                std::string_view name,
                                                int minimum, int maximum) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::unexpected{std::format("{} 不能为空。", name)};
  }
  const std::string_view source{text};
  const auto parsed = scn::scan_int<int>(source);
  if (!parsed || parsed->begin() != parsed->end()) {
    return std::unexpected{std::format("{} 必须是整数。", name)};
  }
  const int value = parsed->value();
  if (value < minimum || value > maximum) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", name, minimum, maximum)};
  }
  return value;
}

std::expected<int, std::string> parse_quality_field(std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::unexpected{"质量不能为空。"};
  }
  return awj::parse_quality(awj::wide_from_utf8(text));
}

std::expected<std::optional<int>, std::string> parse_optional_int_field(
    std::string text, std::string_view name, int minimum, int maximum) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::optional<int>{};
  }
  const auto value = parse_int_field(std::move(text), name, minimum, maximum);
  if (!value) {
    return std::unexpected{value.error()};
  }
  return std::optional<int>{*value};
}

std::expected<std::optional<int>, std::string> parse_visual_quality_field(
    std::string text) {
  return parse_optional_int_field(std::move(text), "视觉质量", 1, 100);
}

std::expected<int, std::string> parse_jobs_field(std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return awj::default_max_jobs();
  }
  return awj::parse_auto_jobs(awj::wide_from_utf8(text));
}

std::expected<std::optional<int>, std::string> parse_bit_depth_field(
    std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::optional<int>{};
  }
  const auto value = parse_int_field(std::move(text), "位深", 1, 16);
  if (!value) {
    return std::unexpected{value.error()};
  }
  return std::optional<int>{*value};
}

std::expected<std::uint64_t, std::string> parse_memory_limit_field(
    std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::uint64_t{};
  }
  const auto gib = parse_int_field(std::move(text), "内存", 1, 1024);
  if (!gib) {
    return std::unexpected{"内存只允许填写 GiB 数字，例如 4；留空表示自动。"};
  }
  constexpr std::uint64_t bytes_per_gib = 1024ull * 1024ull * 1024ull;
  return static_cast<std::uint64_t>(*gib) * bytes_per_gib;
}

slint::SharedString to_shared(std::string_view text) {
  return slint::SharedString{std::string{text}.c_str()};
}

std::string text_from_wide(std::wstring_view text) {
  return awj::utf8_from_wide(text);
}

std::string text_from_int(int value) { return std::format("{}", value); }

ComboOption combo_option(std::string_view text, bool enabled = true) {
  return ComboOption{.text = to_shared(text), .enabled = enabled};
}

void set_combo_options(
    AwjStudio& app, const std::vector<ComboOption>& options,
    void (AwjStudio::*setter)(const std::shared_ptr<slint::Model<ComboOption>>&)
        const) {
  auto model = std::make_shared<slint::VectorModel<ComboOption>>();
  model->set_vector(options);
  (app.*setter)(model);
}

bool avif_encoder_enabled_for_ui(awj::AvifEncoderMode mode,
                                 bool enable_experimental) {
  if (mode == awj::AvifEncoderMode::automatic) {
    return true;
  }
  auto capabilities =
      awj::avif_encoder_capabilities_for_current_build(enable_experimental);
  const auto it =
      std::ranges::find(capabilities, mode, &awj::AvifEncoderCapability::mode);
  return it != capabilities.end() && it->enabled &&
         (!it->experimental || it->feature_enabled);
}

std::vector<ComboOption> avif_encoder_options(bool enable_experimental) {
  return {
      combo_option("auto"),
      combo_option("svt-av1-hdr",
                   avif_encoder_enabled_for_ui(awj::AvifEncoderMode::svt,
                                               enable_experimental)),
      combo_option("aom", avif_encoder_enabled_for_ui(awj::AvifEncoderMode::aom,
                                                      enable_experimental)),
      combo_option("zenrav1e",
                   avif_encoder_enabled_for_ui(awj::AvifEncoderMode::zenrav1e,
                                               enable_experimental))};
}

struct LargeImageManualAvailability {
  bool grid{};
  bool zenrav1e{};
};

LargeImageManualAvailability large_image_manual_availability(
    bool enable_experimental) {
  const auto capabilities =
      awj::avif_encoder_capabilities_for_current_build(enable_experimental);
  LargeImageManualAvailability result{};
  for (const auto& capability : capabilities) {
    const bool enabled = capability.enabled && (!capability.experimental ||
                                                capability.feature_enabled);
    if (!enabled) {
      continue;
    }
    if (capability.mode == awj::AvifEncoderMode::aom &&
        capability.supports_avif_grid) {
      result.grid = true;
    }
    if (capability.mode == awj::AvifEncoderMode::zenrav1e) {
      result.zenrav1e = true;
    }
  }
  return result;
}

std::expected<std::vector<std::filesystem::path>, std::string>
supported_files_in_folder(const std::filesystem::path& folder) {
  try {
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it{
        folder, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) {
      return std::unexpected{std::format("扫描文件夹失败: {}；系统错误：{}。",
                                         awj::display_path_for_user(folder),
                                         ec.message())};
    }
    for (std::filesystem::recursive_directory_iterator end; it != end;
         it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      if (!it->is_regular_file(ec) || ec) {
        ec.clear();
        continue;
      }
      if (awj::is_supported_image_extension(it->path())) {
        paths.push_back(it->path());
      }
    }
    std::ranges::sort(paths);
    return paths;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"扫描文件夹时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"扫描文件夹时文件数量超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"扫描文件夹时文件系统访问失败。"};
  }
}

void add_manual_large_images_from_picker(AwjStudio& app, UiState& state,
                                         const std::filesystem::path& picked,
                                         bool folder) {
  const auto availability =
      large_image_manual_availability(app.get_experimental_encoders());
  const bool allow_wic_fallback = app.get_allow_wic_fallback();
  std::vector<std::filesystem::path> paths;
  if (folder) {
    auto scanned = supported_files_in_folder(picked);
    if (!scanned) {
      app.set_status_text(to_shared(scanned.error()));
      return;
    }
    paths = std::move(*scanned);
    if (paths.empty()) {
      app.set_status_text(to_shared("文件夹中没有支持的图片文件。"));
      return;
    }
  } else {
    paths.push_back(picked);
  }

  const bool was_empty = state.large_image_items.empty();
  std::size_t added = 0;
  std::size_t failed = 0;
  std::string first_error;
  for (const auto& path : paths) {
    auto result =
        add_manual_large_image_path(state, path, allow_wic_fallback,
                                    availability.grid, availability.zenrav1e);
    if (result) {
      ++added;
    } else {
      ++failed;
      if (first_error.empty()) {
        first_error = result.error();
      }
    }
  }
  if (was_empty && added > 0) {
    app.set_selected_large_image_index(0);
    if (!state.large_image_items.empty()) {
      app.set_selected_large_image_action_index(
          preferred_large_image_action_index(state.large_image_items.front()));
    }
  }
  if (added == 0) {
    app.set_status_text(
        to_shared(first_error.empty() ? "未添加任何大图任务。" : first_error));
    return;
  }
  app.set_status_text(to_shared(
      std::format("已添加 {} 个大图任务{}。", added,
                  failed == 0 ? "" : std::format("，{} 个失败", failed))));
}

void refresh_avif_encoder_options(AwjStudio& app) {
  const auto options = avif_encoder_options(app.get_experimental_encoders());
  set_combo_options(app, options, &AwjStudio::set_avif_encoder_options);
  const auto selected = app.get_avif_encoder_index();
  if (selected < 0 || static_cast<std::size_t>(selected) >= options.size() ||
      !options[static_cast<std::size_t>(selected)].enabled) {
    app.set_avif_encoder_index(0);
  }
}

bool template_contains_token(std::string_view text, std::string_view token) {
  return text.find(token) != std::string_view::npos;
}

void sync_template_flags(AwjStudio& app) {
  const auto text = shared_to_string(app.get_template_text());
  app.set_template_params_selected(template_contains_token(text, "{params}"));
  app.set_template_date_selected(template_contains_token(text, "{date}"));
  app.set_template_time_selected(template_contains_token(text, "{time}"));
  app.set_template_rand_selected(template_contains_token(text, "{rand}"));
  app.set_template_hash8_selected(template_contains_token(text, "{hash8}"));
  app.set_template_sha2568_selected(
      template_contains_token(text, "{sha2568}") ||
      template_contains_token(text, "{sha256_8}"));
}

std::string token_with_separator(std::string_view text,
                                 std::string_view token) {
  if (text.empty()) {
    return std::string{token};
  }
  const char last = text.back();
  const bool needs_separator =
      last != '_' && last != '-' && last != ' ' && last != '.';
  return std::format("{}{}{}", text, needs_separator ? "_" : "", token);
}

void toggle_template_token(AwjStudio& app, std::string_view token) {
  auto text = shared_to_string(app.get_template_text());
  const auto pos = text.find(token);
  if (pos != std::string::npos) {
    text.erase(pos, token.size());
    if (pos > 0 && text[pos - 1] == '_') {
      text.erase(pos - 1, 1);
    } else if (pos < text.size() && text[pos] == '_') {
      text.erase(pos, 1);
    }
    if (text.empty()) {
      text = std::string{awj::encoding_defaults::default_output_template_text};
    }
  } else {
    text = token_with_separator(text, token);
  }
  app.set_template_text(to_shared(text));
  sync_template_flags(app);
}

bool windows_prefers_dark_mode() {
  DWORD apps_use_light_theme = 1;
  DWORD value_size = sizeof(apps_use_light_theme);
  const auto status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &apps_use_light_theme,
      &value_size);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  return apps_use_light_theme == 0;
}

bool effective_studio_dark_mode(const AwjStudio& app) {
  return app.get_theme_index() == 2 ||
         (app.get_theme_index() == 0 && app.get_system_dark_mode());
}

void apply_title_bar_theme(slint::Window& window, bool dark_mode) noexcept {
  try {
    const HWND hwnd = window.win32_hwnd();
    if (hwnd == nullptr) {
      return;
    }
    const BOOL use_dark_mode = dark_mode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark_mode,
                          sizeof(use_dark_mode));
  } catch (...) {
  }
}

void set_status_text_noexcept(AwjStudio& app, std::string_view text) noexcept {
  try {
    app.set_status_text(to_shared(text));
  } catch (...) {
  }
}

void reset_failed_run(AwjStudio& app, UiState& state, std::uint64_t run_id,
                      std::string_view message) noexcept {
  try {
    std::scoped_lock lock{state.mutex};
    if (run_id != 0 && state.run_id == run_id) {
      state.update_timer.stop();
      state.pending_events.clear();
      state.worker_active = false;
    }
  } catch (...) {
  }
  try {
    app.set_running(false);
  } catch (...) {
  }
  set_status_text_noexcept(app, message);
}

bool clear_run_if_callback_not_posted(UiState& state,
                                      std::uint64_t run_id) noexcept {
  try {
    std::scoped_lock lock{state.mutex};
    if (state.run_id != run_id) {
      return false;
    }
    state.pending_events.clear();
    state.worker_active = false;
    return true;
  } catch (...) {
    return false;
  }
}

void report_ui_callback_failure(slint::ComponentWeakHandle<AwjStudio> weak,
                                std::string_view context,
                                std::string_view detail) noexcept {
  try {
    if (auto app = weak.lock()) {
      try {
        set_status_text_noexcept(**app, std::format("{}：{}", context, detail));
      } catch (...) {
        set_status_text_noexcept(**app, "界面操作失败。");
      }
    }
  } catch (...) {
  }
}

template <class Function>
void run_ui_callback(slint::ComponentWeakHandle<AwjStudio> weak,
                     std::string_view context, Function&& fn) noexcept {
  try {
    std::forward<Function>(fn)();
  } catch (const std::bad_alloc&) {
    report_ui_callback_failure(weak, context, "内存不足。");
  } catch (const std::length_error&) {
    report_ui_callback_failure(weak, context, "数据超过运行时限制。");
  } catch (const std::exception&) {
    report_ui_callback_failure(weak, context, "发生未预期异常。");
  } catch (...) {
    report_ui_callback_failure(weak, context, "发生未知异常。");
  }
}

template <class Function>
bool post_to_ui(slint::ComponentWeakHandle<AwjStudio> weak, Function&& fn) {
  try {
    slint::invoke_from_event_loop(
        [weak, fn = std::forward<Function>(fn)]() mutable {
          run_ui_callback(weak, "界面更新失败", [&] {
            if (auto app = weak.lock()) {
              fn(**app);
            }
          });
        });
    return true;
  } catch (...) {
    return false;
  }
}

std::string result_status_text(const awj::EncodeResult& result) {
  if (result.ok) {
    if (result.skipped) {
      return "已跳过";
    }
    const double ratio = result.original_bytes == 0
                             ? 0.0
                             : static_cast<double>(result.output_bytes) /
                                   static_cast<double>(result.original_bytes) *
                                   100.0;
    if (result.requested_visual_quality) {
      if (result.lossless) {
        return std::format("完成 · {} · {:.1f}% · VQ lossless · q{} · {:.2f}s",
                           awj::format_size(result.output_bytes), ratio,
                           result.final_encoder_quality, result.seconds);
      }
      const char* target_state =
          result.visual_quality_target_met ? "" : " 未达标兜底";
      return std::format("完成 · {} · {:.1f}% · VQ {:.2f}{} · q{} · {:.2f}s",
                         awj::format_size(result.output_bytes), ratio,
                         result.visual_score, target_state,
                         result.final_encoder_quality, result.seconds);
    }
    return std::format("完成 · {} · {:.1f}% · {:.2f}s",
                       awj::format_size(result.output_bytes), ratio,
                       result.seconds);
  }
  if (result.canceled) {
    return "已取消";
  }
  return result.message.empty() ? "失败"
                                : std::format("失败 · {}", result.message);
}

std::string result_log_text(const awj::EncodeResult& result) {
  if (!result.requested_visual_quality || !result.ok || result.skipped) {
    return {};
  }
  if (result.lossless) {
    return std::format("requested={} · lossless=true · final q{} · {}",
                       *result.requested_visual_quality,
                       result.final_encoder_quality,
                       awj::format_size(result.output_bytes));
  }
  const char* target_state =
      result.visual_quality_target_met ? "" : " · 未达标兜底";
  return std::format(
      "score {:.2f}{} · GMSD {:.6f} · MS-SSIM {:.6f} · Qg {:.2f} · Qm {:.2f} · "
      "q{} · {} 次 · {}",
      result.visual_score, target_state, result.raw_gmsd, result.raw_ms_ssim,
      result.gmsd_quality_score, result.msssim_quality_score,
      result.final_encoder_quality, result.search_attempt_count,
      awj::format_size(result.output_bytes));
}

constexpr std::size_t kMaxTaskRows = 5000;
constexpr std::size_t kMaxPendingEvents = 2048;

bool push_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                   TaskRow row) noexcept {
  if (rows == nullptr) {
    return false;
  }
  try {
    rows->push_back(std::move(row));
    while (rows->row_count() > kMaxTaskRows) {
      rows->erase(0);
    }
  } catch (...) {
    return false;
  }
  return true;
}

void add_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                  const awj::EncodeResult& result) noexcept {
  try {
    auto output_format = awj::OutputFormat::avif;
    auto ext = result.output_path.extension().wstring();
    std::ranges::transform(ext, ext.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    if (ext == L".webp") {
      output_format = awj::OutputFormat::webp;
    } else if (ext == L".jxl") {
      output_format = awj::OutputFormat::jxl;
    }

    push_task_row(
        rows,
        TaskRow{.filename =
                    to_shared(awj::path_to_utf8(result.input_path.filename())),
                .format = to_shared(awj::output_format_name(output_format)),
                .status = to_shared(result_status_text(result)),
                .log = to_shared(result_log_text(result)),
                .warning = result.ok &&
                           result.requested_visual_quality.has_value() &&
                           !result.visual_quality_target_met});
  } catch (...) {
  }
}

void append_log_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                    std::string_view text) noexcept {
  try {
    push_task_row(rows, TaskRow{.filename = {},
                                .format = {},
                                .status = {},
                                .log = to_shared(text),
                                .warning = false});
  } catch (...) {
  }
}

bool large_image_grid_available(const awj::BatchLargeImageItem& item) noexcept {
  if (!item.decision.available_grid) {
    return false;
  }
  const auto plan =
      awj::plan_grid(awj::GridPlanRequest{.width = item.dimensions.width,
                                          .height = item.dimensions.height,
                                          .mode = awj::GridMode::auto_grid});
  return plan && !plan->uses_padding;
}

bool large_image_zenrav1e_available(
    const awj::BatchLargeImageItem& item) noexcept {
  return item.decision.available_zenrav1e &&
         item.dimensions.width <=
             awj::encoding_defaults::avif_single_image_max_dimension &&
         item.dimensions.height <=
             awj::encoding_defaults::avif_single_image_max_dimension;
}

bool large_image_action_available(const awj::BatchLargeImageItem& item,
                                  std::string_view action) noexcept {
  if (action == "grid") {
    return large_image_grid_available(item);
  }
  if (action == "zenrav1e") {
    return large_image_zenrav1e_available(item);
  }
  return false;
}

int preferred_large_image_action_index(
    const awj::BatchLargeImageItem& item) noexcept {
  if (large_image_zenrav1e_available(item)) {
    return 0;
  }
  if (large_image_grid_available(item)) {
    return 1;
  }
  return 0;
}

std::string large_image_actions_summary(const awj::BatchLargeImageItem& item) {
  const auto grid = large_image_grid_available(item)
                        ? std::string{"grid 可用"}
                        : std::string{"grid 不可用"};
  std::string zenrav1e;
  if (large_image_zenrav1e_available(item)) {
    zenrav1e = "zenrav1e 可用";
  } else if (item.decision.available_zenrav1e) {
    zenrav1e = "zenrav1e 超出边长";
  } else {
    zenrav1e = "zenrav1e 不可用";
  }
  return std::format("{} / {}", grid, zenrav1e);
}

void add_large_image_task_row(
    const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
    const awj::BatchLargeImageItem& item) noexcept {
  try {
    push_task_row(
        rows,
        TaskRow{
            .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
            .format = to_shared("AVIF"),
            .status = to_shared("大图模式"),
            .log = to_shared(std::format(
                "原因：{}；{}；可用处理方式：{}。",
                awj::large_image_reason_name(item.decision.reason),
                item.decision.reason_text, large_image_actions_summary(item))),
            .warning = false});
  } catch (...) {
  }
}

LargeImageRow make_large_image_row(const awj::BatchLargeImageItem& item,
                                   std::string_view status) {
  const auto actions = large_image_actions_summary(item);

  return LargeImageRow{
      .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
      .dimensions = to_shared(std::format("{} x {}", item.dimensions.width,
                                          item.dimensions.height)),
      .reason = to_shared(std::format(
          "{} · {}", awj::large_image_reason_name(item.decision.reason),
          item.decision.reason_text)),
      .actions = to_shared(actions),
      .status = to_shared(status),
      .grid_available = large_image_grid_available(item),
      .zenrav1e_available = large_image_zenrav1e_available(item)};
}

bool push_large_image_row(UiState& state,
                          awj::BatchLargeImageItem item) noexcept {
  if (state.large_image_rows == nullptr) {
    return false;
  }
  try {
    const bool was_empty = state.large_image_items.empty();
    auto row = make_large_image_row(item, "等待选择");
    bool item_added = false;
    try {
      state.large_image_items.push_back(item);
      item_added = true;
      state.large_image_rows->push_back(std::move(row));
    } catch (...) {
      if (item_added) {
        state.large_image_items.pop_back();
      }
      return false;
    }
    return was_empty;
  } catch (...) {
    return false;
  }
}

bool slint_rect_contains(float x, float y, float left, float top, float width,
                         float height) noexcept {
  if (width <= 0.0f || height <= 0.0f) {
    return false;
  }
  return x >= left && y >= top && x <= left + width && y <= top + height;
}

bool point_in_import_drop_zone(const AwjStudio& app, POINT point) noexcept {
  try {
    return app.get_selected_page() == 1 &&
           slint_rect_contains(
               static_cast<float>(point.x), static_cast<float>(point.y),
               app.get_import_drop_zone_x(), app.get_import_drop_zone_y(),
               app.get_import_drop_zone_width(),
               app.get_import_drop_zone_height());
  } catch (...) {
    return false;
  }
}

bool point_in_window_drag_zone(const AwjStudio& app, POINT point) noexcept {
  try {
    return slint_rect_contains(static_cast<float>(point.x),
                               static_cast<float>(point.y),
                               app.get_drag_zone_x(), app.get_drag_zone_y(),
                               app.get_drag_zone_width(),
                               app.get_drag_zone_height());
  } catch (...) {
    return false;
  }
}

void select_first_large_image_from_state(AwjStudio& app,
                                         const UiState& state) noexcept {
  try {
    if (state.large_image_items.empty()) {
      return;
    }
    app.set_selected_large_image_index(0);
    app.set_selected_large_image_action_index(
        preferred_large_image_action_index(state.large_image_items.front()));
    app.set_selected_page(0);
  } catch (...) {
  }
}

std::vector<std::filesystem::path> paths_from_hdrop(HDROP drop) {
  std::vector<std::filesystem::path> paths;
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
  paths.reserve(count);
  for (UINT index = 0; index < count; ++index) {
    const UINT length = DragQueryFileW(drop, index, nullptr, 0);
    if (length == 0) {
      continue;
    }
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    const UINT copied = DragQueryFileW(drop, index, buffer.data(), length + 1);
    if (copied == 0) {
      continue;
    }
    buffer.resize(copied);
    paths.emplace_back(std::move(buffer));
  }
  return paths;
}

bool path_is_directory(const std::filesystem::path& path) noexcept {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) && !ec;
}

void apply_import_drop_paths(AwjStudio& app,
                             const std::vector<std::filesystem::path>& paths) {
  if (paths.empty()) {
    app.set_status_text(to_shared("拖入内容为空。"));
    return;
  }
  const auto& path = paths.front();
  const bool folder = path_is_directory(path);
  app.set_input_mode_index(folder ? 1 : 0);
  set_input_path_preserving_output(app, path);
  if (paths.size() > 1) {
    app.set_status_text(
        to_shared(std::format("已导入首个路径；普通编码队列一次使用一个输入路径"
                              "，其余 {} 项未作为输入路径。",
                              paths.size() - 1)));
  } else {
    app.set_status_text(to_shared("已从拖拽导入输入路径。"));
  }
}

void apply_large_image_drop_paths(
    AwjStudio& app, UiState& state,
    const std::vector<std::filesystem::path>& paths) {
  if (paths.empty()) {
    app.set_status_text(to_shared("拖入内容为空。"));
    return;
  }
  const bool was_empty = state.large_image_items.empty();
  std::size_t added = 0;
  for (const auto& path : paths) {
    if (path_is_directory(path)) {
      const auto before = state.large_image_items.size();
      add_manual_large_images_from_picker(app, state, path, true);
      added += state.large_image_items.size() - before;
    } else {
      const auto before = state.large_image_items.size();
      add_manual_large_images_from_picker(app, state, path, false);
      added += state.large_image_items.size() - before;
    }
  }
  if (was_empty && added > 0) {
    select_first_large_image_from_state(app, state);
  }
  if (added > 0) {
    app.set_status_text(
        to_shared(std::format("已从拖拽添加 {} 个大图任务。", added)));
  }
}

void set_large_image_status(UiState& state, int index,
                            std::string_view status) noexcept {
  if (state.large_image_rows == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= state.large_image_items.size()) {
    return;
  }
  try {
    state.large_image_rows->set_row_data(
        static_cast<std::size_t>(index),
        make_large_image_row(
            state.large_image_items[static_cast<std::size_t>(index)], status));
  } catch (...) {
  }
}

std::string large_image_action_status(const awj::BatchLargeImageItem& item,
                                      std::string_view action) {
  if (!large_image_action_available(item, action)) {
    return std::format("{} 不可用", action);
  }
  if (action == "grid") {
    const auto plan =
        awj::plan_grid(awj::GridPlanRequest{.width = item.dimensions.width,
                                            .height = item.dimensions.height,
                                            .mode = awj::GridMode::auto_grid});
    if (!plan) {
      return std::format("grid 规划失败：{}", plan.error());
    }
    return std::format("已选择 grid · {}x{} 分块 · tile {}x{}", plan->cols,
                       plan->rows, plan->tile_width, plan->tile_height);
  }
  if (action == "zenrav1e") {
    return "已选择 zenrav1e · 单项编码";
  }
  return "未知处理方式";
}

void append_pending_event(UiState& state, std::uint64_t run_id,
                          const awj::BatchProgress& event) noexcept {
  try {
    std::scoped_lock lock{state.mutex};
    if (state.run_id != run_id) {
      return;
    }
    if (state.pending_events.size() >= kMaxPendingEvents) {
      const auto keep = [](const awj::BatchProgress& pending) {
        return pending.kind == awj::BatchEventKind::item_finished ||
               pending.kind == awj::BatchEventKind::warning ||
               pending.kind == awj::BatchEventKind::large_image_queued;
      };
      const auto retained = std::ranges::remove_if(
          state.pending_events,
          [&](const awj::BatchProgress& pending) { return !keep(pending); });
      state.pending_events.erase(retained.begin(), retained.end());
      if (state.pending_events.size() >= kMaxPendingEvents) {
        state.pending_events.erase(
            state.pending_events.begin(),
            state.pending_events.begin() +
                static_cast<std::ptrdiff_t>(state.pending_events.size() -
                                            kMaxPendingEvents + 1));
      }
    }
    state.pending_events.push_back(event);
  } catch (...) {
  }
}

bool request_all_workers_stop_locked(UiState& state) noexcept {
  bool stop_requested = false;
  if (state.worker_active) {
    stop_requested = true;
  }
  if (state.active_child != nullptr) {
    state.active_child->request_cancel();
    stop_requested = true;
  }
  return stop_requested;
}

bool request_all_workers_stop(const std::shared_ptr<UiState>& state) noexcept {
  if (state == nullptr) {
    return false;
  }
  try {
    std::scoped_lock lock{state->mutex};
    return request_all_workers_stop_locked(*state);
  } catch (...) {
    return false;
  }
}

bool worker_active(const std::shared_ptr<UiState>& state) noexcept {
  if (state == nullptr) {
    return false;
  }
  try {
    std::scoped_lock lock{state->mutex};
    return state->worker_active;
  } catch (...) {
    return false;
  }
}

bool force_stop_current_worker(const std::shared_ptr<UiState>& state) noexcept {
  if (state == nullptr) {
    return false;
  }
  std::shared_ptr<StudioChildProcess> child;
  bool had_worker = false;
  try {
    std::scoped_lock lock{state->mutex};
    had_worker = state->worker_active || state->active_child != nullptr;
    child = state->active_child;
    request_all_workers_stop_locked(*state);
  } catch (...) {
    return false;
  }
  if (child != nullptr) {
    return child->terminate() || had_worker;
  }
  return had_worker;
}

std::wstring cli_output_format_arg(awj::OutputFormat format) {
  switch (format) {
    case awj::OutputFormat::webp:
      return L"webp";
    case awj::OutputFormat::jxl:
      return L"jxl";
    case awj::OutputFormat::avif:
    default:
      return L"avif";
  }
}

std::wstring cli_collision_arg(awj::CollisionMode mode) {
  switch (mode) {
    case awj::CollisionMode::skip:
      return L"skip";
    case awj::CollisionMode::suffix_time:
      return L"time";
    case awj::CollisionMode::suffix_random:
      return L"random";
    case awj::CollisionMode::overwrite:
    default:
      return L"overwrite";
  }
}

std::wstring cli_chroma_arg(awj::ChromaMode mode) {
  return awj::wide_from_utf8(awj::chroma_mode_name(mode));
}

std::wstring cli_avif_encoder_arg(awj::AvifEncoderMode mode) {
  return awj::wide_from_utf8(awj::avif_encoder_mode_name(mode));
}

std::wstring cli_alpha_arg(awj::AlphaModePolicy policy) {
  return awj::wide_from_utf8(awj::alpha_mode_policy_name(policy));
}

void push_cli_option(std::vector<std::wstring>& args, std::wstring option,
                     std::wstring value) {
  args.push_back(std::move(option));
  args.push_back(std::move(value));
}

void push_cli_option(std::vector<std::wstring>& args, std::wstring option,
                     const std::filesystem::path& value) {
  push_cli_option(args, std::move(option), value.native());
}

std::wstring bytes_argument(std::uint64_t bytes) {
  return bytes == 0 ? std::wstring{L"auto"} : std::format(L"{}b", bytes);
}

std::vector<std::wstring> cli_arguments_from_config(
    const awj::AppConfig& cfg, std::wstring_view cancel_event_name) {
  std::vector<std::wstring> args;
  args.reserve(80 + cfg.svtav1hdr_params.size() * 2);

  push_cli_option(args, L"--input", cfg.input_path);
  if (!cfg.output_dir.empty()) {
    push_cli_option(args, L"--output", cfg.output_dir);
  }
  push_cli_option(args, L"--format", cli_output_format_arg(cfg.output_format));
  push_cli_option(args, L"--template", cfg.output_template);
  push_cli_option(args, L"--threads", std::to_wstring(cfg.max_jobs));
  push_cli_option(args, L"--memory-limit", bytes_argument(cfg.memory_limit_bytes));
  push_cli_option(args, L"--timeout-encode",
                  std::to_wstring(cfg.encode_timeout_minutes));
  push_cli_option(args, L"--collision", cli_collision_arg(cfg.collision_mode));
  push_cli_option(args, L"--studio-cancel-event",
                  std::wstring{cancel_event_name});
  if (!cfg.studio_large_action.empty()) {
    push_cli_option(args, L"--studio-large-action", cfg.studio_large_action);
  }

  if (cfg.visual_quality) {
    push_cli_option(args, L"--visual-quality",
                    std::to_wstring(*cfg.visual_quality));
    args.push_back(cfg.visual_quality_fallback ? L"--visual-quality-fallback"
                                               : L"--no-visual-quality-fallback");
    args.push_back(cfg.visual_quality_gpu ? L"--visual-quality-gpu"
                                          : L"--no-visual-quality-gpu");
  } else {
    push_cli_option(args, L"--quality", std::to_wstring(cfg.quality));
  }

  if (cfg.bit_depth) {
    push_cli_option(args, L"--bit-depth", std::to_wstring(*cfg.bit_depth));
  }
  if (cfg.speed) {
    push_cli_option(args, L"--speed", std::to_wstring(*cfg.speed));
  }

  args.push_back(cfg.allow_wic_fallback ? L"--allow-wic-fallback"
                                        : L"--no-wic-fallback");
  args.push_back(cfg.enable_experimental_encoders ? L"--experimental-encoders"
                                                  : L"--no-experimental-encoders");
  args.push_back(cfg.experimental_clamped_grid_padding
                     ? L"--experimental-clamped-grid-padding"
                     : L"--no-experimental-clamped-grid-padding");
  args.push_back(cfg.strip_metadata ? L"--strip" : L"--keep-metadata");
  args.push_back(cfg.write_summary ? L"--summary" : L"--no-summary");
  args.push_back(cfg.write_log ? L"--log" : L"--no-log");

  if (cfg.output_format == awj::OutputFormat::avif) {
    push_cli_option(args, L"--avif-encoder",
                    cli_avif_encoder_arg(cfg.avif_encoder));
    push_cli_option(args, L"--chroma", cli_chroma_arg(cfg.chroma_mode));
    push_cli_option(args, L"--alpha", cli_alpha_arg(cfg.alpha_policy));
  }

  if (cfg.svtav1hdr_crf) {
    push_cli_option(args, L"--svtav1hdr-crf",
                    std::to_wstring(*cfg.svtav1hdr_crf));
  }
  if (cfg.svtav1hdr_preset) {
    push_cli_option(args, L"--svtav1hdr-preset",
                    std::to_wstring(*cfg.svtav1hdr_preset));
  }
  if (!cfg.svtav1hdr_tune.empty()) {
    push_cli_option(args, L"--svtav1hdr-tune",
                    awj::wide_from_utf8(cfg.svtav1hdr_tune));
  }
  if (cfg.svtav1hdr_keyint) {
    push_cli_option(args, L"--svtav1hdr-keyint",
                    std::to_wstring(*cfg.svtav1hdr_keyint));
  }
  for (const auto& param : cfg.svtav1hdr_params) {
    push_cli_option(args, L"--svtav1hdr-params", param);
  }
  if (cfg.color_primaries) {
    push_cli_option(args, L"--color-primaries",
                    std::to_wstring(*cfg.color_primaries));
  }
  if (cfg.transfer_characteristics) {
    push_cli_option(args, L"--transfer-characteristics",
                    std::to_wstring(*cfg.transfer_characteristics));
  }
  if (cfg.matrix_coefficients) {
    push_cli_option(args, L"--matrix-coefficients",
                    std::to_wstring(*cfg.matrix_coefficients));
  }
  if (cfg.color_range) {
    push_cli_option(args, L"--color-range", std::to_wstring(*cfg.color_range));
  }
  if (!cfg.mastering_display.empty()) {
    push_cli_option(args, L"--mastering-display", cfg.mastering_display);
  }
  if (!cfg.content_light.empty()) {
    push_cli_option(args, L"--content-light", cfg.content_light);
  }

  return args;
}

std::wstring quote_windows_command_arg(std::wstring_view arg) {
  if (arg.empty()) {
    return L"\"\"";
  }
  const bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needs_quotes) {
    return std::wstring{arg};
  }
  std::wstring quoted;
  quoted.reserve(arg.size() + 2);
  quoted.push_back(L'\"');
  std::size_t backslashes = 0;
  for (const wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

std::wstring command_line_from_args(std::span<const std::wstring> args) {
  std::wstring command;
  for (const auto& arg : args) {
    if (!command.empty()) {
      command.push_back(L' ');
    }
    command += quote_windows_command_arg(arg);
  }
  return command;
}

std::expected<std::shared_ptr<StudioChildProcess>, std::string>
start_studio_cli_worker(const awj::AppConfig& cfg, std::uint64_t run_id) {
  auto exe_dir = awj::executable_directory();
  if (!exe_dir) {
    return std::unexpected{exe_dir.error()};
  }
  const auto cli_path = *exe_dir / L"AWJ-cli.exe";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(cli_path, ec) || ec) {
    return std::unexpected{std::format("找不到 Studio 编码 worker: {}。",
                                       awj::display_path_for_user(cli_path))};
  }

  auto child = std::make_shared<StudioChildProcess>();
  const auto event_name = std::format(L"Local\\AWJStudioCancel-{}-{}",
                                      GetCurrentProcessId(), run_id);
  child->cancel_event.reset(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
  if (child->cancel_event == nullptr) {
    return std::unexpected{std::format("创建编码取消事件失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }

  auto args = cli_arguments_from_config(cfg, event_name);
  args.insert(args.begin(), cli_path.native());
  auto command_line = command_line_from_args(args);
  if (command_line.size() >= 32767) {
    return std::unexpected{"编码 worker 命令行超过 Windows 长度限制。"};
  }
  child->command_line = command_line;

  UniqueWin32Handle job{CreateJobObjectW(nullptr, nullptr)};
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
      job.reset();
    }
  }

  HANDLE pipe_read = nullptr;
  HANDLE pipe_write = nullptr;
  SECURITY_ATTRIBUTES pipe_security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                    .lpSecurityDescriptor = nullptr,
                                    .bInheritHandle = TRUE};
  if (!CreatePipe(&pipe_read, &pipe_write, &pipe_security, 0)) {
    return std::unexpected{std::format("创建编码 worker 输出管道失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }
  UniqueWin32Handle output_read{pipe_read};
  UniqueWin32Handle output_write{pipe_write};
  SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output_write.get();
  startup.hStdError = output_write.get();
  PROCESS_INFORMATION process{};
  auto mutable_command_line = command_line;
  const auto cwd = exe_dir->native();
  const BOOL created = CreateProcessW(
      cli_path.c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, cwd.c_str(),
      &startup, &process);
  if (!created) {
    return std::unexpected{std::format("启动编码 worker 失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }
  child->process.reset(process.hProcess);
  child->thread.reset(process.hThread);
  child->output_read = std::move(output_read);
  output_write.reset();
  child->process_id = process.dwProcessId;
  if (job != nullptr && AssignProcessToJobObject(job.get(), child->process.get())) {
    child->job = std::move(job);
  }
  return child;
}

void begin_system_window_drag(slint::Window& window) noexcept {
  try {
    const HWND hwnd = window.win32_hwnd();
    if (hwnd == nullptr) {
      return;
    }
    ::ReleaseCapture();
    ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
  } catch (...) {
  }
}

bool reject_when_worker_active(AwjStudio& app,
                               const std::shared_ptr<UiState>& state,
                               const char* message) {
  {
    std::scoped_lock lock{state->mutex};
    if (!state->worker_active) {
      return false;
    }
  }
  app.set_status_text(to_shared(message));
  return true;
}

void handle_drop_files(DropBridge& bridge, HDROP drop) noexcept {
  try {
    POINT point{};
    DragQueryPoint(drop, &point);
    auto paths = paths_from_hdrop(drop);
    if (paths.empty()) {
      return;
    }

    if (!post_to_ui(bridge.weak, [state = bridge.state, point,
                                  paths = std::move(paths)](AwjStudio& app) {
          if (reject_when_worker_active(app, state,
                                        "当前任务正在运行，无法拖拽导入")) {
            return;
          }
          if (!point_in_import_drop_zone(app, point)) {
            app.set_status_text(to_shared("请拖入到上方红框导入区域内。"));
            return;
          }
          apply_import_drop_paths(app, paths);
        })) {
      return;
    }
  } catch (...) {
  }
}

LRESULT CALLBACK drop_bridge_window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                         LPARAM lparam) {
  constexpr wchar_t kDropBridgeProperty[] = L"AWJImageDropBridge";
  auto* bridge =
      reinterpret_cast<DropBridge*>(GetPropW(hwnd, kDropBridgeProperty));
  if (message == WM_DROPFILES && bridge != nullptr) {
    const auto drop = reinterpret_cast<HDROP>(wparam);
    handle_drop_files(*bridge, drop);
    DragFinish(drop);
    return 0;
  }
  if (message == WM_NCHITTEST && bridge != nullptr) {
    const LRESULT previous_result =
        bridge->previous_proc != nullptr
            ? CallWindowProcW(bridge->previous_proc, hwnd, message, wparam,
                              lparam)
            : DefWindowProcW(hwnd, message, wparam, lparam);
    if (previous_result == HTCLIENT) {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (ScreenToClient(hwnd, &point)) {
        try {
          if (auto app = bridge->weak.lock();
              app && point_in_window_drag_zone(**app, point)) {
            return HTCAPTION;
          }
        } catch (...) {
        }
      }
    }
    return previous_result;
  }
  if (message == WM_NCDESTROY && bridge != nullptr) {
    const auto previous = bridge->previous_proc;
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
    RemovePropW(hwnd, kDropBridgeProperty);
    DragAcceptFiles(hwnd, FALSE);
    delete bridge;
    return CallWindowProcW(previous, hwnd, message, wparam, lparam);
  }
  if (bridge != nullptr && bridge->previous_proc != nullptr) {
    return CallWindowProcW(bridge->previous_proc, hwnd, message, wparam,
                           lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void install_drop_bridge(slint::Window& window,
                         slint::ComponentWeakHandle<AwjStudio> weak,
                         std::shared_ptr<UiState> state) noexcept {
  try {
    constexpr wchar_t kDropBridgeProperty[] = L"AWJImageDropBridge";
    const HWND hwnd = window.win32_hwnd();
    if (hwnd == nullptr) {
      return;
    }
    if (GetPropW(hwnd, kDropBridgeProperty) != nullptr) {
      return;
    }
    auto bridge = std::make_unique<DropBridge>();
    bridge->weak = std::move(weak);
    bridge->state = std::move(state);
    bridge->previous_proc =
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (bridge->previous_proc == nullptr) {
      return;
    }
    if (SetPropW(hwnd, kDropBridgeProperty, bridge.get()) == FALSE) {
      return;
    }
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(drop_bridge_window_proc));
    DragAcceptFiles(hwnd, TRUE);
    bridge.release();
  } catch (...) {
  }
}

void trim_process_working_set() {
  SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                           static_cast<SIZE_T>(-1));
}

LONG physical_extent_to_long(std::uint32_t value) noexcept {
  return static_cast<LONG>(std::min<std::uint32_t>(
      value, static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())));
}

LONG add_window_extent(LONG origin, LONG extent) noexcept {
  if (origin > std::numeric_limits<LONG>::max() - extent) {
    return std::numeric_limits<LONG>::max();
  }
  return origin + extent;
}

void constrain_window_to_work_area(slint::Window& window) {
  auto size = window.size();
  auto position = window.position();
  const LONG current_width = physical_extent_to_long(size.width);
  const LONG current_height = physical_extent_to_long(size.height);
  RECT current_rect{position.x, position.y,
                    add_window_extent(position.x, current_width),
                    add_window_extent(position.y, current_height)};

  HMONITOR monitor = MonitorFromRect(&current_rect, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{.cbSize = sizeof(MONITORINFO)};
  if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitor_info)) {
    return;
  }

  const RECT& work = monitor_info.rcWork;
  const auto work_width = std::max<LONG>(1, work.right - work.left);
  const auto work_height = std::max<LONG>(1, work.bottom - work.top);
  const auto clamped_width = std::min<std::uint32_t>(
      size.width, static_cast<std::uint32_t>(work_width));
  const auto clamped_height = std::min<std::uint32_t>(
      size.height, static_cast<std::uint32_t>(work_height));

  if (clamped_width != size.width || clamped_height != size.height) {
    size = slint::PhysicalSize{{clamped_width, clamped_height}};
    window.set_size(size);
  }

  const auto max_x = work.right - static_cast<LONG>(size.width);
  const auto max_y = work.bottom - static_cast<LONG>(size.height);
  const auto clamped_x =
      std::clamp<LONG>(position.x, work.left, std::max(work.left, max_x));
  const auto clamped_y =
      std::clamp<LONG>(position.y, work.top, std::max(work.top, max_y));
  if (clamped_x != position.x || clamped_y != position.y) {
    window.set_position(slint::PhysicalPosition{{clamped_x, clamped_y}});
  }
}

awj::CollisionMode collision_from_index(int index) {
  switch (index) {
    case 1:
      return awj::CollisionMode::skip;
    case 2:
      return awj::CollisionMode::suffix_time;
    case 3:
      return awj::CollisionMode::suffix_random;
    case 0:
    default:
      return awj::CollisionMode::overwrite;
  }
}

awj::OutputFormat output_format_from_index(int index) {
  switch (index) {
    case 1:
      return awj::OutputFormat::webp;
    case 2:
      return awj::OutputFormat::jxl;
    case 0:
    default:
      return awj::OutputFormat::avif;
  }
}

awj::ChromaMode chroma_from_index(int index) {
  switch (index) {
    case 1:
      return awj::ChromaMode::yuv444;
    case 2:
      return awj::ChromaMode::yuv422;
    case 3:
      return awj::ChromaMode::yuv420;
    case 0:
    default:
      return awj::ChromaMode::auto_keep;
  }
}

awj::AlphaModePolicy alpha_policy_from_index(int index) {
  switch (index) {
    case 0:
      return awj::AlphaModePolicy::force;
    case 2:
      return awj::AlphaModePolicy::off;
    case 1:
    default:
      return awj::AlphaModePolicy::automatic;
  }
}

awj::AvifEncoderMode avif_encoder_from_index(int index) {
  switch (index) {
    case 1:
      return awj::AvifEncoderMode::svt;
    case 2:
      return awj::AvifEncoderMode::aom;
    case 3:
      return awj::AvifEncoderMode::zenrav1e;
    case 0:
    default:
      return awj::AvifEncoderMode::automatic;
  }
}

awj::Preset preset_from_index(int index) {
  switch (index) {
    case 0:
      return awj::Preset::custom;
    case 1:
      return awj::Preset::lossless;
    case 2:
      return awj::Preset::visual_lossless;
    case 3:
      return awj::Preset::balanced;
    case 4:
      return awj::Preset::fast;
    case 5:
      return awj::Preset::fastest;
    default:
      return awj::Preset::balanced;
  }
}

std::optional<int> visual_quality_for_preset(awj::Preset preset) noexcept {
  switch (preset) {
    case awj::Preset::custom:
    case awj::Preset::lossless:
      return std::nullopt;
    case awj::Preset::visual_lossless:
      return 75;
    case awj::Preset::balanced:
      return 50;
    case awj::Preset::fast:
      return 25;
    case awj::Preset::fastest:
    default:
      return 17;
  }
}

void apply_preset_defaults_to_ui(AwjStudio& app, int preset_index) {
  const auto preset = preset_from_index(preset_index);
  if (preset == awj::Preset::custom) {
    return;
  }
  if (preset == awj::Preset::lossless) {
    app.set_quality_text(to_shared("100"));
    app.set_visual_quality_text({});
  } else if (const auto visual_quality = visual_quality_for_preset(preset)) {
    app.set_quality_text({});
    app.set_visual_quality_text(to_shared(text_from_int(*visual_quality)));
  }
  app.set_quality_follows_format(false);
}

void apply_format_defaults_to_ui(AwjStudio& app, int format_index) {
  refresh_avif_encoder_options(app);
  const auto format = output_format_from_index(format_index);
  if (app.get_quality_follows_format()) {
    app.set_quality_text(
        to_shared(text_from_int(awj::default_quality_for(format))));
  }
  if (format == awj::OutputFormat::webp) {
    app.set_bit_depth_text(to_shared(
        text_from_int(awj::encoding_defaults::default_webp_bit_depth)));
    app.set_chroma_index(0);
  } else {
    if (app.get_bit_depth_follows_format()) {
      app.set_bit_depth_text({});
    }
    if (format != awj::OutputFormat::avif) {
      app.set_chroma_index(0);
    }
  }
}

void initialize_ui_defaults(AwjStudio& app) {
  const auto defaults = awj::default_app_config();
  app.set_input_path({});
  app.set_input_mode_index(0);
  app.set_template_text(to_shared(text_from_wide(defaults.output_template)));
  app.set_quality_text(to_shared(text_from_int(defaults.quality)));
  app.set_avif_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::avif))));
  app.set_webp_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::webp))));
  app.set_jxl_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::jxl))));
  app.set_webp_bit_depth_default(
      to_shared(text_from_int(awj::encoding_defaults::default_webp_bit_depth)));
  app.set_memory_limit_text({});
  app.set_preset_index(3);
  app.set_visual_quality_text(to_shared("50"));
  app.set_format_index(0);
  app.set_experimental_encoders(defaults.enable_experimental_encoders);
  app.set_visual_quality_gpu(defaults.visual_quality_gpu);
  app.set_visual_quality_fallback(defaults.visual_quality_fallback);
  app.set_allow_wic_fallback(defaults.allow_wic_fallback);
  refresh_avif_encoder_options(app);
  app.set_avif_encoder_index(0);
  app.set_collision_index(0);
  app.set_chroma_index(0);
  app.set_alpha_policy_index(1);
  app.set_quality_follows_format(true);
  app.set_bit_depth_follows_format(true);
}

std::expected<awj::AppConfig, std::string> config_from_ui(
    const AwjStudio& app) try {
  awj::AppConfig cfg = awj::default_app_config();
  cfg.input_path = awj::wide_from_utf8(shared_to_string(app.get_input_path()));
  if (cfg.input_path.empty()) {
    return std::unexpected{"输入路径为空，请选择或输入一个图片文件/文件夹。"};
  }
  cfg.output_dir = awj::wide_from_utf8(shared_to_string(app.get_output_dir()));
  cfg.output_template =
      awj::wide_from_utf8(shared_to_string(app.get_template_text()));
  if (cfg.output_template.empty()) {
    cfg.output_template = awj::encoding_defaults::default_output_template;
  }
  cfg.allow_wic_fallback = app.get_allow_wic_fallback();
  cfg.preset = preset_from_index(app.get_preset_index());

  cfg.output_format = output_format_from_index(app.get_format_index());
  cfg.avif_encoder = cfg.output_format == awj::OutputFormat::avif
                         ? avif_encoder_from_index(app.get_avif_encoder_index())
                         : awj::AvifEncoderMode::automatic;
  cfg.enable_experimental_encoders = app.get_experimental_encoders();
  if (cfg.avif_encoder == awj::AvifEncoderMode::zenrav1e) {
    const auto capabilities = awj::avif_encoder_capabilities_for_current_build(
        cfg.enable_experimental_encoders);
    const auto it =
        std::ranges::find(capabilities, awj::AvifEncoderMode::zenrav1e,
                          &awj::AvifEncoderCapability::mode);
    if (it == capabilities.end() || !it->enabled ||
        (it->experimental && !it->feature_enabled)) {
      return std::unexpected{"zenrav1e 当前构建不可用，无法选择。"};
    }
  }
  cfg.collision_mode = collision_from_index(app.get_collision_index());

  const auto visual_quality = parse_visual_quality_field(
      shared_to_string(app.get_visual_quality_text()));
  if (!visual_quality) {
    return std::unexpected{visual_quality.error()};
  }
  cfg.visual_quality = *visual_quality;
  cfg.visual_quality_gpu = app.get_visual_quality_gpu();
  cfg.visual_quality_fallback =
      cfg.visual_quality.has_value() && app.get_visual_quality_fallback();

  std::optional<int> quality;
  if (!cfg.visual_quality) {
    const auto parsed_quality =
        parse_quality_field(shared_to_string(app.get_quality_text()));
    if (!parsed_quality) {
      return std::unexpected{parsed_quality.error()};
    }
    quality = *parsed_quality;
    cfg.quality = *quality;
  }

  const auto bit_depth =
      cfg.output_format == awj::OutputFormat::webp
          ? std::expected<std::optional<int>, std::string>{std::optional<int>{
                awj::encoding_defaults::default_webp_bit_depth}}
          : (cfg.output_format == awj::OutputFormat::jxl
                 ? std::expected<std::optional<int>,
                                 std::string>{std::optional<int>{}}
                 : parse_bit_depth_field(
                       shared_to_string(app.get_bit_depth_text())));
  if (!bit_depth) {
    return std::unexpected{bit_depth.error()};
  }
  cfg.bit_depth = *bit_depth;

  cfg.alpha_policy = cfg.output_format == awj::OutputFormat::avif
                         ? alpha_policy_from_index(app.get_alpha_policy_index())
                         : awj::AlphaModePolicy::automatic;
  cfg.chroma_mode = cfg.output_format == awj::OutputFormat::avif
                        ? chroma_from_index(app.get_chroma_index())
                        : awj::ChromaMode::auto_keep;

  const auto max_jobs =
      parse_jobs_field(shared_to_string(app.get_threads_text()));
  if (!max_jobs) {
    return std::unexpected{max_jobs.error()};
  }
  cfg.max_jobs = *max_jobs;

  const auto memory_limit =
      parse_memory_limit_field(shared_to_string(app.get_memory_limit_text()));
  if (!memory_limit) {
    return std::unexpected{memory_limit.error()};
  }
  cfg.memory_limit_bytes = *memory_limit;

  const auto speed = parse_optional_int_field(
      shared_to_string(app.get_speed_text()), "speed", 0, 10);
  if (!speed) {
    return std::unexpected{speed.error()};
  }
  cfg.speed = *speed;

  awj::apply_preset(cfg, cfg.preset);
  if (quality) {
    cfg.quality = *quality;
  }

  cfg.strip_metadata = app.get_strip_metadata();
  cfg.write_summary = app.get_write_summary();
  cfg.write_log = app.get_write_log();

  if (auto valid = awj::finalize_config_defaults(cfg, true, true); !valid) {
    return std::unexpected{valid.error()};
  }
  return cfg;
} catch (const std::bad_alloc&) {
  return std::unexpected{"Studio 配置解析内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"Studio 配置解析数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"Studio 配置解析文件系统访问失败。"};
}

std::optional<std::filesystem::path> choose_path(bool pick_folder) {
  ComApartment apartment;
  if (!apartment.usable()) {
    return std::nullopt;
  }

  // 文件选择器是 COM 对象，unique_ptr 的 deleter 让后续任何早退路径都能配对
  // Release。
  IFileDialog* raw_dialog = nullptr;
  HRESULT hr =
      CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(&raw_dialog));
  std::unique_ptr<IFileDialog, ComReleaseDeleter<IFileDialog>> dialog{
      raw_dialog};
  if (FAILED(hr) || dialog == nullptr) {
    return std::nullopt;
  }

  DWORD options{};
  hr = dialog->GetOptions(&options);
  if (FAILED(hr)) {
    return std::nullopt;
  }
  options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
  options |= pick_folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
  hr = dialog->SetOptions(options);
  if (FAILED(hr)) {
    return std::nullopt;
  }

  if (!pick_folder) {
    const COMDLG_FILTERSPEC filters[] = {
        {L"图片文件",
         L"*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.webp;*.bmp;*.dib;*.rle;*.tif;*."
         L"tiff;*.gif;*.jxl;*.avif;*.awsraw;*.dng;*.cr2;*.cr3;*.nef;*.arw;*."
         L"rw2;*.orf;*.raf;*.pef;*.srw;*.x3f;*.3fr;*.erf;*.kdc;*.mrw;*.raw;*."
         L"heic;*.heif;*.jxr;*.wdp;*.hdp"},
        {L"所有文件", L"*.*"}};
    hr = dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    if (FAILED(hr)) {
      return std::nullopt;
    }
  }

  hr = dialog->Show(nullptr);
  if (FAILED(hr)) {
    return std::nullopt;
  }

  IShellItem* raw_item = nullptr;
  hr = dialog->GetResult(&raw_item);
  std::unique_ptr<IShellItem, ComReleaseDeleter<IShellItem>> item{raw_item};
  if (FAILED(hr) || item == nullptr) {
    return std::nullopt;
  }

  PWSTR raw_path = nullptr;
  hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path);
  std::unique_ptr<wchar_t, CoTaskMemDeleter> path{raw_path};
  if (FAILED(hr) || path == nullptr) {
    return std::nullopt;
  }

  // SIGDN_FILESYSPATH 返回 CoTaskMemAlloc 缓冲区，复制进 filesystem::path
  // 后立即交还给 COM 分配器。
  return std::filesystem::path{path.get()};
}

std::filesystem::path effective_output_dir(const AwjStudio& app) {
  auto output = std::filesystem::path{
      awj::wide_from_utf8(shared_to_string(app.get_output_dir()))};
  if (!output.empty()) {
    return output;
  }
  const auto input = std::filesystem::path{
      awj::wide_from_utf8(shared_to_string(app.get_input_path()))};
  return awj::default_output_dir_for(input);
}

std::expected<void, std::string> open_path(std::filesystem::path path,
                                           bool create_if_missing) try {
  if (path.empty()) {
    return std::unexpected{"路径为空。"};
  }

  std::error_code ec;
  const bool regular_file = std::filesystem::is_regular_file(path, ec);
  if (ec) {
    return std::unexpected{std::format("无法判断路径类型: {}；系统错误：{}。",
                                       awj::display_path_for_user(path),
                                       ec.message())};
  }
  if (regular_file) {
    path = path.parent_path();
  }
  if (path.empty()) {
    auto current = std::filesystem::current_path(ec);
    if (ec) {
      return std::unexpected{
          std::format("无法定位当前目录；系统错误：{}。", ec.message())};
    }
    path = std::move(current);
  }
  if (create_if_missing) {
    std::filesystem::create_directories(path, ec);
    if (ec) {
      return std::unexpected{std::format("无法创建目录: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
  }

  bool exists = std::filesystem::exists(path, ec);
  if (ec) {
    return std::unexpected{std::format("无法检查路径: {}；系统错误：{}。",
                                       awj::display_path_for_user(path),
                                       ec.message())};
  }
  if (!exists) {
    const auto parent = path.parent_path();
    if (create_if_missing || parent.empty() || parent == path) {
      return std::unexpected{
          std::format("路径不存在: {}。", awj::display_path_for_user(path))};
    }
    path = parent;
    exists = std::filesystem::exists(path, ec);
    if (ec) {
      return std::unexpected{std::format("无法检查父目录: {}；系统错误：{}。",
                                         awj::display_path_for_user(path),
                                         ec.message())};
    }
    if (!exists) {
      return std::unexpected{
          std::format("父目录不存在: {}。", awj::display_path_for_user(path))};
    }
  }
  const bool directory = std::filesystem::is_directory(path, ec);
  if (ec) {
    return std::unexpected{std::format("无法判断目录类型: {}；系统错误：{}。",
                                       awj::display_path_for_user(path),
                                       ec.message())};
  }
  if (!directory) {
    return std::unexpected{
        std::format("路径不是目录: {}。", awj::display_path_for_user(path))};
  }

  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    return std::unexpected{
        std::format("无法打开目录: {}；ShellExecuteW 返回码：{}。",
                    awj::display_path_for_user(path), result)};
  }
  return {};
} catch (const std::bad_alloc&) {
  return std::unexpected{"打开路径内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"打开路径数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"打开路径文件系统访问失败。"};
}

void begin_child_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                const std::shared_ptr<UiState>& state,
                                awj::AppConfig cfg,
                                std::optional<int> large_index = std::nullopt);

void begin_child_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                const std::shared_ptr<UiState>& state,
                                awj::AppConfig cfg,
                                std::optional<int> large_index) {
  auto app = weak.lock();
  if (!app) {
    return;
  }

  std::uint64_t run_id{};
  std::shared_ptr<slint::VectorModel<TaskRow>> rows;
  std::shared_ptr<slint::VectorModel<LargeImageRow>> large_rows;
  {
    std::scoped_lock lock{state->mutex};
    if (state->worker_active) {
      (*app)->set_status_text(to_shared("当前任务正在运行，请先取消或强制终止"));
      return;
    }
    run_id = ++state->run_id;
    state->pending_events.clear();
    state->worker_active = true;
    state->active_child.reset();
    if (!large_index) {
      state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
      state->large_image_rows = std::make_shared<slint::VectorModel<LargeImageRow>>();
      state->large_image_items.clear();
      rows = state->task_rows;
      large_rows = state->large_image_rows;
    } else {
      set_large_image_status(*state, *large_index, "正在编码…");
      rows = state->task_rows;
      large_rows = state->large_image_rows;
    }
  }

  try {
    (*app)->set_running(true);
    (*app)->set_progress(0.0f);
    if (!large_index) {
      (*app)->set_task_rows(rows);
      (*app)->set_large_image_rows(large_rows);
      (*app)->set_selected_large_image_index(-1);
    }
    (*app)->set_status_text(to_shared("正在启动编码 worker…"));
  } catch (...) {
    reset_failed_run(**app, *state, run_id, "转换启动失败。");
    return;
  }

  auto child = start_studio_cli_worker(cfg, run_id);
  if (!child) {
    reset_failed_run(**app, *state, run_id,
                     std::format("启动编码 worker 失败：{}", child.error()));
    if (large_index) {
      set_large_image_status(*state, *large_index, "启动失败");
    }
    return;
  }
  try {
    (*app)->set_status_text(
        to_shared(std::format("编码 worker 已启动，PID {}", (*child)->process_id)));
  } catch (...) {
  }

  std::optional<std::jthread> worker;
  try {
    {
      std::scoped_lock lock{state->mutex};
      state->active_child = *child;
    }
    worker.emplace([weak, state, run_id, child = *child,
                    large_index](std::stop_token token) mutable {
      std::string pending;
      auto publish_line = [&](std::string line) {
        line = trim_copy(std::move(line));
        if (line.empty()) {
          return;
        }
        post_to_ui(weak, [state, run_id, line = std::move(line)](AwjStudio&) {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          append_log_row(state->task_rows, line);
        });
      };
      auto consume_output = [&] {
        if (child->output_read == nullptr) {
          return;
        }
        while (true) {
          DWORD available = 0;
          if (!PeekNamedPipe(child->output_read.get(), nullptr, 0, nullptr,
                             &available, nullptr) ||
              available == 0) {
            break;
          }
          std::array<char, 4096> buffer{};
          DWORD read = 0;
          if (!ReadFile(child->output_read.get(), buffer.data(),
                        static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available)),
                        &read, nullptr) ||
              read == 0) {
            break;
          }
          pending.append(buffer.data(), buffer.data() + read);
          std::size_t pos = 0;
          while ((pos = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
            pending.erase(0, pos + 1);
            publish_line(std::move(line));
          }
        }
      };

      DWORD exit_code = 1;
      while (!token.stop_requested()) {
        consume_output();
        const DWORD wait = WaitForSingleObject(child->process.get(), 80);
        if (wait == WAIT_OBJECT_0) {
          break;
        }
        if (wait != WAIT_TIMEOUT) {
          break;
        }
      }
      if (token.stop_requested() && child->process != nullptr) {
        child->terminate();
      }
      WaitForSingleObject(child->process.get(), INFINITE);
      consume_output();
      if (!pending.empty()) {
        publish_line(std::move(pending));
      }
      GetExitCodeProcess(child->process.get(), &exit_code);
      const bool forced = child->force_terminated.load(std::memory_order_acquire) ||
                          exit_code == kStudioWorkerForceStopExitCode;
      const bool canceled = child->cancel_requested.load(std::memory_order_acquire) &&
                            !forced;
      post_to_ui(weak, [state, run_id, large_index, exit_code, forced,
                        canceled](AwjStudio& app) {
              {
          std::scoped_lock lock{state->mutex};
          if (state->run_id != run_id) {
            return;
          }
          state->update_timer.stop();
          state->pending_events.clear();
          state->worker_active = false;
          state->active_child.reset();
                if (large_index) {
            set_large_image_status(
                *state, *large_index,
                exit_code == 0 ? "完成" : (forced ? "已强制终止"
                                                   : (canceled ? "已取消" : "失败")));
          }
              }
              try {
          app.set_running(false);
          app.set_progress(exit_code == 0 ? 1.0f : 0.0f);
          if (exit_code == 0) {
            app.set_status_text(to_shared(large_index ? "大图处理完成" : "完成"));
          } else if (forced) {
            app.set_status_text(to_shared("编码已强制终止，Studio 仍可继续使用"));
          } else if (canceled) {
            app.set_status_text(to_shared("已取消"));
          } else {
            app.set_status_text(
                to_shared(std::format("编码 worker 失败，退出码 {}", exit_code)));
          }
        } catch (...) {
        }
      });
    });
  } catch (const std::exception&) {
    force_stop_current_worker(state);
    reset_failed_run(**app, *state, run_id, "转换启动失败。");
    return;
  } catch (...) {
    force_stop_current_worker(state);
    reset_failed_run(**app, *state, run_id, "转换启动失败。");
    return;
  }
  {
    std::scoped_lock lock{state->mutex};
    state->worker = std::move(*worker);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  try {
    auto app = AwjStudio::create();
    auto state = std::make_shared<UiState>();
    state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
    state->large_image_rows =
        std::make_shared<slint::VectorModel<LargeImageRow>>();
    auto weak = slint::ComponentWeakHandle(app);

    app->set_task_rows(state->task_rows);
    app->set_large_image_rows(state->large_image_rows);
    initialize_ui_defaults(*app);
    app->set_threads_text({});
    app->set_selected_large_image_action_index(0);
    app->set_system_dark_mode(windows_prefers_dark_mode());
    sync_template_flags(*app);

    app->on_clear_tasks([weak, state] {
      run_ui_callback(weak, "清空任务失败", [&] {
        auto app = weak.lock();
        if (!app) {
          return;
        }
        if (reject_when_worker_active(**app, state,
                                      "当前任务正在运行，无法清空队列")) {
          return;
        }
        state->task_rows->set_vector({});
        state->large_image_rows->set_vector({});
        state->large_image_items.clear();
        (*app)->set_selected_large_image_index(-1);
        (*app)->set_selected_large_image_action_index(0);
        (*app)->set_progress(0.0f);
        (*app)->set_status_text(to_shared("就绪"));
      });
    });

    app->on_format_defaults_requested([weak, state](int index) {
      run_ui_callback(weak, "应用格式默认值失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(
                  **app, state, "当前任务正在运行，无法修改格式默认值")) {
            return;
          }
          apply_format_defaults_to_ui(**app, index);
        }
      });
    });

    app->on_preset_defaults_requested([weak, state](int index) {
      run_ui_callback(weak, "应用预设默认值失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法修改预设")) {
            return;
          }
          apply_preset_defaults_to_ui(**app, index);
        }
      });
    });

    app->on_toggle_template_token([weak, state](slint::SharedString token) {
      run_ui_callback(weak, "切换模板变量失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法修改命名规则")) {
            return;
          }
          toggle_template_token(**app, shared_to_string(token));
        }
      });
    });

    app->on_title_bar_theme_requested([weak](bool dark_mode) {
      try {
        if (auto app = weak.lock()) {
          apply_title_bar_theme((*app)->window(), dark_mode);
        }
      } catch (...) {
      }
    });

    app->on_begin_window_drag([weak] {
      try {
        if (auto app = weak.lock()) {
          begin_system_window_drag((*app)->window());
        }
      } catch (...) {
      }
    });

    state->theme_timer.start(
        slint::TimerMode::Repeated, std::chrono::seconds{3}, [weak] {
          run_ui_callback(weak, "更新主题状态失败", [&] {
            if (auto app = weak.lock()) {
              (*app)->set_system_dark_mode(windows_prefers_dark_mode());
            }
          });
        });

    app->on_browse_input([weak, state] {
      run_ui_callback(weak, "选择输入路径失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法选择输入路径")) {
            return;
          }
          const bool pick_folder = (*app)->get_input_mode_index() != 0;
          if (auto path = choose_path(pick_folder)) {
            post_to_ui(weak, [path = *path](AwjStudio& app) {
              set_input_path_preserving_output(app, path);
            });
          }
        }
      });
    });

    app->on_open_input([weak] {
      run_ui_callback(weak, "打开输入路径失败", [&] {
        if (auto app = weak.lock()) {
          const auto input = std::filesystem::path{
              awj::wide_from_utf8(shared_to_string((*app)->get_input_path()))};
          if (auto opened = open_path(input, false); !opened) {
            (*app)->set_status_text(
                to_shared(std::format("打开输入路径失败：{}", opened.error())));
          }
        }
      });
    });

    app->on_browse_output([weak, state] {
      run_ui_callback(weak, "选择输出路径失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法选择输出目录")) {
            return;
          }
          if (auto folder = choose_path(true)) {
            post_to_ui(weak, [folder = *folder](AwjStudio& app) {
              app.set_output_dir(to_shared(awj::path_to_utf8(folder)));
            });
          }
        }
      });
    });

    app->on_browse_large_image_file([weak, state] {
      run_ui_callback(weak, "添加大图文件失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法添加大图任务")) {
            return;
          }
          if (auto path = choose_path(false)) {
            add_manual_large_images_from_picker(**app, *state, *path, false);
          }
        }
      });
    });

    app->on_browse_large_image_folder([weak, state] {
      run_ui_callback(weak, "添加大图文件夹失败", [&] {
        if (auto app = weak.lock()) {
          if (reject_when_worker_active(**app, state,
                                        "当前任务正在运行，无法添加大图任务")) {
            return;
          }
          if (auto folder = choose_path(true)) {
            add_manual_large_images_from_picker(**app, *state, *folder, true);
          }
        }
      });
    });

    app->on_open_output([weak] {
      run_ui_callback(weak, "打开输出路径失败", [&] {
        if (auto app = weak.lock()) {
          if (auto opened = open_path(effective_output_dir(**app), true);
              !opened) {
            (*app)->set_status_text(
                to_shared(std::format("打开输出路径失败：{}", opened.error())));
          }
        }
      });
    });

    app->on_cancel_conversion([weak, state] {
      run_ui_callback(weak, "取消任务失败", [&] {
        const bool stop_requested = request_all_workers_stop(state);
        if (auto app = weak.lock()) {
          if (!stop_requested) {
            (*app)->set_running(false);
            (*app)->set_status_text(to_shared("没有正在运行的任务"));
            return;
          }
          (*app)->set_running(true);
          (*app)->set_status_text(to_shared("正在取消当前任务…"));
        }
      });
    });

    app->on_large_image_action_requested(
        [weak, state](int index, slint::SharedString action_text) {
          run_ui_callback(weak, "处理大图操作失败", [&] {
            auto app = weak.lock();
            if (!app) {
              return;
            }
            const auto action = shared_to_string(action_text);
            awj::BatchLargeImageItem item{};
            {
              std::scoped_lock lock{state->mutex};
              if (index < 0 || static_cast<std::size_t>(index) >=
                                   state->large_image_items.size()) {
                (*app)->set_status_text(to_shared("未选择大图任务"));
                return;
              }
              if (state->worker_active) {
                (*app)->set_status_text(
                    to_shared("当前任务正在终止，请稍后再处理大图"));
                return;
              }
              item = state->large_image_items[static_cast<std::size_t>(index)];
            }
            auto cfg = config_from_ui(**app);
            if (!cfg) {
              (*app)->set_status_text(
                  to_shared(std::format("配置错误：{}", cfg.error())));
              return;
            }
            (*cfg).studio_large_action = awj::wide_from_utf8(action);
            (*cfg).visual_quality.reset();
            begin_child_conversion_run(weak, state, std::move(*cfg), index);
          });
        });

    app->on_start_conversion([weak, state] {
      run_ui_callback(weak, "启动转换失败", [&] {
        auto app = weak.lock();
        if (!app) {
          return;
        }

        if (worker_active(state)) {
          const bool stopped = force_stop_current_worker(state);
          (*app)->set_running(true);
          (*app)->set_status_text(to_shared(
              stopped ? "正在强制终止当前编码任务…"
                      : "没有正在运行的编码任务"));
          return;
        }

        auto cfg = config_from_ui(**app);
        if (!cfg) {
          (*app)->set_running(false);
          (*app)->set_status_text(
              to_shared(std::format("配置错误：{}", cfg.error())));
          state->task_rows->set_vector({});
          state->large_image_rows->set_vector({});
          state->large_image_items.clear();
          (*app)->set_selected_large_image_index(-1);
          return;
        }

        begin_child_conversion_run(weak, state, std::move(*cfg));
      });
    });

    app->show();
    apply_title_bar_theme(app->window(), effective_studio_dark_mode(*app));
    constrain_window_to_work_area(app->window());
    install_drop_bridge(app->window(), weak, state);
    app->window().on_close_requested([state] {
      if (worker_active(state)) {
        force_stop_current_worker(state);
      }
      return slint::CloseRequestResponse::HideWindow;
    });
    app->run();
    std::jthread worker;
    {
      std::scoped_lock lock{state->mutex};
      worker = std::move(state->worker);
    }
    if (worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
    return 0;
  } catch (const std::exception&) {
    MessageBoxW(nullptr, L"Studio 启动失败。", L"AWJ-studio",
                MB_OK | MB_ICONERROR);
    return 1;
  } catch (...) {
    MessageBoxW(nullptr, L"Studio 启动失败：未知异常。", L"AWJ-studio",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
