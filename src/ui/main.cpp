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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
constexpr std::string_view kStudioConfigFileName = "AWJ.jsonc";

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

enum class QueueItemStatus {
  pending,
  running,
  done,
  failed,
  skipped,
  canceled
};

struct QueueImageItem {
  std::uint64_t id{};
  std::filesystem::path path{};
  std::filesystem::path source_root{};
  std::filesystem::path relative_dir{};
  std::uintmax_t bytes{};
  QueueItemStatus status{QueueItemStatus::pending};
  std::size_t run_index{std::numeric_limits<std::size_t>::max()};
  std::filesystem::path locked_output_path{};
  std::string status_text{"等待编码"};
  std::string log_text{};
  bool warning{};
};

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

struct StudioConfigSnapshot {
  int input_mode_index{};
  int format_index{};
  int preset_index{};
  int avif_encoder_index{};
  int chroma_index{};
  int jpegli_progressive_index{};
  int alpha_policy_index{};
  int collision_index{};
  int theme_index{};
  int selected_large_image_action_index{};
  bool experimental_encoders{};
  bool visual_quality_gpu{};
  bool visual_quality_fallback{};
  bool allow_wic_fallback{};
  bool jpegli_optimize_huffman{};
  bool jpegli_xyb{};
  bool strip_metadata{};
  bool write_summary{};
  bool write_log{};
  std::string quality_text{};
  std::string visual_quality_text{};
  std::string bit_depth_text{};
  std::string threads_text{};
  std::string memory_limit_text{};
  std::string speed_text{};
  std::string template_text{};

  bool operator==(const StudioConfigSnapshot&) const = default;
};

struct UiState {
  std::jthread worker{};
  std::shared_ptr<slint::VectorModel<TaskRow>> task_rows{};
  std::shared_ptr<slint::VectorModel<LargeImageRow>> large_image_rows{};
  std::vector<QueueImageItem> queue_items{};
  std::vector<awj::BatchLargeImageItem> large_image_items{};
  slint::Timer theme_timer{};
  slint::Timer update_timer{};
  slint::Timer config_timer{};
  std::optional<StudioConfigSnapshot> config_defaults{};
  std::optional<StudioConfigSnapshot> last_config_snapshot{};
  std::uint64_t run_id{};
  std::uint64_t next_queue_id{1};
  std::mutex mutex{};
  std::vector<awj::BatchProgress> pending_events{};
  std::shared_ptr<StudioChildProcess> active_child{};
  bool worker_active{};
  std::uint64_t drag_candidate_id{};
  std::uint64_t last_click_id{};
  std::chrono::steady_clock::time_point drag_started{};
  std::chrono::steady_clock::time_point last_click_time{};
  bool drag_reordered{};
};

LargeImageRow make_large_image_row(const awj::BatchLargeImageItem& item,
                                   std::string_view status);
int preferred_large_image_action_index(
    const awj::BatchLargeImageItem& item) noexcept;

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

int CALLBACK enum_font_family_proc(const LOGFONTW*, const TEXTMETRICW*, DWORD,
                                   LPARAM param) {
  *reinterpret_cast<bool*>(param) = true;
  return 0;
}

bool system_font_available(std::wstring_view family) noexcept {
  if (family.empty()) {
    return false;
  }
  HDC dc = GetDC(nullptr);
  if (dc == nullptr) {
    return false;
  }
  LOGFONTW query{};
  query.lfCharSet = DEFAULT_CHARSET;
  const auto length =
      std::min<std::size_t>(family.size(), std::size(query.lfFaceName) - 1);
  std::copy_n(family.begin(), length, query.lfFaceName);
  bool found = false;
  EnumFontFamiliesExW(dc, &query, enum_font_family_proc,
                      reinterpret_cast<LPARAM>(&found), 0);
  ReleaseDC(nullptr, dc);
  return found;
}

std::string select_system_ui_font_family() {
  const std::array<std::wstring_view, 8> candidates{
      L"鸿蒙黑体",       L"HarmonyOS Sans SC", L"Microsoft YaHei UI",
      L"Microsoft YaHei", L"微软雅黑",          L"Segoe UI",
      L"SimHei",         L"SimSun"};
  for (const auto family : candidates) {
    if (system_font_available(family)) {
      return awj::utf8_from_wide(family);
    }
  }
  return {};
}

void apply_system_ui_font(AwjStudio& app) {
  const auto family = select_system_ui_font_family();
  if (family.empty()) {
    app.set_ui_font_family({});
    app.set_ui_font_label(to_shared("系统默认字体"));
    return;
  }
  app.set_ui_font_family(to_shared(family));
  app.set_ui_font_label(to_shared(std::format("{}（系统）", family)));
}

std::filesystem::path studio_config_path() {
  if (auto directory = awj::executable_directory()) {
    return *directory / awj::wide_from_utf8(std::string{kStudioConfigFileName});
  }
  return {};
}

StudioConfigSnapshot capture_studio_config(const AwjStudio& app) {
  return StudioConfigSnapshot{
      .input_mode_index = app.get_input_mode_index(),
      .format_index = app.get_format_index(),
      .preset_index = app.get_preset_index(),
      .avif_encoder_index = app.get_avif_encoder_index(),
      .chroma_index = app.get_chroma_index(),
      .jpegli_progressive_index = app.get_jpegli_progressive_index(),
      .alpha_policy_index = app.get_alpha_policy_index(),
      .collision_index = app.get_collision_index(),
      .theme_index = app.get_theme_index(),
      .selected_large_image_action_index =
          app.get_selected_large_image_action_index(),
      .experimental_encoders = app.get_experimental_encoders(),
      .visual_quality_gpu = app.get_visual_quality_gpu(),
      .visual_quality_fallback = app.get_visual_quality_fallback(),
      .allow_wic_fallback = app.get_allow_wic_fallback(),
      .jpegli_optimize_huffman = app.get_jpegli_optimize_huffman(),
      .jpegli_xyb = app.get_jpegli_xyb(),
      .strip_metadata = app.get_strip_metadata(),
      .write_summary = app.get_write_summary(),
      .write_log = app.get_write_log(),
      .quality_text = shared_to_string(app.get_quality_text()),
      .visual_quality_text = shared_to_string(app.get_visual_quality_text()),
      .bit_depth_text = shared_to_string(app.get_bit_depth_text()),
      .threads_text = shared_to_string(app.get_threads_text()),
      .memory_limit_text = shared_to_string(app.get_memory_limit_text()),
      .speed_text = shared_to_string(app.get_speed_text()),
      .template_text = shared_to_string(app.get_template_text())};
}

std::string strip_jsonc_comments(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (in_string) {
      out.push_back(ch);
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
      out.push_back(ch);
      continue;
    }
    if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') {
        ++i;
      }
      if (i < text.size()) {
        out.push_back('\n');
      }
      continue;
    }
    if (ch == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        out.push_back(text[i] == '\n' ? '\n' : ' ');
        ++i;
      }
      if (i + 1 < text.size()) {
        ++i;
      }
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

struct JsonConfigValue {
  enum class Kind { boolean, integer, string };
  Kind kind{};
  bool boolean{};
  int integer{};
  std::string string{};
};

void skip_json_ws(std::string_view text, std::size_t& pos) noexcept {
  while (pos < text.size() &&
         std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
}

std::expected<std::string, std::string> parse_json_string(std::string_view text,
                                                          std::size_t& pos) {
  if (pos >= text.size() || text[pos] != '"') {
    return std::unexpected{"期望字符串。"};
  }
  ++pos;
  std::string out;
  while (pos < text.size()) {
    const char ch = text[pos++];
    if (ch == '"') {
      return out;
    }
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (pos >= text.size()) {
      return std::unexpected{"字符串转义不完整。"};
    }
    const char esc = text[pos++];
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        out.push_back(esc);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        return std::unexpected{"不支持的字符串转义。"};
    }
  }
  return std::unexpected{"字符串未闭合。"};
}

