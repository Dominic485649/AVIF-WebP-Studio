module;

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>

#ifndef AWJ_UPDATE_PUBLIC_KEY_HEX
#define AWJ_UPDATE_PUBLIC_KEY_HEX ""
#endif

export module awj.update_manifest;

import awj.update_model;

export namespace awj::update {

inline constexpr std::string_view manifest_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-manifest.json";
inline constexpr std::string_view manifest_signature_url =
    "https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-manifest.json.sig";
inline constexpr std::size_t maximum_manifest_bytes = 1024 * 1024;
inline constexpr std::size_t maximum_signature_bytes = 4096;
inline constexpr std::uint64_t maximum_asset_bytes = 512ull * 1024 * 1024;

bool update_public_key_configured() noexcept {
  constexpr std::string_view public_key_hex = AWJ_UPDATE_PUBLIC_KEY_HEX;
  return public_key_hex.size() == crypto_sign_PUBLICKEYBYTES * 2 &&
         std::ranges::all_of(public_key_hex, [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool valid_update_key_id(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 64 &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                  ch == '-';
         });
}

std::expected<void, std::string> verify_detached_ed25519_signature(
    std::string_view raw_bytes, std::string_view signature_base64,
    std::string_view public_key_hex, std::string_view label) {
  if (sodium_init() < 0) {
    return std::unexpected{"初始化 Ed25519 验签库失败。"};
  }
  if (public_key_hex.size() != crypto_sign_PUBLICKEYBYTES * 2 ||
      !std::ranges::all_of(public_key_hex, [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      })) {
    return std::unexpected{std::format("{} 的 Ed25519 公钥格式错误。", label)};
  }
  std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
  std::size_t public_key_length = 0;
  if (sodium_hex2bin(public_key.data(), public_key.size(), public_key_hex.data(),
                     public_key_hex.size(), nullptr, &public_key_length,
                     nullptr) != 0 ||
      public_key_length != public_key.size()) {
    return std::unexpected{std::format("{} 的 Ed25519 公钥格式错误。", label)};
  }
  std::string signature_text{signature_base64};
  while (!signature_text.empty() &&
         std::isspace(static_cast<unsigned char>(signature_text.back()))) {
    signature_text.pop_back();
  }
  while (!signature_text.empty() &&
         std::isspace(static_cast<unsigned char>(signature_text.front()))) {
    signature_text.erase(signature_text.begin());
  }
  std::array<unsigned char, crypto_sign_BYTES> signature{};
  std::size_t signature_length = 0;
  if (sodium_base642bin(signature.data(), signature.size(),
                        signature_text.data(), signature_text.size(), nullptr,
                        &signature_length, nullptr,
                        sodium_base64_VARIANT_ORIGINAL) != 0 ||
      signature_length != signature.size()) {
    return std::unexpected{std::format("{} 不是有效的 Ed25519 签名。", label)};
  }
  if (crypto_sign_verify_detached(
          signature.data(),
          reinterpret_cast<const unsigned char*>(raw_bytes.data()),
          static_cast<unsigned long long>(raw_bytes.size()),
          public_key.data()) != 0) {
    return std::unexpected{std::format("{} 的 Ed25519 签名验证失败。", label)};
  }
  return {};
}

struct HttpsUrl {
  std::string host{};
  std::string path{};
};

// URL validation is shared by the parser and the HTTP redirect loop. Keep the
// allowlist exact: a valid signature may still contain a malicious URL after a
// signing-key or release-pipeline compromise.
std::expected<HttpsUrl, std::string> parse_allowed_https_url(
    std::string_view url) {
  constexpr std::string_view prefix = "https://";
  if (!url.starts_with(prefix) || url.size() > 4096) {
    return std::unexpected{"更新地址必须是长度受限的 HTTPS URL。"};
  }
  if (std::ranges::any_of(url, [](unsigned char ch) {
        return ch <= 0x20 || ch == 0x7f || ch == '\\';
      })) {
    return std::unexpected{"更新地址包含空白、控制字符或反斜杠。"};
  }
  const auto authority_begin = prefix.size();
  const auto path_begin = url.find('/', authority_begin);
  if (path_begin == std::string_view::npos || path_begin == authority_begin) {
    return std::unexpected{"更新地址缺少主机或路径。"};
  }
  auto authority = url.substr(authority_begin, path_begin - authority_begin);
  if (authority.contains('@')) {
    return std::unexpected{"更新地址不得包含用户信息。"};
  }
  if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
    if (authority.substr(colon) != ":443") {
      return std::unexpected{"更新地址只允许默认 HTTPS 端口。"};
    }
    authority = authority.substr(0, colon);
  }
  std::string host{authority};
  std::ranges::transform(host, host.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  constexpr std::array allowed_hosts{
      std::string_view{"raw.githubusercontent.com"},
      std::string_view{"github.com"},
      std::string_view{"release-assets.githubusercontent.com"},
      std::string_view{"objects.githubusercontent.com"}};
  if (std::ranges::find(allowed_hosts, host) == allowed_hosts.end()) {
    return std::unexpected{std::format("更新地址主机不在白名单中：{}", host)};
  }
  if (url.find('#', path_begin) != std::string_view::npos) {
    return std::unexpected{"更新地址不得包含 fragment。"};
  }
  return HttpsUrl{.host = std::move(host),
                  .path = std::string{url.substr(path_begin)}};
}

namespace manifest_detail {

using Json = nlohmann::json;

std::expected<std::string, std::string> required_string(
    const Json& object, std::string_view name, std::size_t maximum) {
  const auto key = std::string{name};
  if (!object.contains(key) || !object.at(key).is_string()) {
    return std::unexpected{std::format("manifest 字段 {} 必须是字符串。", name)};
  }
  auto value = object.at(key).get<std::string>();
  if (value.empty() || value.size() > maximum || value.contains('\0')) {
    return std::unexpected{std::format("manifest 字段 {} 长度非法。", name)};
  }
  return value;
}

std::expected<AssetInfo, std::string> parse_asset(const Json& value,
                                                   std::string_view name) {
  if (!value.is_object()) {
    return std::unexpected{std::format("manifest 资产 {} 必须是对象。", name)};
  }
  auto url = required_string(value, "url", 4096);
  if (!url) {
    return std::unexpected{url.error()};
  }
  if (auto allowed = parse_allowed_https_url(*url); !allowed) {
    return std::unexpected{std::format("manifest 资产 {}：{}", name,
                                       allowed.error())};
  }
  if (!value.contains("size") || !value.at("size").is_number_unsigned()) {
    return std::unexpected{std::format("manifest 资产 {} 缺少无符号 size。", name)};
  }
  const auto size = value.at("size").get<std::uint64_t>();
  if (size == 0 || size > maximum_asset_bytes) {
    return std::unexpected{std::format("manifest 资产 {} 的 size 超出限制。", name)};
  }
  auto sha256 = required_string(value, "sha256", 64);
  if (!sha256 || sha256->size() != 64 ||
      !std::ranges::all_of(*sha256, [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      })) {
    return std::unexpected{std::format(
        "manifest 资产 {} 的 SHA-256 必须是 64 位小写十六进制。", name)};
  }
  return AssetInfo{.url = std::move(*url),
                   .size_bytes = size,
                   .sha256 = std::move(*sha256)};
}

bool valid_rfc3339_utc(std::string_view value) noexcept {
  if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value[19] != 'Z') {
    return false;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == 19) {
      continue;
    }
    if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }
  const auto two_digits = [&](std::size_t offset) noexcept {
    return static_cast<unsigned>(value[offset] - '0') * 10u +
           static_cast<unsigned>(value[offset + 1] - '0');
  };
  const auto four_digits = [&](std::size_t offset) noexcept {
    return two_digits(offset) * 100u + two_digits(offset + 2);
  };
  const auto year = four_digits(0);
  const auto month = two_digits(5);
  const auto day = two_digits(8);
  const auto hour = two_digits(11);
  const auto minute = two_digits(14);
  const auto second = two_digits(17);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr std::array days_per_month{31u, 28u, 31u, 30u, 31u, 30u,
                                      31u, 31u, 30u, 31u, 30u, 31u};
  auto maximum_day = days_per_month[month - 1];
  const bool leap = year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
  if (month == 2 && leap) {
    ++maximum_day;
  }
  return day >= 1 && day <= maximum_day;
}

std::expected<ManifestEntry, std::string> parse_entry(const Json& value) {
  if (!value.is_object()) {
    return std::unexpected{"manifest entries 的元素必须是对象。"};
  }
  auto version_text = required_string(value, "version", 64);
  auto channel_text = required_string(value, "channel", 32);
  auto release_url = required_string(value, "release_url", 4096);
  auto published_at = required_string(value, "published_at", 64);
  auto minimum_text = required_string(value, "minimum_updater_version", 64);
  if (!version_text) return std::unexpected{version_text.error()};
  if (!channel_text) return std::unexpected{channel_text.error()};
  if (!release_url) return std::unexpected{release_url.error()};
  if (!published_at) return std::unexpected{published_at.error()};
  if (!minimum_text) return std::unexpected{minimum_text.error()};
  auto version = parse_version(*version_text);
  auto channel = parse_channel(*channel_text);
  auto minimum = parse_version(*minimum_text);
  if (!version) return std::unexpected{version.error()};
  if (!channel) return std::unexpected{channel.error()};
  if (!minimum) return std::unexpected{minimum.error()};
  if (!valid_rfc3339_utc(*published_at)) {
    return std::unexpected{"manifest published_at 必须是 YYYY-MM-DDTHH:MM:SSZ。"};
  }
  auto parsed_release = parse_allowed_https_url(*release_url);
  const auto expected_release_path =
      std::format("/Dominic485649/AWJimage/releases/tag/{}", *version_text);
  if (!parsed_release || parsed_release->host != "github.com" ||
      parsed_release->path != expected_release_path) {
    return std::unexpected{"manifest release_url 必须指向 AWJimage GitHub Release。"};
  }
  if (!value.contains("assets") || !value.at("assets").is_object()) {
    return std::unexpected{"manifest entry 缺少 assets 对象。"};
  }
  const auto& assets = value.at("assets");
  auto windows_exe = parse_asset(assets.value("windows_x64_exe", Json{}),
                                 "windows_x64_exe");
  auto windows_com = parse_asset(assets.value("windows_x64_com", Json{}),
                                 "windows_x64_com");
  auto linux = parse_asset(assets.value("linux_x64", Json{}), "linux_x64");
  if (!windows_exe) return std::unexpected{windows_exe.error()};
  if (!windows_com) return std::unexpected{windows_com.error()};
  if (!linux) return std::unexpected{linux.error()};
  if (!value.contains("changelog") || !value.at("changelog").is_object()) {
    return std::unexpected{"manifest entry 缺少 changelog 对象。"};
  }
  const auto& changelog = value.at("changelog");
  auto zh = required_string(changelog, "zh-CN", 256 * 1024);
  auto en = required_string(changelog, "en", 256 * 1024);
  if (!zh) return std::unexpected{zh.error()};
  if (!en) return std::unexpected{en.error()};
  bool revoked = false;
  if (value.contains("revoked")) {
    if (!value.at("revoked").is_boolean()) {
      return std::unexpected{"manifest revoked 必须是布尔值。"};
    }
    revoked = value.at("revoked").get<bool>();
  }
  return ManifestEntry{.version = *version,
                       .channel = *channel,
                       .release_url = std::move(*release_url),
                       .published_at = std::move(*published_at),
                       .minimum_updater_version = *minimum,
                       .windows_x64_exe = std::move(*windows_exe),
                       .windows_x64_com = std::move(*windows_com),
                       .linux_x64 = std::move(*linux),
                       .changelog = {.zh_cn = std::move(*zh),
                                     .en = std::move(*en)},
                       .revoked = revoked};
}

}  // namespace manifest_detail

