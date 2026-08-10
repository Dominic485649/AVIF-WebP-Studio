#include <slint-testing.h>
#include <slint.h>

#include <array>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "awj_studio_ui_smoke.h"

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

void send_key(const slint::ComponentHandle<AwjStudio>& app,
              std::u8string_view key) {
  const slint::SharedString text{key};
  app->window().dispatch_key_press_event(text);
  app->window().dispatch_key_release_event(text);
}

void send_text_key(const slint::ComponentHandle<AwjStudio>& app,
                   std::string_view key) {
  const slint::SharedString text{key};
  app->window().dispatch_key_press_event(text);
  app->window().dispatch_key_release_event(text);
}

std::optional<slint::testing::ElementHandle> find_one(
    const slint::ComponentHandle<AwjStudio>& app, std::string_view label,
    std::optional<slint::language::AccessibleRole> role = std::nullopt) {
  auto elements =
      slint::testing::ElementHandle::find_by_accessible_label(app, label);
  std::optional<slint::testing::ElementHandle> match;
  for (const auto& element : elements) {
    if (role && element.accessible_role() != role) {
      continue;
    }
    if (match) {
      return std::nullopt;
    }
    match = element;
  }
  return match;
}

std::shared_ptr<slint::VectorModel<ComboOption>> font_options() {
  std::vector<ComboOption> options;
  options.reserve(15);
  for (int index = 0; index < 15; ++index) {
    options.push_back(ComboOption{
        .text = slint::SharedString{std::format(
            "Smoke Font {:02} With A Deliberately Long Family Name", index)},
        .enabled = true});
  }
  return std::make_shared<slint::VectorModel<ComboOption>>(std::move(options));
}

std::shared_ptr<slint::VectorModel<TaskRow>> task_rows() {
  std::vector<TaskRow> rows;
  rows.push_back(TaskRow{
      .order = "1",
      .filename =
          "failed-image-with-a-very-long-name-that-must-stay-contained.webp",
      .folder = "C:\\smoke\\input\\a-very-long-folder-name",
      .size = "4.2 MiB",
      .status = "失败",
      .output = "failed.avif",
      .log = "完整错误：测试 worker 返回了可滚动的详细错误文本。",
      .warning = true,
      .locked = true,
      .state = 3,
      .input_path = "C:\\smoke\\input\\failed-image.webp",
      .output_path = "C:\\smoke\\output\\failed-image.avif",
      .encoder = "aom",
      .threads = "1",
      .stage_timings =
          "decode 0.010s · prepare 0.002s · encode 1.250s · write 0.004s"});
  rows.push_back(TaskRow{.order = "2",
                         .filename = "completed.png",
                         .folder = "C:\\smoke\\input",
                         .size = "1.0 MiB",
                         .status = "完成",
                         .output = "completed.avif",
                         .log = {},
                         .warning = false,
                         .locked = true,
                         .state = 2,
                         .input_path = "C:\\smoke\\input\\completed.png",
                         .output_path = "C:\\smoke\\output\\completed.avif",
                         .encoder = "aom",
                         .threads = "1",
                         .stage_timings =
                             "decode 0.008s · prepare 0.001s · encode 0.900s · write 0.003s"});
  return std::make_shared<slint::VectorModel<TaskRow>>(std::move(rows));
}

std::shared_ptr<slint::VectorModel<UpdateHistoryRow>> update_history_rows() {
  std::vector<UpdateHistoryRow> rows;
  rows.push_back(UpdateHistoryRow{
      .version = "1.0.2",
      .channel = "prerelease",
      .published_at = "2026-08-10T00:00:00Z",
      .release_url = "https://github.com/Dominic485649/AWJimage/releases/tag/1.0.2",
      .changelog_zh_cn = "更新测试版",
      .changelog_en = "Update test build"});
  rows.push_back(UpdateHistoryRow{
      .version = "1.0.1",
      .channel = "stable",
      .published_at = "2026-08-09T00:00:00Z",
      .release_url = "https://github.com/Dominic485649/AWJimage/releases/tag/1.0.1",
      .changelog_zh_cn = "稳定版测试记录",
      .changelog_en = "Stable test release"});
  return std::make_shared<slint::VectorModel<UpdateHistoryRow>>(std::move(rows));
}