std::expected<JsonConfigValue, std::string> parse_json_value(
    std::string_view text, std::size_t& pos) {
  skip_json_ws(text, pos);
  if (pos >= text.size()) {
    return std::unexpected{"配置值不完整。"};
  }
  if (text[pos] == '"') {
    auto value = parse_json_string(text, pos);
    if (!value) {
      return std::unexpected{value.error()};
    }
    return JsonConfigValue{.kind = JsonConfigValue::Kind::string,
                           .string = std::move(*value)};
  }
  if (text.substr(pos, 4) == "true") {
    pos += 4;
    return JsonConfigValue{.kind = JsonConfigValue::Kind::boolean,
                           .boolean = true};
  }
  if (text.substr(pos, 5) == "false") {
    pos += 5;
    return JsonConfigValue{.kind = JsonConfigValue::Kind::boolean};
  }
  const std::size_t start = pos;
  if (text[pos] == '-') {
    ++pos;
  }
  while (pos < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  if (pos == start || (pos == start + 1 && text[start] == '-')) {
    return std::unexpected{"配置值只支持布尔、整数或字符串。"};
  }
  const auto parsed = scn::scan_int<int>(text.substr(start, pos - start));
  if (!parsed) {
    return std::unexpected{"整数配置值无效。"};
  }
  return JsonConfigValue{.kind = JsonConfigValue::Kind::integer,
                         .integer = parsed->value()};
}

std::expected<std::unordered_map<std::string, JsonConfigValue>, std::string>
parse_jsonc_config(std::string_view source) {
  const auto text = strip_jsonc_comments(source);
  std::string_view view{text};
  std::unordered_map<std::string, JsonConfigValue> values;
  std::size_t pos = 0;
  skip_json_ws(view, pos);
  if (pos >= view.size()) {
    return values;
  }
  if (view[pos++] != '{') {
    return std::unexpected{"配置文件根节点必须是对象。"};
  }
  while (true) {
    skip_json_ws(view, pos);
    if (pos < view.size() && view[pos] == '}') {
      ++pos;
      break;
    }
    auto key = parse_json_string(view, pos);
    if (!key) {
      return std::unexpected{key.error()};
    }
    skip_json_ws(view, pos);
    if (pos >= view.size() || view[pos++] != ':') {
      return std::unexpected{"配置项缺少冒号。"};
    }
    auto value = parse_json_value(view, pos);
    if (!value) {
      return std::unexpected{value.error()};
    }
    values.insert_or_assign(std::move(*key), std::move(*value));
    skip_json_ws(view, pos);
    if (pos < view.size() && view[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < view.size() && view[pos] == '}') {
      ++pos;
      break;
    }
    return std::unexpected{"配置项之间缺少逗号。"};
  }
  skip_json_ws(view, pos);
  if (pos != view.size()) {
    return std::unexpected{"配置对象后存在多余内容。"};
  }
  return values;
}

std::expected<int, std::string> config_int(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, int minimum, int maximum) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::integer) {
    return std::unexpected{std::format("{} 必须是整数。", key)};
  }
  if (it->second.integer < minimum || it->second.integer > maximum) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", key, minimum, maximum)};
  }
  return it->second.integer;
}

std::expected<bool, std::string> config_bool(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::boolean) {
    return std::unexpected{std::format("{} 必须是布尔值。", key)};
  }
  return it->second.boolean;
}

std::expected<std::string, std::string> config_string(
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key) {
  const auto it = values.find(std::string{key});
  if (it == values.end()) {
    return std::unexpected{""};
  }
  if (it->second.kind != JsonConfigValue::Kind::string) {
    return std::unexpected{std::format("{} 必须是字符串。", key)};
  }
  return it->second.string;
}

template <class Setter>
std::expected<void, std::string> apply_config_int(
    AwjStudio& app,
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, int minimum, int maximum, Setter setter) {
  auto value = config_int(values, key, minimum, maximum);
  if (!value) {
    return value.error().empty() ? std::expected<void, std::string>{}
                                 : std::unexpected{value.error()};
  }
  (app.*setter)(*value);
  return {};
}

template <class Setter>
std::expected<void, std::string> apply_config_bool(
    AwjStudio& app,
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, Setter setter) {
  auto value = config_bool(values, key);
  if (!value) {
    return value.error().empty() ? std::expected<void, std::string>{}
                                 : std::unexpected{value.error()};
  }
  (app.*setter)(*value);
  return {};
}

template <class Setter>
std::expected<void, std::string> apply_config_string(
    AwjStudio& app,
    const std::unordered_map<std::string, JsonConfigValue>& values,
    std::string_view key, Setter setter) {
  auto value = config_string(values, key);
  if (!value) {
    return value.error().empty() ? std::expected<void, std::string>{}
                                 : std::unexpected{value.error()};
  }
  (app.*setter)(to_shared(*value));
  return {};
}

std::expected<void, std::string> apply_studio_config_file(AwjStudio& app) {
  const auto path = studio_config_path();
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return {};
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected{"无法读取 Studio 配置文件。"};
  }
  std::string source{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
  auto values = parse_jsonc_config(source);
  if (!values) {
    return std::unexpected{values.error()};
  }

  const auto apply = [&](auto result) -> std::expected<void, std::string> {
    if (!result) {
      return std::unexpected{result.error()};
    }
    return {};
  };

  if (auto result = apply(apply_config_int(app, *values, "input_mode_index", 0,
                                           1, &AwjStudio::set_input_mode_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(app, *values, "format_index", 0, 3,
                                           &AwjStudio::set_format_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(app, *values, "preset_index", 0, 5,
                                           &AwjStudio::set_preset_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(
          app, *values, "avif_encoder_index", 0, 3,
          &AwjStudio::set_avif_encoder_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(app, *values, "chroma_index", 0, 3,
                                           &AwjStudio::set_chroma_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(
          app, *values, "jpegli_progressive_index", 0, 2,
          &AwjStudio::set_jpegli_progressive_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(
          app, *values, "alpha_policy_index", 0, 2,
          &AwjStudio::set_alpha_policy_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(app, *values, "collision_index", 0,
                                           3, &AwjStudio::set_collision_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(app, *values, "theme_index", 0, 2,
                                           &AwjStudio::set_theme_index));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_int(
          app, *values, "selected_large_image_action_index", 0, 1,
          &AwjStudio::set_selected_large_image_action_index));
      !result) {
    return result;
  }

  if (auto result = apply(apply_config_bool(
          app, *values, "experimental_encoders",
          &AwjStudio::set_experimental_encoders));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "visual_quality_gpu",
          &AwjStudio::set_visual_quality_gpu));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "visual_quality_fallback",
          &AwjStudio::set_visual_quality_fallback));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "allow_wic_fallback",
          &AwjStudio::set_allow_wic_fallback));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(
          app, *values, "jpegli_optimize_huffman",
          &AwjStudio::set_jpegli_optimize_huffman));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(app, *values, "jpegli_xyb",
                                            &AwjStudio::set_jpegli_xyb));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(app, *values, "strip_metadata",
                                            &AwjStudio::set_strip_metadata));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(app, *values, "write_summary",
                                            &AwjStudio::set_write_summary));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_bool(app, *values, "write_log",
                                            &AwjStudio::set_write_log));
      !result) {
    return result;
  }

  if (auto result = apply(apply_config_string(app, *values, "quality_text",
                                              &AwjStudio::set_quality_text));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_string(
          app, *values, "visual_quality_text",
          &AwjStudio::set_visual_quality_text));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_string(app, *values, "bit_depth_text",
                                              &AwjStudio::set_bit_depth_text));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_string(app, *values, "threads_text",
                                              &AwjStudio::set_threads_text));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_string(
          app, *values, "memory_limit_text",
          &AwjStudio::set_memory_limit_text));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_string(app, *values, "speed_text",
                                              &AwjStudio::set_speed_text));
      !result) {
    return result;
  }
  if (auto result = apply(apply_config_string(app, *values, "template_text",
                                              &AwjStudio::set_template_text));
      !result) {
    return result;
  }
  return {};
}

std::string json_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