std::expected<std::chrono::sys_seconds, std::string> parse_rfc3339_utc(
    std::string_view value, std::string_view field_name) {
  if (!manifest_detail::valid_rfc3339_utc(value)) {
    return std::unexpected{
        std::format("{} 必须是 YYYY-MM-DDTHH:MM:SSZ。", field_name)};
  }
  const auto two_digits = [&](std::size_t offset) noexcept {
    return static_cast<unsigned>(value[offset] - '0') * 10u +
           static_cast<unsigned>(value[offset + 1] - '0');
  };
  const auto year = static_cast<int>(two_digits(0) * 100u + two_digits(2));
  const auto month = two_digits(5);
  const auto day = two_digits(8);
  const auto hour = two_digits(11);
  const auto minute = two_digits(14);
  const auto second = two_digits(17);
  const auto ymd = std::chrono::year{year} / std::chrono::month{month} /
                   std::chrono::day{day};
  if (!ymd.ok()) {
    return std::unexpected{
        std::format("{} 包含无效日期。", field_name)};
  }
  return std::chrono::sys_days{ymd} + std::chrono::hours{hour} +
         std::chrono::minutes{minute} + std::chrono::seconds{second};
}

inline constexpr auto maximum_signed_update_document_lifetime =
    std::chrono::days{180};