int run_scale(const slint::ComponentHandle<AwjStudio>& app,
              float scale_factor) {
  app->window().window_handle().set_const_scale_factor(scale_factor);
  app->window().set_size(slint::LogicalSize({820.0f, 560.0f}));
  app->set_ui_font_options(font_options());
  app->set_task_rows(task_rows());
  app->set_update_history(update_history_rows());
  app->set_queue_failed_count(1);
  app->set_queue_success_count(1);
  app->set_queue_failed_only(false);
  app->set_selected_queue_index(0);
  app->set_current_version("1.0.1");
  app->set_update_available(true);
  app->set_update_version("1.0.2");
  app->set_update_published_at("2026-08-10T00:00:00Z");
  app->set_update_changelog_zh_cn("更新测试版");
  app->set_update_changelog_en("Update test build");
  app->set_update_summary_zh_cn("更新测试版");
  app->set_update_summary_en("Update test build");
  app->set_show_update_changelog(true);
  int retries = 0;
  int version_clicks = 0;
  app->on_retry_failed([&retries] { ++retries; });
  app->on_version_clicked([&version_clicks] { ++version_clicks; });
  const std::array pages{
      std::pair{"编码队列", 1}, std::pair{"参数设置", 0},
      std::pair{"菜单参数", 3}, std::pair{"更新日志", 4},
      std::pair{"设置", 2}};
  for (const auto& [label, page] : pages) {
    auto element =
        find_one(app, label, slint::language::AccessibleRole::Tab);
    if (!element || element->accessible_role() !=
                        slint::language::AccessibleRole::Tab) {
      const auto window_size = app->window().size();
      const auto matches =
          slint::testing::ElementHandle::find_by_accessible_label(app, label);
      std::fprintf(stderr,
                   "navigation diagnostic: scale=%.1f window=%ux%u label=%s "
                   "matches=%zu\n",
                   static_cast<double>(scale_factor), window_size.width,
                   window_size.height, label, matches.size());
      for (const auto& match : matches) {
        const auto position = match.absolute_position();
        const auto size = match.size();
        std::fprintf(stderr,
                     "  role=%d pos=(%.1f,%.1f) size=(%.1f,%.1f)\n",
                     match.accessible_role()
                         ? static_cast<int>(*match.accessible_role())
                         : -1,
                     static_cast<double>(position.x),
                     static_cast<double>(position.y),
                     static_cast<double>(size.width),
                     static_cast<double>(size.height));
      }
      return fail("navigation item is missing its tab role or name");
    }
    element->invoke_accessible_default_action();
    if (app->get_selected_page() != page) {
      return fail("navigation accessibility action did not switch page");
    }
  }

  app->set_selected_page(1);
  send_key(app, slint::platform::key_codes::Tab);
  send_key(app, slint::platform::key_codes::Return);
  send_key(app, slint::platform::key_codes::DownArrow);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_selected_page() != 0) {
    return fail("Tab/Down/Enter navigation failed");
  }
  send_key(app, slint::platform::key_codes::End);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_selected_page() != 2) {
    return fail("End navigation failed");
  }
  send_key(app, slint::platform::key_codes::UpArrow);
  send_text_key(app, " ");
  if (app->get_selected_page() != 4) {
    return fail("Up/Space navigation did not reach the changelog page");
  }
  send_key(app, slint::platform::key_codes::UpArrow);
  send_text_key(app, " ");
  if (app->get_selected_page() != 3) {
    return fail("second Up/Space navigation did not reach menu settings");
  }
  send_key(app, slint::platform::key_codes::Home);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_selected_page() != 1) {
    return fail("Home navigation failed");
  }

  auto settings =
      find_one(app, "设置", slint::language::AccessibleRole::Tab);
  settings->invoke_accessible_default_action();
  auto font =
      find_one(app, "字体", slint::language::AccessibleRole::Combobox);
  if (!font || font->accessible_role() !=
                   slint::language::AccessibleRole::Combobox) {
    return fail("font combobox is missing its role or name");
  }
  slint::cbindgen_private::slint_testing_use_native_popup(
      &app->window().window_handle(), false);
  font->invoke_accessible_default_action();
  if (slint::cbindgen_private::slint_testing_active_popup_count(
          &app->window().window_handle()) != 1) {
    return fail("font popup did not open");
  }
  send_key(app, slint::platform::key_codes::End);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_ui_font_index() != 14) {
    return fail("font End selection did not reach the last item");
  }
  font->invoke_accessible_default_action();
  send_key(app, slint::platform::key_codes::Home);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_ui_font_index() != 0) {
    return fail("font Home selection did not reach the first item");
  }
  font->invoke_accessible_default_action();
  send_key(app, slint::platform::key_codes::Escape);
  if (slint::cbindgen_private::slint_testing_active_popup_count(
          &app->window().window_handle()) != 0) {
    return fail("font Escape did not close the popup");
  }
  const auto font_position = font->absolute_position();
  const auto font_size = font->size();
  app->window().dispatch_pointer_scroll_event(
      slint::LogicalPosition(
          {font_position.x + font_size.width / 2.0f,
           font_position.y + font_size.height / 2.0f}),
      0.0f, -36.0f);
  if (app->get_ui_font_index() != 1) {
    return fail(std::format(
        "focused font combobox did not handle the mouse wheel: scale={:.1f} "
        "index={} pos=({:.1f},{:.1f}) size=({:.1f},{:.1f})",
        scale_factor, app->get_ui_font_index(), font_position.x,
        font_position.y, font_size.width, font_size.height));
  }

  app->set_theme_index(1);
  if (app->get_theme_index() != 1) {
    return fail("light theme did not apply");
  }
  app->set_theme_index(2);
  if (app->get_theme_index() != 2) {
    return fail("dark theme did not apply");
  }

  app->set_selected_page(4);
  app->set_show_update_changelog(false);
  if (find_one(app, "更新日志", slint::language::AccessibleRole::Tab)) {
    return fail("hidden changelog remains in the accessible navigation tree");
  }
  auto version = find_one(app, "当前版本 1.0.1",
                          slint::language::AccessibleRole::Button);
  if (!version) {
    return fail("hiding the changelog also hid the version/update control");
  }
  version->invoke_accessible_default_action();
  if (version_clicks != 1) {
    return fail("version accessibility action did not fire");
  }
  app->set_selected_page(2);
  app->set_show_update_changelog(true);

  // 界面语言切换。中文是 @tr() 的 msgid 原文（语言索引 0），English 来自
  // ui/translations/en/LC_MESSAGES/awj.po 的 bundled 翻译。
  // select_bundled_translation 写 translations_dirty 这个真实属性，所有 @tr()
  // 绑定都会重算——可访问名也是 @tr() 的，所以这里直接用它来证明切换生效，
  // 而不是只检查那个 int 属性被写进去了。
  app->set_selected_page(0);
  if (!find_one(app, "输出格式")) {
    return fail("Chinese is not the default UI language");
  }
  if (!slint::select_bundled_translation("en")) {
    return fail("no bundled English translation; check ui/translations and the "
                "--bundle-translations flag in CMakeLists.txt");
  }
  app->set_language_index(1);
  if (!find_one(app, "Output format")) {
    return fail("switching to English did not retranslate accessible names");
  }
  if (!find_one(app, "Changelog", slint::language::AccessibleRole::Tab)) {
    return fail("the new changelog navigation item was not translated");
  }
  if (find_one(app, "输出格式")) {
    return fail("Chinese accessible name survived the switch to English");
  }
  // 切回中文：后面的断言仍按中文可访问名查找。
  if (!slint::select_bundled_translation("")) {
    return fail("could not switch back to the default language");
  }
  app->set_language_index(0);
  if (!find_one(app, "输出格式")) {
    return fail("switching back to Chinese did not restore the msgid text");
  }

  app->set_selected_page(1);
  if (!find_one(app, "输入路径",
                slint::language::AccessibleRole::TextInput) ||
      !find_one(app, "输出目录",
                slint::language::AccessibleRole::TextInput)) {
    return fail("queue path fields are missing accessible names or roles");
  }
  auto queue_list =
      find_one(app, "编码任务", slint::language::AccessibleRole::List);
  if (!queue_list || queue_list->accessible_item_count() != 2) {
    return fail("queue list is missing its role, name, or item count");
  }
  if (queue_list->size().height < 34.0f) {
    return fail(std::format(
        "queue list has less than one visible row at minimum size: {:.1f}px",
        queue_list->size().height));
  }
  app->set_queue_failed_only(true);
  if (!app->get_queue_failed_only() ||
      queue_list->accessible_item_count() != 1) {
    return fail("failed-only filter did not update the accessible item count");
  }
  auto retry =
      find_one(app, "重试失败", slint::language::AccessibleRole::Button);
  if (!retry || retry->accessible_role() !=
                    slint::language::AccessibleRole::Button) {
    return fail("retry button is missing its role or name");
  }
  retry->invoke_accessible_default_action();
  if (retries != 1) {
    return fail("retry accessibility action did not fire");
  }
  if (!find_one(app, "关闭详情", slint::language::AccessibleRole::Button)) {
    return fail("selected-item details did not open");
  }

  app->set_queue_failed_only(false);
  app->invoke_open_queue_menu(0, 360.0f, 220.0f);
  if (slint::cbindgen_private::slint_testing_active_popup_count(
          &app->window().window_handle()) != 1) {
    return fail("queue context menu did not open");
  }
  send_key(app, slint::platform::key_codes::End);
  send_key(app, slint::platform::key_codes::Escape);
  if (slint::cbindgen_private::slint_testing_active_popup_count(
          &app->window().window_handle()) != 0) {
    return fail("queue context menu did not handle End/Escape");
  }

  bool invalid_geometry = false;
  slint::testing::ElementHandle::visit_elements(
      app, [&invalid_geometry, scale_factor](slint::testing::ElementHandle element) {
        if (!element.accessible_role() ||
            *element.accessible_role() ==
                slint::language::AccessibleRole::None) {
          return;
        }
        const auto size = element.size();
        const auto position = element.absolute_position();
        if (size.width <= 0.0f || size.height <= 0.0f ||
            position.x + size.width < 0.0f ||
            position.y + size.height < 0.0f || position.x > 820.0f ||
            position.y > 560.0f) {
          invalid_geometry = true;
          const auto label = element.accessible_label();
          const auto id = element.id();
          std::fprintf(
              stderr,
              "clipped accessible control: scale=%.1f pos=(%.1f,%.1f) "
              "size=(%.1f,%.1f) label=%s id=%s\n",
              static_cast<double>(scale_factor), static_cast<double>(position.x),
              static_cast<double>(position.y), static_cast<double>(size.width),
              static_cast<double>(size.height),
              label ? label->data() : "<unnamed>", id ? id->data() : "<none>");
        }
      });
  if (invalid_geometry) {
    return fail("an accessible control is clipped outside the minimum window");
  }

  return 0;
}

}  // namespace

int main() {
  slint::testing::init();
  auto app = AwjStudio::create();
  app->show();
  for (const float scale : {1.0f, 1.5f, 2.0f}) {
    if (const int result = run_scale(app, scale); result != 0) {
      return result;
    }
  }
  app->hide();
  return 0;
}