void append_json_config_line(std::vector<std::string>& lines,
                             std::string_view key, std::string value) {
  lines.push_back(std::format("  \"{}\": {}", key, value));
}

std::expected<void, std::string> write_studio_config_file(
    const StudioConfigSnapshot& current,
    const StudioConfigSnapshot& defaults) try {
  const auto path = studio_config_path();
  if (path.empty()) {
    return std::unexpected{"无法定位程序同目录配置文件路径。"};
  }
  std::vector<std::string> lines;
  const auto add_int = [&](std::string_view key, int value, int fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key, std::format("{}", value));
    }
  };
  const auto add_bool = [&](std::string_view key, bool value, bool fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key, value ? "true" : "false");
    }
  };
  const auto add_string = [&](std::string_view key, const std::string& value,
                              const std::string& fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key,
                              std::format("\"{}\"", json_escape(value)));
    }
  };

  add_int("input_mode_index", current.input_mode_index,
          defaults.input_mode_index);
  add_int("format_index", current.format_index, defaults.format_index);
  add_int("preset_index", current.preset_index, defaults.preset_index);
  add_int("avif_encoder_index", current.avif_encoder_index,
          defaults.avif_encoder_index);
  add_int("chroma_index", current.chroma_index, defaults.chroma_index);
  add_int("jpegli_progressive_index", current.jpegli_progressive_index,
          defaults.jpegli_progressive_index);
  add_int("alpha_policy_index", current.alpha_policy_index,
          defaults.alpha_policy_index);
  add_int("collision_index", current.collision_index, defaults.collision_index);
  add_int("theme_index", current.theme_index, defaults.theme_index);
  add_int("selected_large_image_action_index",
          current.selected_large_image_action_index,
          defaults.selected_large_image_action_index);

  add_bool("experimental_encoders", current.experimental_encoders,
           defaults.experimental_encoders);
  add_bool("visual_quality_gpu", current.visual_quality_gpu,
           defaults.visual_quality_gpu);
  add_bool("visual_quality_fallback", current.visual_quality_fallback,
           defaults.visual_quality_fallback);
  add_bool("allow_wic_fallback", current.allow_wic_fallback,
           defaults.allow_wic_fallback);
  add_bool("jpegli_optimize_huffman", current.jpegli_optimize_huffman,
           defaults.jpegli_optimize_huffman);
  add_bool("jpegli_xyb", current.jpegli_xyb, defaults.jpegli_xyb);
  add_bool("strip_metadata", current.strip_metadata, defaults.strip_metadata);
  add_bool("write_summary", current.write_summary, defaults.write_summary);
  add_bool("write_log", current.write_log, defaults.write_log);

  add_string("quality_text", current.quality_text, defaults.quality_text);
  add_string("visual_quality_text", current.visual_quality_text,
             defaults.visual_quality_text);
  add_string("bit_depth_text", current.bit_depth_text,
             defaults.bit_depth_text);
  add_string("threads_text", current.threads_text, defaults.threads_text);
  add_string("memory_limit_text", current.memory_limit_text,
             defaults.memory_limit_text);
  add_string("speed_text", current.speed_text, defaults.speed_text);
  add_string("template_text", current.template_text, defaults.template_text);

  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    return std::unexpected{"无法写入程序同目录配置文件。"};
  }
  output << "{\n";
  output << "  // AWJ Studio runtime config. Only values that differ from "
            "built-in defaults are written.\n";
  for (std::size_t i = 0; i < lines.size(); ++i) {
    output << lines[i];
    if (i + 1 < lines.size()) {
      output << ',';
    }
    output << '\n';
  }
  output << "}\n";
  if (!output) {
    return std::unexpected{"程序同目录配置文件写入不完整。"};
  }
  return {};
} catch (const std::bad_alloc&) {
  return std::unexpected{"写入 Studio 配置时内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"写入 Studio 配置时数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"写入 Studio 配置时发生文件系统错误。"};
}

std::string queue_status_label(QueueItemStatus status) {
  switch (status) {
    case QueueItemStatus::running:
      return "正在编码";
    case QueueItemStatus::done:
      return "完成";
    case QueueItemStatus::failed:
      return "失败";
    case QueueItemStatus::skipped:
      return "已跳过";
    case QueueItemStatus::canceled:
      return "已取消";
    case QueueItemStatus::pending:
    default:
      return "等待编码";
  }
}

bool queue_item_editable(const QueueImageItem& item) noexcept {
  return item.status == QueueItemStatus::pending;
}

TaskRow make_queue_task_row(const QueueImageItem& item, std::size_t order) {
  const auto folder = item.path.parent_path();
  const auto output =
      item.locked_output_path.empty()
          ? std::string{}
          : awj::path_to_utf8(item.locked_output_path.filename());
  const auto status = item.status_text.empty()
                          ? queue_status_label(item.status)
                          : item.status_text;
  return TaskRow{.order = to_shared(std::format("{}", order + 1)),
                 .filename = to_shared(awj::path_to_utf8(item.path.filename())),
                 .folder = to_shared(awj::path_to_utf8(folder)),
                 .size = to_shared(awj::format_size(item.bytes)),
                 .status = to_shared(status),
                 .output = to_shared(output),
                 .log = to_shared(item.log_text),
                 .warning = item.warning,
                 .locked = !queue_item_editable(item)};
}

void refresh_queue_rows(AwjStudio& app, UiState& state) {
  std::vector<TaskRow> rows;
  rows.reserve(state.queue_items.size());
  for (const auto i : std::views::iota(std::size_t{}, state.queue_items.size())) {
    rows.push_back(make_queue_task_row(state.queue_items[i], i));
  }
  state.task_rows->set_vector(std::move(rows));
  app.set_task_rows(state.task_rows);
}