std::expected<void, std::string> validate_signed_update_document_window(
    std::string_view issued_at, std::string_view expires_at,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) {
  auto issued = parse_rfc3339_utc(issued_at, "issued_at");
  auto expires = parse_rfc3339_utc(expires_at, "expires_at");
  if (!issued) return std::unexpected{issued.error()};
  if (!expires) return std::unexpected{expires.error()};
  if (*expires <= *issued || *expires - *issued > maximum_signed_update_document_lifetime) {
    return std::unexpected{"签名更新文档的 expires_at 必须晚于 issued_at 且有效期不超过 180 天。"};
  }
  const auto current = std::chrono::floor<std::chrono::seconds>(now);
  if (*issued > current + std::chrono::hours{24}) {
    return std::unexpected{"签名更新文档的 issued_at 明显晚于本机时间，已拒绝。"};
  }
  if (*expires <= current) {
    return std::unexpected{"签名更新文档已过期，已拒绝以防止 freeze attack。"};
  }
  return {};
}

std::expected<Manifest, std::string> parse_manifest_json(
    std::string_view raw_bytes) {
  if (raw_bytes.empty() || raw_bytes.size() > maximum_manifest_bytes ||
      raw_bytes.contains('\0') ||
      (raw_bytes.size() >= 3 &&
       static_cast<unsigned char>(raw_bytes[0]) == 0xef &&
       static_cast<unsigned char>(raw_bytes[1]) == 0xbb &&
       static_cast<unsigned char>(raw_bytes[2]) == 0xbf)) {
    return std::unexpected{"manifest 为空、过大、含 NUL 或带 UTF-8 BOM。"};
  }
  try {
    auto json = manifest_detail::Json::parse(raw_bytes.begin(), raw_bytes.end());
    if (!json.is_object() || !json.contains("schema") ||
        !json.at("schema").is_number_unsigned() || !json.contains("sequence") ||
        !json.at("sequence").is_number_unsigned() || !json.contains("key_id") ||
        !json.contains("issued_at") || !json.contains("expires_at") ||
        !json.contains("entries") || !json.at("entries").is_array()) {
      return std::unexpected{"manifest 根对象字段不完整或类型错误。"};
    }
    auto key_id = manifest_detail::required_string(json, "key_id", 64);
    auto issued_at = manifest_detail::required_string(json, "issued_at", 20);
    auto expires_at = manifest_detail::required_string(json, "expires_at", 20);
    if (!key_id || !issued_at || !expires_at) {
      return std::unexpected{!key_id ? key_id.error()
                          : !issued_at ? issued_at.error()
                                       : expires_at.error()};
    }
    if (!valid_update_key_id(*key_id)) {
      return std::unexpected{"manifest key_id 只能包含小写字母、数字和连字符。"};
    }
    auto issued = parse_rfc3339_utc(*issued_at, "manifest issued_at");
    auto expires = parse_rfc3339_utc(*expires_at, "manifest expires_at");
    if (!issued || !expires || *expires <= *issued ||
        *expires - *issued > maximum_signed_update_document_lifetime) {
      return std::unexpected{"manifest 的 issued_at/expires_at 有效期非法。"};
    }
    Manifest manifest{.schema = json.at("schema").get<std::uint32_t>(),
                      .sequence = json.at("sequence").get<std::uint64_t>(),
                      .key_id = std::move(*key_id),
                      .issued_at = std::move(*issued_at),
                      .expires_at = std::move(*expires_at)};
    if (manifest.sequence == 0 || json.at("entries").size() > 256) {
      return std::unexpected{"manifest sequence 或 entries 数量非法。"};
    }
    manifest.entries.reserve(json.at("entries").size());
    for (const auto& value : json.at("entries")) {
      auto entry = manifest_detail::parse_entry(value);
      if (!entry) {
        return std::unexpected{entry.error()};
      }
      manifest.entries.push_back(std::move(*entry));
    }
    if (auto consistent = validate_manifest_consistency(manifest); !consistent) {
      return std::unexpected{consistent.error()};
    }
    return manifest;
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected{std::format("解析 update-manifest.json 失败：{}",
                                       error.what())};
  } catch (const std::exception& error) {
    return std::unexpected{std::format("读取 update-manifest.json 失败：{}",
                                       error.what())};
  }
}

std::expected<void, std::string> verify_manifest_signature(
    std::string_view raw_bytes, std::string_view signature_base64) {
  constexpr std::string_view public_key_hex = AWJ_UPDATE_PUBLIC_KEY_HEX;
  if (!update_public_key_configured()) {
    return std::unexpected{
        "此构建未配置有效的更新公钥，已拒绝联网更新。"};
  }
  return verify_detached_ed25519_signature(raw_bytes, signature_base64,
                                           public_key_hex,
                                           "update-manifest.json.sig");
}

std::expected<Manifest, std::string> verify_and_parse_manifest(
    std::string_view raw_bytes, std::string_view signature_base64) {
  if (auto verified = verify_manifest_signature(raw_bytes, signature_base64);
      !verified) {
    return std::unexpected{verified.error()};
  }
  auto manifest = parse_manifest_json(raw_bytes);
  if (!manifest) return std::unexpected{manifest.error()};
  if (auto valid = validate_signed_update_document_window(
          manifest->issued_at, manifest->expires_at);
      !valid) {
    return std::unexpected{valid.error()};
  }
  return manifest;
}

}  // namespace awj::update
