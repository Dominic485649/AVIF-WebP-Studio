#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "awj_studio.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <format>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <slint.h>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

import awj.config;
import awj.core;
import awj.encoding_defaults;
import awj.pipeline;

namespace {

struct UiState {
  std::jthread worker{};
  std::shared_ptr<slint::VectorModel<TaskRow>> task_rows{};
  slint::Timer theme_timer{};
  std::uint64_t run_id{};
  std::mutex mutex{};
};

struct ComApartment {
  ComApartment() : init{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                COINIT_DISABLE_OLE1DDE)} {}

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

std::expected<int, std::string> parse_int_field(std::string text,
                                                   std::string_view name,
                                                   int minimum,
                                                   int maximum) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::unexpected{std::format("{} 不能为空。", name)};
  }
  int value{};
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::unexpected{std::format("{} 必须是整数。", name)};
  }
  if (value < minimum || value > maximum) {
    return std::unexpected{
        std::format("{} 范围必须在 {} 到 {} 之间。", name, minimum, maximum)};
  }
  return value;
}

std::expected<std::optional<int>, std::string> parse_optional_int_field(
    std::string text,
    std::string_view name,
    int minimum,
    int maximum) {
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
  return parse_int_field(std::move(text), "线程", 1, 128);
}

std::expected<int, std::string> parse_resolution_field(std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return 0;
  }
  return parse_int_field(std::move(text), "最长边", 0, 100000);
}

std::expected<std::optional<int>, std::string> parse_bit_depth_field(std::string text) {
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

std::expected<std::uint64_t, std::string> parse_memory_limit_field(std::string text) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    text = std::string{awj::encoding_defaults::default_memory_limit_text};
  }
  return awj::parse_memory_limit(awj::wide_from_utf8(text));
}