std::optional<std::size_t> queue_index_for_id(const UiState& state,
                                              std::uint64_t id) noexcept {
  for (const auto i : std::views::iota(std::size_t{}, state.queue_items.size())) {
    if (state.queue_items[i].id == id) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> queue_index_for_run_index(
    const UiState& state, std::size_t run_index) noexcept {
  for (const auto i : std::views::iota(std::size_t{}, state.queue_items.size())) {
    if (state.queue_items[i].run_index == run_index) {
      return i;
    }
  }
  return std::nullopt;
}

bool move_queue_item(UiState& state, std::size_t from, std::size_t to) {
  if (from >= state.queue_items.size() || to >= state.queue_items.size() ||
      from == to || !queue_item_editable(state.queue_items[from])) {
    return false;
  }
  if (!queue_item_editable(state.queue_items[to])) {
    return false;
  }
  auto item = std::move(state.queue_items[from]);
  state.queue_items.erase(state.queue_items.begin() +
                          static_cast<std::ptrdiff_t>(from));
  if (from < to) {
    --to;
  }
  state.queue_items.insert(
      state.queue_items.begin() + static_cast<std::ptrdiff_t>(to),
      std::move(item));
  return true;
}

std::size_t first_pending_index(const UiState& state) noexcept {
  for (const auto i : std::views::iota(std::size_t{}, state.queue_items.size())) {
    if (queue_item_editable(state.queue_items[i])) {
      return i;
    }
  }
  return state.queue_items.size();
}

std::size_t last_pending_index(const UiState& state) noexcept {
  for (std::size_t i = state.queue_items.size(); i > 0; --i) {
    if (queue_item_editable(state.queue_items[i - 1])) {
      return i - 1;
    }
  }
  return state.queue_items.size();
}

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
      combo_option("自动"),
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

std::wstring queue_path_key(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return awj::normalized_lower_path_key(ec ? path : absolute);
}

bool queue_contains_path(const UiState& state,
                         const std::filesystem::path& path) {
  const auto key = queue_path_key(path);
  return std::ranges::any_of(state.queue_items, [&](const QueueImageItem& item) {
    return queue_path_key(item.path) == key;
  });
}

std::filesystem::path queue_relative_dir_for(
    const std::filesystem::path& root, const std::filesystem::path& path) {
  std::error_code ec;
  const auto relative = std::filesystem::relative(path.parent_path(), root, ec);
  if (ec || relative.empty() || relative == L".") {
    return {};
  }
  return relative;
}

std::expected<bool, std::string> append_queue_image_path(
    UiState& state, const std::filesystem::path& path,
    const std::filesystem::path& source_root) {
  try {
    if (queue_contains_path(state, path)) {
      return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
      return std::unexpected{std::format("不是可加入队列的文件: {}。",
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
    if (bytes > static_cast<std::uintmax_t>(
                    awj::encoding_defaults::max_input_file_bytes)) {
      return std::unexpected{std::format("输入文件超过 20 GiB 上限: {}。",
                                         awj::display_path_for_user(path))};
    }
    state.queue_items.push_back(QueueImageItem{
        .id = state.next_queue_id++,
        .path = path,
        .source_root = source_root,
        .relative_dir =
            source_root.empty() ? std::filesystem::path{}
                                : queue_relative_dir_for(source_root, path),
        .bytes = bytes});
    return true;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"添加队列项时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"添加队列项时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"添加队列项时文件系统访问失败。"};
  }
}

void add_queue_from_path(AwjStudio& app, UiState& state,
                         const std::filesystem::path& picked,
                         bool pick_folder) {
  std::error_code ec;
  const bool is_folder = pick_folder ||
                         (std::filesystem::is_directory(picked, ec) && !ec);
  std::vector<std::filesystem::path> paths;
  if (is_folder) {
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

  set_input_path_preserving_output(app, picked);
  std::size_t added = 0;
  std::size_t skipped = 0;
  std::size_t failed = 0;
  std::string first_error;
  const auto source_root = is_folder ? picked : std::filesystem::path{};
  for (const auto& path : paths) {
    auto appended = append_queue_image_path(state, path, source_root);
    if (appended && *appended) {
      ++added;
    } else if (appended) {
      ++skipped;
    } else {
      ++failed;
      if (first_error.empty()) {
        first_error = appended.error();
      }
    }
  }
  refresh_queue_rows(app, state);
  if (added == 0) {
    app.set_status_text(
        to_shared(first_error.empty()
                      ? std::format("没有新图片加入队列{}。",
                                    skipped > 0 ? "，重复项已跳过" : "")
                      : first_error));
    return;
  }
  app.set_status_text(to_shared(std::format(
      "已加入 {} 张图片{}{}。", added,
      skipped == 0 ? "" : std::format("，跳过 {} 个重复项", skipped),
      failed == 0 ? "" : std::format("，{} 个失败", failed))));
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
    std::string output_format = result.output_format;
    if (output_format.empty()) {
      auto inferred = awj::OutputFormat::avif;
      auto ext = result.output_path.extension().wstring();
      std::ranges::transform(ext, ext.begin(),
                             [](wchar_t ch) { return std::towlower(ch); });
      if (ext == L".webp") {
        inferred = awj::OutputFormat::webp;
      } else if (ext == L".jxl") {
        inferred = awj::OutputFormat::jxl;
      } else if (result.encoder_id == "jpegli") {
        inferred = awj::OutputFormat::jpgli;
      }
      output_format = awj::output_format_name(inferred);
    }

    push_task_row(
        rows,
        TaskRow{.order = to_shared(std::format("{}", result.index + 1)),
                .filename =
                    to_shared(awj::path_to_utf8(result.input_path.filename())),
                .folder =
                    to_shared(awj::path_to_utf8(result.input_path.parent_path())),
                .size = to_shared(awj::format_size(result.original_bytes)),
                .status = to_shared(result_status_text(result)),
                .output =
                    to_shared(awj::path_to_utf8(result.output_path.filename())),
                .log = to_shared(result_log_text(result)),
                .warning = result.ok &&
                           result.requested_visual_quality.has_value() &&
                           !result.visual_quality_target_met,
                .locked = result.processed});
  } catch (...) {
  }
}

void append_log_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                    std::string_view text) noexcept {
  try {
    push_task_row(rows, TaskRow{.order = {},
                                .filename = {},
                                .folder = {},
                                .size = {},
                                .status = {},
                                .output = {},
                                .log = to_shared(text),
                                .warning = false,
                                .locked = true});
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
            .order = to_shared(std::format("{}", item.file.index + 1)),
            .filename = to_shared(awj::path_to_utf8(item.file.path.filename())),
            .folder =
                to_shared(awj::path_to_utf8(item.file.path.parent_path())),
            .size = to_shared(awj::format_size(item.file.bytes)),
            .status = to_shared("大图模式"),
            .output = {},
            .log = to_shared(std::format(
                "原因：{}；{}；可用处理方式：{}。",
                awj::large_image_reason_name(item.decision.reason),
                item.decision.reason_text, large_image_actions_summary(item))),
            .warning = false,
            .locked = true});
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

bool path_is_directory(const std::filesystem::path& path) noexcept {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) && !ec;
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
    state.worker.request_stop();
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
    case awj::OutputFormat::jpgli:
      return L"jpgli";
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
  if (cfg.output_format == awj::OutputFormat::jpgli) {
    push_cli_option(args, L"--chroma", cli_chroma_arg(cfg.chroma_mode));
    push_cli_option(args, L"--jpegli-progressive-level",
                    std::to_wstring(cfg.jpegli_progressive_level));
    args.push_back(cfg.jpegli_optimize_huffman
                       ? L"--jpegli-optimize-huffman"
                       : L"--no-jpegli-optimize-huffman");
    if (cfg.jpegli_xyb) {
      args.push_back(L"--jpegli-xyb");
    }
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

std::wstring quote_windows_command_arg(std::wstring_view arg,
                                      bool force_quotes = false) {
  if (arg.empty()) {
    return L"\"\"";
  }
  const bool needs_quotes =
      force_quotes || arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
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
  bool first = true;
  for (const auto& arg : args) {
    if (!command.empty()) {
      command.push_back(L' ');
    }
    command += quote_windows_command_arg(arg, first);
    first = false;
  }
  return command;
}

std::expected<std::shared_ptr<StudioChildProcess>, std::string>
start_studio_cli_worker(const awj::AppConfig& cfg, std::uint64_t run_id) {
  auto exe_path = awj::executable_path();
  if (!exe_path) {
    return std::unexpected{exe_path.error()};
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(*exe_path, ec) || ec) {
    return std::unexpected{std::format("找不到 Studio 编码 worker: {}。",
                                       awj::display_path_for_user(*exe_path))};
  }
  const auto exe_dir = exe_path->parent_path();

  auto child = std::make_shared<StudioChildProcess>();
  const auto event_name = std::format(L"Local\\AWJStudioCancel-{}-{}",
                                      GetCurrentProcessId(), run_id);
  child->cancel_event.reset(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
  if (child->cancel_event == nullptr) {
    return std::unexpected{std::format("创建编码取消事件失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }

  auto args = cli_arguments_from_config(cfg, event_name);
  args.insert(args.begin(), exe_path->native());
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
  const auto cwd = exe_dir.native();
  const BOOL created = CreateProcessW(
      exe_path->c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE,
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

class DropBridge {
 public:
  DropBridge(slint::Window& window,
             slint::ComponentWeakHandle<AwjStudio> weak,
             std::shared_ptr<UiState> state)
      : hwnd_{window.win32_hwnd()}, weak_{std::move(weak)}, state_{std::move(state)} {
    if (hwnd_ == nullptr) {
      return;
    }
    SetPropW(hwnd_, kPropertyName, this);
    previous_proc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(&DropBridge::window_proc)));
    if (previous_proc_ == nullptr) {
      RemovePropW(hwnd_, kPropertyName);
      hwnd_ = nullptr;
      return;
    }
    ChangeWindowMessageFilterEx(hwnd_, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    DragAcceptFiles(hwnd_, TRUE);
  }

  DropBridge(const DropBridge&) = delete;
  DropBridge& operator=(const DropBridge&) = delete;

  ~DropBridge() {
    if (hwnd_ == nullptr) {
      return;
    }
    DragAcceptFiles(hwnd_, FALSE);
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd_, GWLP_WNDPROC)) ==
        &DropBridge::window_proc) {
      SetWindowLongPtrW(hwnd_, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(previous_proc_));
    }
    RemovePropW(hwnd_, kPropertyName);
  }

 private:
  static constexpr const wchar_t* kPropertyName = L"AWJ_DropBridge";

  static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
    auto* self = static_cast<DropBridge*>(GetPropW(hwnd, kPropertyName));
    if (self != nullptr && message == WM_DROPFILES) {
      self->handle_drop(reinterpret_cast<HDROP>(wparam));
      return 0;
    }
    if (self != nullptr && self->previous_proc_ != nullptr) {
      return CallWindowProcW(self->previous_proc_, hwnd, message, wparam,
                             lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  void handle_drop(HDROP drop) {
    std::vector<std::filesystem::path> paths;
    const auto cleanup = std::unique_ptr<std::remove_pointer_t<HDROP>, void (*)(HDROP)>{
        drop, [](HDROP value) {
          if (value != nullptr) {
            DragFinish(value);
          }
        }};
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
      const UINT length = DragQueryFileW(drop, i, nullptr, 0);
      if (length == 0) {
        continue;
      }
      std::wstring path(length + 1, L'\0');
      const UINT copied = DragQueryFileW(drop, i, path.data(), length + 1);
      if (copied == 0) {
        continue;
      }
      path.resize(copied);
      paths.emplace_back(std::move(path));
    }
    if (paths.empty()) {
      return;
    }
    auto weak = weak_;
    auto state = state_;
    slint::invoke_from_event_loop(
        [weak, state, paths = std::move(paths)]() mutable {
          run_ui_callback(weak, "拖入导入失败", [&] {
            if (auto app = weak.lock()) {
              if (reject_when_worker_active(**app, state,
                                            "当前任务正在运行，无法拖入导入")) {
                return;
              }
              for (const auto& path : paths) {
                std::error_code ec;
                const bool is_folder =
                    std::filesystem::is_directory(path, ec) && !ec;
                add_queue_from_path(**app, *state, path, is_folder);
              }
            }
          });
        });
  }

  HWND hwnd_{};
  WNDPROC previous_proc_{};
  slint::ComponentWeakHandle<AwjStudio> weak_;
  std::shared_ptr<UiState> state_;
};

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
    case 3:
      return awj::OutputFormat::jpgli;
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
  if (format == awj::OutputFormat::webp ||
      format == awj::OutputFormat::jpgli) {
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
  app.set_quality_text(to_shared("80"));
  app.set_avif_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::avif))));
  app.set_webp_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::webp))));
  app.set_jxl_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::jxl))));
  app.set_jpegli_quality_default(to_shared(
      text_from_int(awj::default_quality_for(awj::OutputFormat::jpgli))));
  app.set_webp_bit_depth_default(
      to_shared(text_from_int(awj::encoding_defaults::default_webp_bit_depth)));
  app.set_memory_limit_text({});
  app.set_preset_index(0);
  app.set_visual_quality_text({});
  app.set_format_index(0);
  app.set_experimental_encoders(defaults.enable_experimental_encoders);
  app.set_visual_quality_gpu(defaults.visual_quality_gpu);
  app.set_visual_quality_fallback(defaults.visual_quality_fallback);
  app.set_allow_wic_fallback(defaults.allow_wic_fallback);
  refresh_avif_encoder_options(app);
  app.set_avif_encoder_index(0);
  app.set_collision_index(0);
  app.set_chroma_index(0);
  app.set_jpegli_progressive_index(2);
  app.set_jpegli_optimize_huffman(true);
  app.set_jpegli_xyb(false);
  app.set_alpha_policy_index(1);
  app.set_quality_follows_format(false);
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
      cfg.output_format == awj::OutputFormat::webp ||
              cfg.output_format == awj::OutputFormat::jpgli
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
  cfg.chroma_mode = (cfg.output_format == awj::OutputFormat::avif ||
                     cfg.output_format == awj::OutputFormat::jpgli)
                       ? chroma_from_index(app.get_chroma_index())
                       : awj::ChromaMode::auto_keep;
  cfg.jpegli_progressive_level = app.get_jpegli_progressive_index();
  cfg.jpegli_optimize_huffman =
      cfg.jpegli_progressive_level > 0 ? true : app.get_jpegli_optimize_huffman();
  cfg.jpegli_xyb = app.get_jpegli_xyb();

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

