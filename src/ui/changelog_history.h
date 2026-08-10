#pragma once

#include "awj_changelog_data.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace awj::ui {

struct ChangelogHistoryEntry {
  std::string version{};
  std::string channel{};
  std::string published_at{};
  std::string release_url{};
  std::string changelog_zh_cn{};
  std::string changelog_en{};
};

namespace changelog_detail {

struct Section {
  std::string_view heading{};
  std::string_view body{};
};

inline std::vector<Section> parse_sections(std::string_view source) {
  std::vector<Section> sections;
  std::vector<std::pair<std::size_t, std::size_t>> headings;

  std::size_t cursor = 0;
  while (cursor < source.size()) {
    const auto line_end = source.find('\n', cursor);
    const auto end = line_end == std::string_view::npos ? source.size() : line_end;
    auto line = source.substr(cursor, end - cursor);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.starts_with("## ")) headings.emplace_back(cursor, end);
    cursor = end == source.size() ? source.size() : end + 1;
  }

  sections.reserve(headings.size());
  for (std::size_t index = 0; index < headings.size(); ++index) {
    const auto [heading_begin, heading_end] = headings[index];
    auto heading = source.substr(heading_begin + 3, heading_end - heading_begin - 3);
    if (!heading.empty() && heading.back() == '\r') heading.remove_suffix(1);
    const auto body_begin = heading_end == source.size() ? source.size() : heading_end + 1;
    const auto body_end = index + 1 < headings.size() ? headings[index + 1].first : source.size();
    auto body = source.substr(body_begin, body_end - body_begin);
    while (!body.empty() && (body.front() == '\n' || body.front() == '\r')) body.remove_prefix(1);
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.remove_suffix(1);
    sections.push_back(Section{.heading = heading, .body = body});
  }
  return sections;
}

inline std::vector<ChangelogHistoryEntry> parse(std::string_view chinese,
                                                std::string_view english) {
  const auto zh_sections = parse_sections(chinese);
  const auto en_sections = parse_sections(english);
  std::vector<ChangelogHistoryEntry> history;
  history.reserve(zh_sections.size());
  for (const auto& section : zh_sections) {
    const auto separator = section.heading.find(" - ");
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 3 >= section.heading.size() || section.body.empty()) {
      continue;
    }
    const auto version = section.heading.substr(0, separator);
    const auto date = section.heading.substr(separator + 3);
    std::string english_body;
    for (const auto& english_section : en_sections) {
      if (english_section.heading.starts_with(version) &&
          english_section.heading.size() > version.size() &&
          english_section.heading[version.size()] == ' ' &&
          !english_section.body.empty()) {
        english_body = std::string{english_section.body};
        break;
      }
    }
    if (english_body.empty()) continue;
    history.push_back(ChangelogHistoryEntry{
        .version = std::string{version},
        .published_at = std::string{date},
        .changelog_zh_cn = std::string{section.body},
        .changelog_en = std::move(english_body)});
  }
  return history;
}

}  // namespace changelog_detail

inline std::vector<ChangelogHistoryEntry> embedded_changelog_history() {
  return changelog_detail::parse(embedded_changelog::zh, embedded_changelog::en);
}

}  // namespace awj::ui
