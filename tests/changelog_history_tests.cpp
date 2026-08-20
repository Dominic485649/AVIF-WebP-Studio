#include <iostream>
#include <string_view>

#include "changelog_history.h"

int main() {
  constexpr std::string_view zh =
      "## 1.0.4 - 2026-08-21\n\n- 新日志\n\n"
      "## 0.1 - 2026-05-16\n\n- 旧日志\n";
  constexpr std::string_view en = "## 1.0.4 - 2026-08-21\n\n- New log\n";
  const auto parsed = awj::ui::changelog_detail::parse(zh, en);
  if (parsed.size() != 2 || parsed[0].changelog_en != "- New log" ||
      parsed[1].changelog_en != parsed[1].changelog_zh_cn) {
    std::cerr << "legacy changelog fallback lost an entry\n";
    return 1;
  }

  const auto embedded = awj::ui::embedded_changelog_history();
  const auto all_zh = awj::ui::changelog_detail::parse_sections(
      awj::embedded_changelog::zh);
  if (embedded.size() != all_zh.size() || embedded.empty()) {
    std::cerr << "embedded changelog does not include every historical entry\n";
    return 1;
  }
  return 0;
}