enum class PathPickerMode { image_file, folder };

std::optional<std::filesystem::path> choose_path(PathPickerMode mode) {
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
  options |= mode == PathPickerMode::folder ? FOS_PICKFOLDERS
                                            : FOS_FILEMUSTEXIST;
  hr = dialog->SetOptions(options);
  if (FAILED(hr)) {
    return std::nullopt;
  }

  if (mode == PathPickerMode::image_file) {
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

std::optional<std::filesystem::path> choose_path(bool pick_folder) {
  return choose_path(pick_folder ? PathPickerMode::folder
                                 : PathPickerMode::image_file);
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

std::expected<void, std::string> open_file_with_default_app(
    const std::filesystem::path& path) try {
  if (path.empty()) {
    return std::unexpected{"路径为空。"};
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::unexpected{std::format("图片文件不存在: {}。",
                                       awj::display_path_for_user(path))};
  }
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    return std::unexpected{
        std::format("无法打开图片: {}；ShellExecuteW 返回码：{}。",
                    awj::display_path_for_user(path), result)};
  }
  return {};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"打开图片文件系统访问失败。"};
}

std::expected<void, std::string> copy_text_to_clipboard(
    std::wstring_view text) {
  if (text.empty()) {
    return std::unexpected{"复制内容为空。"};
  }
  if (!OpenClipboard(nullptr)) {
    return std::unexpected{std::format("打开剪贴板失败: {}。",
                                       awj::win32_error_message(GetLastError()))};
  }
  struct ClipboardGuard {
    ~ClipboardGuard() { CloseClipboard(); }
  } guard;
  if (!EmptyClipboard()) {
    return std::unexpected{std::format("清空剪贴板失败: {}。",
                                       awj::win32_error_message(GetLastError()))};
  }
  const auto bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    return std::unexpected{"分配剪贴板内存失败。"};
  }
  void* locked = GlobalLock(memory);
  if (locked == nullptr) {
    GlobalFree(memory);
    return std::unexpected{"锁定剪贴板内存失败。"};
  }
  std::memcpy(locked, text.data(), text.size() * sizeof(wchar_t));
  static_cast<wchar_t*>(locked)[text.size()] = L'\0';
  GlobalUnlock(memory);
  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    return std::unexpected{std::format("写入剪贴板失败: {}。",
                                       awj::win32_error_message(GetLastError()))};
  }
  return {};
}

bool output_template_contains(std::wstring_view text,
                              std::wstring_view token) {
  return text.find(token) != std::wstring_view::npos;
}

std::expected<std::vector<awj::ImageFile>, std::string> build_run_files(
    const awj::AppConfig& cfg, const std::vector<QueueImageItem>& queue) {
  try {
    std::vector<awj::ImageFile> files;
    files.reserve(queue.size());
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    const bool needs_hash =
        output_template_contains(cfg.output_template, L"{hash}") ||
        output_template_contains(cfg.output_template, L"{hash8}");
    const bool needs_sha256 =
        output_template_contains(cfg.output_template, L"{sha256}") ||
        output_template_contains(cfg.output_template, L"{sha2568}") ||
        output_template_contains(cfg.output_template, L"{sha256_8}");
    for (const auto i : std::views::iota(std::size_t{}, queue.size())) {
      std::wstring hash;
      std::wstring sha256;
      if (needs_hash) {
        if (auto ok = awj::file_hash_token(queue[i].path, hash); !ok) {
          return std::unexpected{ok.error()};
        }
      }
      if (needs_sha256) {
        if (auto ok = awj::file_sha256_token(queue[i].path, sha256); !ok) {
          return std::unexpected{ok.error()};
        }
      }
      files.push_back(awj::make_image_file(i, queue[i].path,
                                           queue[i].relative_dir,
                                           queue[i].bytes, rng,
                                           std::move(hash),
                                           std::move(sha256)));
    }
    if (auto disambiguated = awj::apply_source_extension_disambiguation(cfg, files);
        !disambiguated) {
      return std::unexpected{disambiguated.error()};
    }
    if (auto resolved = awj::resolve_batch_output_paths(cfg, files); !resolved) {
      return std::unexpected{resolved.error()};
    }
    return files;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"构建队列运行快照时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"构建队列运行快照时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"构建队列运行快照时文件系统访问失败。"};
  }
}