std::expected<void, std::string> validate_token_options(std::string text) {
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find_first_of(",;", start);
    auto token = trim_copy(text.substr(start, end - start));
    if (!token.empty()) {
      auto wide = awj::wide_from_utf8(token);
      if (auto valid = awj::validate_native_option(wide); !valid) {
        return std::unexpected{valid.error()};
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return {};
}

slint::SharedString to_shared(std::string_view text) {
  return slint::SharedString{std::string{text}.c_str()};
}

std::string text_from_wide(std::wstring_view text) {
  return awj::utf8_from_wide(text);
}

std::string text_from_int(int value) {
  return std::format("{}", value);
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
}

std::string token_with_separator(std::string_view text, std::string_view token) {
  if (text.empty()) {
    return std::string{token};
  }
  const char last = text.back();
  const bool needs_separator = last != '_' && last != '-' && last != ' ' && last != '.';
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
  // Windows stores the per-app theme choice in this user registry value.
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

template <class Function>
void post_to_ui(slint::ComponentWeakHandle<AwjStudio> weak, Function&& fn) {
  slint::invoke_from_event_loop([weak, fn = std::forward<Function>(fn)]() mutable {
    if (auto app = weak.lock()) {
      fn(**app);
    }
  });
}

std::string result_status_text(const awj::EncodeResult& result) {
  if (result.ok) {
    if (result.skipped) {
      return "已跳过";
    }
    const double ratio = result.original_bytes == 0
                             ? 0.0
                             : static_cast<double>(result.output_bytes) /
                                   static_cast<double>(result.original_bytes) * 100.0;
    if (result.requested_visual_quality) {
      return std::format("完成 · {} · {:.1f}% · VQ {:.2f} · q{} · {:.2f}s",
                         awj::format_size(result.output_bytes), ratio,
                         result.visual_score, result.final_encoder_quality,
                         result.seconds);
    }
    return std::format("完成 · {} · {:.1f}% · {:.2f}s", awj::format_size(result.output_bytes),
                       ratio, result.seconds);
  }
  if (result.canceled) {
    return "已取消";
  }
  return result.message.empty() ? "失败" : std::format("失败 · {}", result.message);
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
  return std::format(
      "score {:.2f} · GMSD {:.6f} · MS-SSIM {:.6f} · Qg {:.2f} · Qm {:.2f} · q{} · {} 次 · {}",
      result.visual_score, result.raw_gmsd, result.raw_ms_ssim,
      result.gmsd_quality_score, result.msssim_quality_score,
      result.final_encoder_quality, result.search_attempt_count,
      awj::format_size(result.output_bytes));
}

void add_task_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                  const awj::EncodeResult& result) {
  auto output_format = awj::OutputFormat::avif;
  auto ext = result.output_path.extension().wstring();
  std::ranges::transform(ext, ext.begin(),
                         [](wchar_t ch) { return std::towlower(ch); });
  if (ext == L".webp") {
    output_format = awj::OutputFormat::webp;
  } else if (ext == L".jxl") {
    output_format = awj::OutputFormat::jxl;
  }

  rows->push_back(TaskRow{.filename = to_shared(awj::path_to_utf8(result.input_path.filename())),
                          .format = to_shared(awj::output_format_name(output_format)),
                          .status = to_shared(result_status_text(result)),
                          .log = to_shared(result_log_text(result))});
  constexpr std::size_t max_rows = 5000;
  if (rows->row_count() > max_rows) {
    rows->erase(0);
  }
}

void append_log_row(const std::shared_ptr<slint::VectorModel<TaskRow>>& rows,
                    std::string_view text) {
  rows->push_back(TaskRow{.filename = {},
                          .format = {},
                          .status = {},
                          .log = to_shared(text)});
  constexpr std::size_t max_rows = 5000;
  if (rows->row_count() > max_rows) {
    rows->erase(0);
  }
}

void trim_process_working_set() {
  SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                           static_cast<SIZE_T>(-1));
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

void apply_format_defaults_to_ui(AwjStudio& app, int format_index) {
  const auto format = output_format_from_index(format_index);
  if (app.get_quality_follows_format()) {
    app.set_quality_text(to_shared(text_from_int(awj::default_quality_for(format))));
  }
  if (format == awj::OutputFormat::webp) {
    app.set_bit_depth_text(to_shared(text_from_int(awj::encoding_defaults::default_webp_bit_depth)));
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
  app.set_input_path(to_shared(text_from_wide(defaults.input_path.native())));
  app.set_template_text(to_shared(text_from_wide(defaults.output_template)));
  app.set_quality_text(to_shared(text_from_int(defaults.quality)));
  app.set_avif_quality_default(to_shared(text_from_int(awj::default_quality_for(awj::OutputFormat::avif))));
  app.set_webp_quality_default(to_shared(text_from_int(awj::default_quality_for(awj::OutputFormat::webp))));
  app.set_jxl_quality_default(to_shared(text_from_int(awj::default_quality_for(awj::OutputFormat::jxl))));
  app.set_webp_bit_depth_default(to_shared(text_from_int(awj::encoding_defaults::default_webp_bit_depth)));
  app.set_memory_limit_text(to_shared(awj::encoding_defaults::default_memory_limit_text));
  app.set_format_index(0);
  app.set_backend_index(0);
  app.set_avif_encoder_index(0);
  app.set_collision_index(0);
  app.set_chroma_index(0);
  app.set_quality_follows_format(true);
  app.set_bit_depth_follows_format(true);
}

std::expected<awj::AppConfig, std::string> config_from_ui(const AwjStudio& app) {
  awj::AppConfig cfg = awj::default_app_config();
  cfg.input_path = awj::wide_from_utf8(shared_to_string(app.get_input_path()));
  cfg.output_dir = awj::wide_from_utf8(shared_to_string(app.get_output_dir()));
  cfg.output_template =
      awj::wide_from_utf8(shared_to_string(app.get_template_text()));
  if (cfg.output_template.empty()) {
    cfg.output_template = awj::encoding_defaults::default_output_template;
  }

  cfg.output_format = output_format_from_index(app.get_format_index());
  cfg.avif_encoder = cfg.output_format == awj::OutputFormat::avif
                         ? avif_encoder_from_index(app.get_avif_encoder_index())
                         : awj::AvifEncoderMode::automatic;
  cfg.enable_experimental_encoders = app.get_experimental_encoders();
  cfg.collision_mode = collision_from_index(app.get_collision_index());

  const auto quality =
      parse_int_field(shared_to_string(app.get_quality_text()), "质量", 1, 100);
  if (!quality) {
    return std::unexpected{quality.error()};
  }
  cfg.quality = *quality;
  const auto visual_quality = parse_visual_quality_field(
      shared_to_string(app.get_visual_quality_text()));
  if (!visual_quality) {
    return std::unexpected{visual_quality.error()};
  }
  cfg.visual_quality = *visual_quality;

  const auto bit_depth = app.get_format_index() == 1
                             ? std::expected<std::optional<int>, std::string>{std::optional<int>{awj::encoding_defaults::default_webp_bit_depth}}
                             : parse_bit_depth_field(shared_to_string(app.get_bit_depth_text()));
  if (!bit_depth) {
    return std::unexpected{bit_depth.error()};
  }
  cfg.bit_depth = *bit_depth;

  cfg.chroma_mode = cfg.output_format == awj::OutputFormat::avif
                        ? (cfg.avif_encoder == awj::AvifEncoderMode::svt
                               ? awj::ChromaMode::yuv420
                               : chroma_from_index(app.get_chroma_index()))
                        : awj::ChromaMode::auto_keep;

  const auto max_resolution = parse_resolution_field(
      shared_to_string(app.get_max_resolution_text()));
  if (!max_resolution) {
    return std::unexpected{max_resolution.error()};
  }
  cfg.max_resolution = *max_resolution;

  const auto max_jobs = parse_jobs_field(shared_to_string(app.get_threads_text()));
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

  const auto speed =
      parse_optional_int_field(shared_to_string(app.get_speed_text()), "speed", 0, 10);
  if (!speed) {
    return std::unexpected{speed.error()};
  }
  cfg.speed = *speed;

  cfg.strip_metadata = app.get_strip_metadata();
  cfg.write_summary = app.get_write_summary();
  cfg.write_log = app.get_write_log();
  if (auto options = validate_token_options(shared_to_string(app.get_defines_text()));
      !options) {
    return std::unexpected{options.error()};
  }

  if (auto valid = awj::finalize_config_defaults(cfg, true, false); !valid) {
    return std::unexpected{valid.error()};
  }
  return cfg;
}

std::optional<std::filesystem::path> choose_path(bool pick_folder) {
  ComApartment apartment;
  if (!apartment.usable()) {
    return std::nullopt;
  }

  // 文件选择器是 COM 对象，unique_ptr 的 deleter 让后续任何早退路径都能配对 Release。
  IFileDialog* raw_dialog = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&raw_dialog));
  std::unique_ptr<IFileDialog, ComReleaseDeleter<IFileDialog>> dialog{raw_dialog};
  if (FAILED(hr) || dialog == nullptr) {
    return std::nullopt;
  }

  DWORD options{};
  dialog->GetOptions(&options);
  options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
  options |= pick_folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
  dialog->SetOptions(options);

  if (!pick_folder) {
    const COMDLG_FILTERSPEC filters[] = {
        {L"图片文件",
         L"*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.tif;*.tiff;*.gif;*.jxl;*.jp2;*.heic;*.heif;*.avif"},
        {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
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

  // SIGDN_FILESYSPATH 返回 CoTaskMemAlloc 缓冲区，复制进 filesystem::path 后立即交还给 COM 分配器。
  return std::filesystem::path{path.get()};
}


std::filesystem::path effective_output_dir(const AwjStudio& app) {
  auto output = std::filesystem::path{
      awj::wide_from_utf8(shared_to_string(app.get_output_dir()))};
  if (!output.empty()) {
    return output;
  }
  const auto input =
      std::filesystem::path{awj::wide_from_utf8(shared_to_string(app.get_input_path()))};
  return awj::default_output_dir_for(input);
}

void open_path(std::filesystem::path path, bool create_if_missing) {
  if (path.empty()) {
    return;
  }

  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec) && !ec) {
    path = path.parent_path();
  }
  if (create_if_missing) {
    std::filesystem::create_directories(path, ec);
  }
  if (!std::filesystem::exists(path, ec)) {
    path = path.parent_path();
  }
  if (!path.empty()) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  try {
    auto app = AwjStudio::create();
    auto state = std::make_shared<UiState>();
    state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
    auto weak = slint::ComponentWeakHandle(app);

    app->set_task_rows(state->task_rows);
    initialize_ui_defaults(*app);
    app->set_threads_text({});
    app->set_system_dark_mode(windows_prefers_dark_mode());
    sync_template_flags(*app);

    app->on_clear_tasks([weak, state] {
      state->task_rows->set_vector({});
      if (auto app = weak.lock()) {
        (*app)->set_progress(0.0f);
        (*app)->set_status_text(to_shared("就绪"));
      }
    });

    app->on_format_defaults_requested([weak](int index) {
      if (auto app = weak.lock()) {
        apply_format_defaults_to_ui(**app, index);
      }
    });

    app->on_toggle_template_token([weak](slint::SharedString token) {
      if (auto app = weak.lock()) {
        toggle_template_token(**app, shared_to_string(token));
      }
    });

    state->theme_timer.start(slint::TimerMode::Repeated, std::chrono::seconds{3}, [weak] {
      if (auto app = weak.lock()) {
        (*app)->set_system_dark_mode(windows_prefers_dark_mode());
      }
    });

    app->on_browse_input([weak] {
      if (auto app = weak.lock()) {
        const bool pick_folder = (*app)->get_input_mode_index() != 0;
        if (auto path = choose_path(pick_folder)) {
          const auto output = awj::default_output_dir_for(*path);
          post_to_ui(weak, [path = *path, output](AwjStudio& app) {
            app.set_input_path(to_shared(awj::path_to_utf8(path)));
            app.set_output_dir(to_shared(awj::path_to_utf8(output)));
          });
        }
      }
    });

    app->on_open_input([weak] {
      if (auto app = weak.lock()) {
        const auto input = std::filesystem::path{
            awj::wide_from_utf8(shared_to_string((*app)->get_input_path()))};
        open_path(input, false);
      }
    });

    app->on_browse_output([weak] {
      if (auto folder = choose_path(true)) {
        post_to_ui(weak, [folder = *folder](AwjStudio& app) {
          app.set_output_dir(to_shared(awj::path_to_utf8(folder)));
        });
      }
    });

    app->on_open_output([weak] {
      if (auto app = weak.lock()) {
        open_path(effective_output_dir(**app), true);
      }
    });

    app->on_cancel_conversion([state] {
      std::scoped_lock lock{state->mutex};
      if (state->worker.joinable()) {
        state->worker.request_stop();
      }
    });

    app->on_start_conversion([weak, state] {
      auto app = weak.lock();
      if (!app) {
        return;
      }

      auto cfg = config_from_ui(**app);
      if (!cfg) {
        (*app)->set_running(false);
        (*app)->set_status_text(to_shared(std::format("配置错误：{}", cfg.error())));
        state->task_rows->set_vector({});
        return;
      }

      (*app)->set_running(true);
      (*app)->set_progress(0.0f);
      (*app)->set_status_text(to_shared("准备中"));

      std::jthread old_worker;
      {
        std::scoped_lock lock{state->mutex};
        old_worker = std::move(state->worker);
      }
      if (old_worker.joinable()) {
        old_worker.request_stop();
        old_worker.join();
      }

      std::uint64_t run_id{};
      std::shared_ptr<slint::VectorModel<TaskRow>> rows;
      {
        std::scoped_lock lock{state->mutex};
        run_id = ++state->run_id;
        state->task_rows = std::make_shared<slint::VectorModel<TaskRow>>();
        rows = state->task_rows;
      }
      (*app)->set_task_rows(rows);

      std::jthread worker{
          [weak, state, rows, run_id, cfg = std::move(*cfg)](
              std::stop_token token) {
            const auto summary = awj::run_batch(
                cfg,
                [weak, state, rows, run_id](const awj::BatchProgress& event) {
                  // 后台线程只能投递 weak handle；run_id 用来丢弃上一轮转换迟到的 UI 更新。
                  post_to_ui(weak, [event, state, rows, run_id](AwjStudio& app) {
                    {
                      std::scoped_lock lock{state->mutex};
                      if (state->run_id != run_id) {
                        return;
                      }
                    }
                    if (event.total > 0) {
                      const auto progress =
                          static_cast<float>(event.completed) /
                          static_cast<float>(event.total);
                      app.set_progress(std::clamp(progress, 0.0f, 1.0f));
                      app.set_status_text(to_shared(
                          std::format("{}/{}", event.completed, event.total)));
                    }
                    if (event.kind == awj::BatchEventKind::item_finished) {
                      add_task_row(rows, event.result);
                    }
                    if (event.kind == awj::BatchEventKind::warning) {
                      append_log_row(rows, event.text);
                    }
                  });
                },
                token);

            trim_process_working_set();
            // summary 回调同样可能晚于用户新开的一轮转换，必须在 event loop 内再次校验 run_id。
            post_to_ui(weak, [summary, state, run_id](AwjStudio& app) {
              {
                std::scoped_lock lock{state->mutex};
                if (state->run_id != run_id) {
                  return;
                }
              }
              if (!summary) {
                app.set_status_text(to_shared(std::format("失败：{}", summary.error())));
              } else if (summary->canceled) {
                app.set_status_text(to_shared("已取消"));
              } else {
                app.set_status_text(to_shared(summary->failed_count == 0 ? "完成" : "有失败"));
                app.set_progress(1.0f);
              }
              app.set_running(false);
            });
          }};
      {
        std::scoped_lock lock{state->mutex};
        state->worker = std::move(worker);
      }
    });

    app->show();
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
  } catch (const std::exception& ex) {
    const auto message = awj::wide_from_utf8(std::format("Studio 启动失败：{}", ex.what()));
    MessageBoxW(nullptr, message.c_str(), L"AWJ-studio", MB_OK | MB_ICONERROR);
    return 1;
  } catch (...) {
    MessageBoxW(nullptr, L"Studio 启动失败：未知异常。", L"AWJ-studio", MB_OK | MB_ICONERROR);
    return 1;
  }
}