awj::EncodeResult failed_queue_result(const awj::AppConfig& cfg,
                                      const awj::ImageFile& image,
                                      std::string message) {
  return awj::EncodeResult{.index = image.index,
                           .input_path = image.path,
                           .output_path = awj::output_path_for(cfg, image),
                           .output_format = awj::output_format_name(cfg.output_format),
                           .original_bytes = image.bytes,
                           .quality = cfg.quality,
                           .requested_visual_quality = cfg.visual_quality,
                           .final_encoder_quality = cfg.quality,
                           .speed = cfg.speed.value_or(
                               awj::default_speed_for(cfg.output_format)),
                           .processed = true,
                           .ok = false,
                           .message = std::move(message)};
}

std::expected<std::optional<awj::BatchLargeImageItem>, awj::EncodeResult>
classify_queue_large_item(const awj::AppConfig& cfg,
                          const awj::ImageFile& image) {
  if (cfg.output_format != awj::OutputFormat::avif) {
    return std::optional<awj::BatchLargeImageItem>{};
  }
  const auto availability =
      large_image_manual_availability(cfg.enable_experimental_encoders);
  auto dimensions = awj::probe_image_dimensions_for_path(
      image.path,
      awj::DecoderRegistryOptions{.allow_wic_fallback = cfg.allow_wic_fallback});
  if (!dimensions) {
    return std::unexpected{
        failed_queue_result(cfg, image, dimensions.error())};
  }
  auto decision =
      awj::classify_large_image(*dimensions, availability.grid,
                                availability.zenrav1e);
  if (decision.klass != awj::LargeImageClass::large_mode_required) {
    return std::optional<awj::BatchLargeImageItem>{};
  }
  return awj::BatchLargeImageItem{.file = image,
                                  .dimensions = *dimensions,
                                  .decision = std::move(decision)};
}

struct WorkerComApartment {
  explicit WorkerComApartment(bool enabled) noexcept : enabled_{enabled} {
    if (enabled_) {
      init_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED |
                                          COINIT_DISABLE_OLE1DDE);
    }
  }
  ~WorkerComApartment() {
    if (enabled_ && SUCCEEDED(init_)) {
      CoUninitialize();
    }
  }
  [[nodiscard]] bool usable() const noexcept {
    return !enabled_ || SUCCEEDED(init_) || init_ == RPC_E_CHANGED_MODE;
  }
  bool enabled_{};
  HRESULT init_{S_FALSE};
};

void post_queue_refresh(slint::ComponentWeakHandle<AwjStudio> weak,
                        const std::shared_ptr<UiState>& state,
                        std::string status_text = {},
                        std::optional<float> progress = std::nullopt) {
  post_to_ui(weak, [state, status_text = std::move(status_text),
                    progress](AwjStudio& app) {
    std::scoped_lock lock{state->mutex};
    refresh_queue_rows(app, *state);
    if (!status_text.empty()) {
      app.set_status_text(to_shared(status_text));
    }
    if (progress) {
      app.set_progress(*progress);
    }
  });
}

std::uint64_t queue_id_for_run_index(const UiState& state,
                                     std::size_t run_index) noexcept {
  if (auto index = queue_index_for_run_index(state, run_index)) {
    return state.queue_items[*index].id;
  }
  return 0;
}

void apply_result_to_queue_item(QueueImageItem& item,
                                const awj::EncodeResult& result) {
  item.warning = result.ok && result.requested_visual_quality.has_value() &&
                 !result.visual_quality_target_met;
  item.log_text = result_log_text(result);
  if (result.canceled) {
    item.status = QueueItemStatus::canceled;
  } else if (result.ok && result.skipped) {
    item.status = QueueItemStatus::skipped;
  } else if (result.ok) {
    item.status = QueueItemStatus::done;
  } else {
    item.status = QueueItemStatus::failed;
    item.warning = true;
  }
  item.status_text = result_status_text(result);
  if (!result.output_path.empty()) {
    item.locked_output_path = result.output_path;
  }
}

std::optional<awj::ImageFile> take_next_queue_work(
    UiState& state, const std::vector<awj::ImageFile>& files,
    std::unordered_set<std::wstring>& active_outputs) {
  for (auto& item : state.queue_items) {
    if (item.status != QueueItemStatus::pending ||
        item.run_index >= files.size()) {
      continue;
    }
    const auto output_key = queue_path_key(item.locked_output_path);
    if (!output_key.empty() && active_outputs.contains(output_key)) {
      continue;
    }
    item.status = QueueItemStatus::running;
    item.status_text = "正在编码";
    item.warning = false;
    if (!output_key.empty()) {
      active_outputs.insert(output_key);
    }
    return files[item.run_index];
  }
  return std::nullopt;
}

void begin_queue_conversion_run(slint::ComponentWeakHandle<AwjStudio> weak,
                                const std::shared_ptr<UiState>& state,
                                awj::AppConfig cfg) {
  auto app = weak.lock();
  if (!app) {
    return;
  }

  std::vector<QueueImageItem> queue_snapshot;
  {
    std::scoped_lock lock{state->mutex};
    if (state->worker_active) {
      (*app)->set_status_text(to_shared("当前任务正在运行，请先取消或强制终止"));
      return;
    }
    if (state->queue_items.empty()) {
      (*app)->set_status_text(to_shared("队列为空，请先输入或选择图片。"));
      return;
    }
    queue_snapshot = state->queue_items;
  }

  auto files = build_run_files(cfg, queue_snapshot);
  if (!files) {
    (*app)->set_status_text(to_shared(std::format("队列准备失败：{}", files.error())));
    return;
  }
  if (files->empty()) {
    (*app)->set_status_text(to_shared("队列为空，请先输入或选择图片。"));
    return;
  }

  std::uint64_t run_id{};
  {
    std::scoped_lock lock{state->mutex};
    run_id = ++state->run_id;
    state->worker_active = true;
    state->pending_events.clear();
    for (const auto i : std::views::iota(std::size_t{}, state->queue_items.size())) {
      auto& item = state->queue_items[i];
      item.status = QueueItemStatus::pending;
      item.status_text = "等待编码";
      item.log_text.clear();
      item.warning = false;
      item.run_index = i;
      item.locked_output_path = awj::output_path_for(cfg, (*files)[i]);
    }
    refresh_queue_rows(**app, *state);
  }
  (*app)->set_running(true);
  (*app)->set_progress(0.0f);
  (*app)->set_status_text(to_shared("正在启动队列编码…"));

  std::vector<awj::ImageFile> run_files = std::move(*files);
  std::optional<std::jthread> coordinator;
  try {
    coordinator.emplace([weak, state, cfg = std::move(cfg),
                         files = std::move(run_files),
                         run_id](std::stop_token stop_token) mutable {
      const auto output_dir = awj::output_dir_for(cfg);
      awj::FileLogger logger{output_dir, cfg.write_log};
      if (cfg.write_log && !logger.enabled()) {
        post_queue_refresh(
            weak, state,
            std::format("[WARN] 日志写入失败: {}", logger.last_error()));
      }
      const auto configured_memory_limit =
          cfg.memory_limit_bytes == 0
              ? awj::automatic_memory_limit(awj::current_memory_status())
              : cfg.memory_limit_bytes;
      std::uint64_t estimate = 1;
      for (const auto& image : files) {
        estimate = std::max<std::uint64_t>(
            estimate, static_cast<std::uint64_t>(
                          std::max<std::uintmax_t>(1, image.bytes)));
      }
      const auto resource_plan = awj::plan_resources(awj::ResourcePlanRequest{
          .automatic_thread_budget = cfg.max_jobs,
          .file_count = static_cast<int>(
              std::min<std::size_t>(files.size(),
                                    static_cast<std::size_t>(
                                        std::numeric_limits<int>::max()))),
          .memory_limit_bytes = configured_memory_limit,
          .estimated_bytes_per_file = estimate,
          .av1_encoder = cfg.output_format == awj::OutputFormat::avif});
      const int jobs = std::max(
          1, std::min<int>(resource_plan.file_parallelism,
                           static_cast<int>(std::min<std::size_t>(
                               files.size(), static_cast<std::size_t>(
                                                 std::numeric_limits<int>::max())))));
      post_queue_refresh(
          weak, state,
          std::format("队列共 {} 张图片，编码并发 {}。", files.size(), jobs));

      std::vector<awj::EncodeResult> results(files.size());
      for (const auto& image : files) {
        results[image.index] = awj::EncodeResult{
            .index = image.index,
            .input_path = image.path,
            .output_path = awj::output_path_for(cfg, image),
            .output_format = awj::output_format_name(cfg.output_format),
            .original_bytes = image.bytes,
            .quality = cfg.quality,
            .requested_visual_quality = cfg.visual_quality,
            .final_encoder_quality = cfg.quality,
            .speed = cfg.speed.value_or(awj::default_speed_for(cfg.output_format)),
            .message = "未处理。"};
      }

      std::atomic<std::size_t> completed{0};
      std::atomic<int> worker_failures{0};
      std::unordered_set<std::wstring> active_outputs;
      std::vector<std::jthread> workers;
      workers.reserve(static_cast<std::size_t>(jobs));
      for (const int worker_index : std::views::iota(0, jobs)) {
        try {
          workers.emplace_back([&, worker_index] {
            WorkerComApartment com{cfg.allow_wic_fallback};
            awj::AppConfig effective_cfg = cfg;
            if (cfg.allow_wic_fallback && !com.usable()) {
              effective_cfg.allow_wic_fallback = false;
            }
            awj::NativeBackend backend{effective_cfg, logger, resource_plan};
            while (!stop_token.stop_requested()) {
              std::optional<awj::ImageFile> image;
              {
                std::scoped_lock lock{state->mutex};
                if (state->run_id != run_id) {
                  return;
                }
                image = take_next_queue_work(*state, files, active_outputs);
              }
              if (!image) {
                break;
              }
              post_queue_refresh(weak, state);

              awj::EncodeResult result;
              auto large = classify_queue_large_item(effective_cfg, *image);
              if (!large) {
                result = std::move(large.error());
              } else if (*large) {
                result = awj::pipeline_detail::encode_large_mode_item(
                    effective_cfg, logger, **large, configured_memory_limit,
                    stop_token);
              } else {
                result = backend.encode(*image, stop_token);
              }

              const auto output_key = queue_path_key(result.output_path);
              const auto done = completed.fetch_add(1) + 1;
              {
                std::scoped_lock lock{state->mutex};
                if (result.index < results.size()) {
                  results[result.index] = result;
                }
                if (!output_key.empty()) {
                  active_outputs.erase(output_key);
                }
                if (auto index = queue_index_for_run_index(*state, result.index)) {
                  apply_result_to_queue_item(state->queue_items[*index], result);
                }
              }
              post_queue_refresh(
                  weak, state,
                  std::format("已完成 {}/{}：{}", done, files.size(),
                              awj::path_to_utf8(result.input_path.filename())),
                  static_cast<float>(done) / static_cast<float>(files.size()));
            }
          });
        } catch (...) {
          worker_failures.fetch_add(1);
        }
      }
      workers.clear();

      const bool canceled = stop_token.stop_requested();
      {
        std::scoped_lock lock{state->mutex};
        for (auto& item : state->queue_items) {
          if (item.status == QueueItemStatus::pending ||
              item.status == QueueItemStatus::running) {
            item.status = canceled ? QueueItemStatus::canceled
                                   : QueueItemStatus::failed;
            item.status_text = canceled ? "已取消" : "未处理";
            item.warning = !canceled;
          }
        }
      }

      std::size_t ok_count = 0;
      std::size_t failed_count = 0;
      std::size_t canceled_count = 0;
      std::uintmax_t original_total = 0;
      std::uintmax_t output_total = 0;
      for (const auto& result : results) {
        if (result.ok) {
          ++ok_count;
          original_total += result.original_bytes;
          output_total += result.output_bytes;
        } else if (result.canceled || canceled) {
          ++canceled_count;
        } else if (result.processed) {
          ++failed_count;
          original_total += result.original_bytes;
        }
      }
      bool csv_failed = false;
      std::string csv_error;
      if (cfg.write_summary) {
        if (auto csv = awj::write_csv(output_dir, results); !csv) {
          csv_failed = true;
          csv_error = csv.error();
        }
      }
      post_to_ui(weak, [state, run_id, ok_count, failed_count, canceled_count,
                        worker_failures = worker_failures.load(), csv_failed,
                        csv_error = std::move(csv_error),
                        canceled](AwjStudio& app) {
        std::scoped_lock lock{state->mutex};
        if (state->run_id != run_id) {
          return;
        }
        state->worker_active = false;
        state->pending_events.clear();
        app.set_running(false);
        app.set_progress(canceled ? 0.0f : 1.0f);
        refresh_queue_rows(app, *state);
        if (csv_failed) {
          app.set_status_text(to_shared(std::format(
              "完成：成功 {}，失败 {}，取消 {}，报告写入失败：{}",
              ok_count, failed_count + static_cast<std::size_t>(worker_failures),
              canceled_count, csv_error)));
        } else {
          app.set_status_text(to_shared(std::format(
              "{}：成功 {}，失败 {}，取消 {}。",
              canceled ? "已取消" : "完成", ok_count,
              failed_count + static_cast<std::size_t>(worker_failures),
              canceled_count)));
        }
      });
    });
  } catch (...) {
    reset_failed_run(**app, *state, run_id, "队列转换启动失败。");
    return;
  }
  {
    std::scoped_lock lock{state->mutex};
    state->worker = std::move(*coordinator);
  }
}

void handle_queue_menu_action(AwjStudio& app,
                              const std::shared_ptr<UiState>& state,
                              int index, std::string action) {
  std::optional<std::filesystem::path> path_to_open;
  std::optional<std::filesystem::path> image_to_open;
  std::optional<std::wstring> text_to_copy;
  std::string status;
  {
    std::scoped_lock lock{state->mutex};
    if (index < 0 ||
        static_cast<std::size_t>(index) >= state->queue_items.size()) {
      app.set_status_text(to_shared("队列项不存在。"));
      return;
    }
    auto& item = state->queue_items[static_cast<std::size_t>(index)];
    if (action == "open-folder") {
      path_to_open = item.path.parent_path();
    } else if (action == "open-image") {
      image_to_open = item.path;
    } else if (action == "copy-path") {
      text_to_copy = item.path.native();
    } else if (action == "remove") {
      if (state->worker_active || !queue_item_editable(item)) {
        app.set_status_text(to_shared("运行中不能移除队列项。"));
        return;
      }
      state->queue_items.erase(state->queue_items.begin() + index);
      status = "已从队列移除。";
    } else if (!queue_item_editable(item)) {
      app.set_status_text(to_shared("该图片已开始编码，不能重新排序。"));
      return;
    } else if (action == "priority") {
      const auto target = first_pending_index(*state);
      if (target < state->queue_items.size()) {
        move_queue_item(*state, static_cast<std::size_t>(index), target);
      }
      status = "已移到未编码队列最前。";
    } else if (action == "last") {
      const auto target = last_pending_index(*state);
      if (target < state->queue_items.size()) {
        move_queue_item(*state, static_cast<std::size_t>(index), target);
      }
      status = "已移到未编码队列最后。";
    } else if (action == "move-up") {
      if (index > 0) {
        move_queue_item(*state, static_cast<std::size_t>(index),
                        static_cast<std::size_t>(index - 1));
      }
      status = "已上移。";
    } else if (action == "move-down") {
      move_queue_item(*state, static_cast<std::size_t>(index),
                      static_cast<std::size_t>(index + 1));
      status = "已下移。";
    }
    refresh_queue_rows(app, *state);
  }

  if (path_to_open) {
    if (auto opened = open_path(*path_to_open, false); !opened) {
      app.set_status_text(
          to_shared(std::format("打开所在位置失败：{}", opened.error())));
    }
    return;
  }
  if (image_to_open) {
    if (auto opened = open_file_with_default_app(*image_to_open); !opened) {
      app.set_status_text(
          to_shared(std::format("打开图片失败：{}", opened.error())));
    }
    return;
  }
  if (text_to_copy) {
    if (auto copied = copy_text_to_clipboard(*text_to_copy); !copied) {
      app.set_status_text(
          to_shared(std::format("复制路径失败：{}", copied.error())));
    } else {
      app.set_status_text(to_shared("已复制完整路径。"));
    }
    return;
  }
  if (!status.empty()) {
    app.set_status_text(to_shared(status));
  }
}

void handle_queue_pointer_event(AwjStudio& app,
                                const std::shared_ptr<UiState>& state,
                                int index, int button, int kind,
                                float local_y) {
  if (button != 0) {
    return;
  }
  constexpr float row_height = 34.0f;
  constexpr auto long_press_delay = std::chrono::milliseconds{360};
  constexpr auto double_click_delay = std::chrono::milliseconds{450};
  std::optional<std::filesystem::path> double_click_folder;
  bool refresh = false;
  {
    std::scoped_lock lock{state->mutex};
    if (index < 0 ||
        static_cast<std::size_t>(index) >= state->queue_items.size()) {
      return;
    }
    auto& item = state->queue_items[static_cast<std::size_t>(index)];
    const auto now = std::chrono::steady_clock::now();
    if (kind == 0) {
      state->drag_candidate_id = queue_item_editable(item) ? item.id : 0;
      state->drag_started = now;
      state->drag_reordered = false;
      return;
    }
    if (kind == 2 && state->drag_candidate_id != 0 &&
        now - state->drag_started >= long_press_delay) {
      const auto current = queue_index_for_id(*state, state->drag_candidate_id);
      if (!current) {
        return;
      }
      if (local_y < -8.0f && *current > 0) {
        refresh = move_queue_item(*state, *current, *current - 1);
      } else if (local_y > row_height + 8.0f &&
                 *current + 1 < state->queue_items.size()) {
        refresh = move_queue_item(*state, *current, *current + 1);
      }
      state->drag_reordered = state->drag_reordered || refresh;
    }
    if (kind == 1) {
      const bool was_drag = state->drag_reordered;
      state->drag_candidate_id = 0;
      state->drag_reordered = false;
      if (!was_drag) {
        if (state->last_click_id == item.id &&
            now - state->last_click_time <= double_click_delay) {
          double_click_folder = item.path.parent_path();
          state->last_click_id = 0;
        } else {
          state->last_click_id = item.id;
          state->last_click_time = now;
        }
      }
    }
    if (refresh) {
      refresh_queue_rows(app, *state);
      app.set_status_text(to_shared("已调整未编码队列顺序。"));
    }
  }
  if (double_click_folder) {
    if (auto opened = open_path(*double_click_folder, false); !opened) {
      app.set_status_text(
          to_shared(std::format("打开所在位置失败：{}", opened.error())));
    }
  }
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

int run_studio_ui() {
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
    apply_system_ui_font(*app);
    app->set_threads_text({});
    app->set_selected_large_image_action_index(0);
    app->set_system_dark_mode(windows_prefers_dark_mode());
    state->config_defaults = capture_studio_config(*app);
    std::optional<std::string> config_warning;
    if (auto loaded = apply_studio_config_file(*app); !loaded) {
      config_warning = std::format("读取 Studio 配置失败：{}", loaded.error());
    }
    sync_template_flags(*app);
    state->last_config_snapshot = capture_studio_config(*app);
    if (config_warning) {
      app->set_status_text(to_shared(*config_warning));
    }

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
        state->queue_items.clear();
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

    state->theme_timer.start(
        slint::TimerMode::Repeated, std::chrono::seconds{3}, [weak] {
          run_ui_callback(weak, "更新主题状态失败", [&] {
            if (auto app = weak.lock()) {
              (*app)->set_system_dark_mode(windows_prefers_dark_mode());
            }
          });
        });

    std::weak_ptr<UiState> weak_state = state;
    state->config_timer.start(
        slint::TimerMode::Repeated, std::chrono::milliseconds{800},
        [weak, weak_state] {
          run_ui_callback(weak, "保存 Studio 配置失败", [&] {
            auto state = weak_state.lock();
            auto app = weak.lock();
            if (!state || !app || !state->config_defaults) {
              return;
            }
            auto current = capture_studio_config(**app);
            if (state->last_config_snapshot &&
                current == *state->last_config_snapshot) {
              return;
            }
            if (auto saved =
                    write_studio_config_file(current, *state->config_defaults);
                !saved) {
              (*app)->set_status_text(
                  to_shared(std::format("保存 Studio 配置失败：{}",
                                        saved.error())));
              return;
            }
            state->last_config_snapshot = std::move(current);
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
            add_queue_from_path(**app, *state, *path, pick_folder);
          }
        }
      });
    });

    app->on_input_path_accepted(
        [weak, state](slint::SharedString input_text) {
          run_ui_callback(weak, "输入路径入队失败", [&] {
            if (auto app = weak.lock()) {
              if (reject_when_worker_active(**app, state,
                                            "当前任务正在运行，无法添加队列")) {
                return;
              }
              auto text = trim_copy(shared_to_string(input_text));
              if (text.empty()) {
                (*app)->set_status_text(to_shared("输入路径为空。"));
                return;
              }
              const auto path =
                  std::filesystem::path{awj::wide_from_utf8(text)};
              const bool pick_folder = (*app)->get_input_mode_index() != 0;
              add_queue_from_path(**app, *state, path, pick_folder);
            }
          });
        });

    app->on_queue_menu_action(
        [weak, state](int index, slint::SharedString action_text) {
          run_ui_callback(weak, "队列菜单操作失败", [&] {
            if (auto app = weak.lock()) {
              handle_queue_menu_action(**app, state, index,
                                       shared_to_string(action_text));
            }
          });
        });

    app->on_queue_row_pointer_event(
        [weak, state](int index, int button, int kind, float local_y) {
          run_ui_callback(weak, "队列交互失败", [&] {
            if (auto app = weak.lock()) {
              handle_queue_pointer_event(**app, state, index, button, kind,
                                         local_y);
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

        if (trim_copy(shared_to_string((*app)->get_input_path())).empty()) {
          std::scoped_lock lock{state->mutex};
          if (!state->queue_items.empty()) {
            const auto& first = state->queue_items.front();
            const auto fallback =
                first.source_root.empty() ? first.path : first.source_root;
            (*app)->set_input_path(to_shared(awj::path_to_utf8(fallback)));
            if (output_dir_is_empty(**app)) {
              (*app)->set_output_dir(to_shared(
                  awj::path_to_utf8(awj::default_output_dir_for(fallback))));
            }
          }
        }

        auto cfg = config_from_ui(**app);
        if (!cfg) {
          (*app)->set_running(false);
          (*app)->set_status_text(
              to_shared(std::format("配置错误：{}", cfg.error())));
          return;
        }

        begin_queue_conversion_run(weak, state, std::move(*cfg));
      });
    });

    app->show();
    apply_title_bar_theme(app->window(), effective_studio_dark_mode(*app));
    constrain_window_to_work_area(app->window());
    DropBridge drop_bridge{app->window(), weak, state};
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
    MessageBoxW(nullptr, L"Studio 启动失败。", L"AWJ",
                MB_OK | MB_ICONERROR);
    return 1;
  } catch (...) {
    MessageBoxW(nullptr, L"Studio 启动失败：未知异常。", L"AWJ",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
