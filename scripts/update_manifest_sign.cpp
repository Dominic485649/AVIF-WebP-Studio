#include <sodium.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) return {};
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

bool decode_seed(std::string& text,
                 std::array<unsigned char, crypto_sign_SEEDBYTES>& seed) {
  std::string_view trimmed{text};
  while (!trimmed.empty() &&
         (trimmed.back() == '\r' || trimmed.back() == '\n' ||
          trimmed.back() == ' ' || trimmed.back() == '\t')) {
    trimmed.remove_suffix(1);
  }
  std::size_t length = 0;
  const bool valid =
      trimmed.size() == seed.size() * 2 &&
      sodium_hex2bin(seed.data(), seed.size(), trimmed.data(), trimmed.size(),
                     nullptr, &length, nullptr) == 0 &&
      length == seed.size();
  sodium_memzero(text.data(), text.size());
  return valid;
}

void trim_ascii_space(std::string& text) {
  while (!text.empty() &&
         (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' ||
          text.back() == '\t')) {
    text.pop_back();
  }
  const auto first = text.find_first_not_of("\r\n \t");
  if (first == std::string::npos) {
    text.clear();
  } else if (first != 0) {
    text.erase(0, first);
  }
}

int verify_manifest(const std::filesystem::path& manifest_path,
                    std::string_view public_key_hex,
                    const std::filesystem::path& signature_path) {
  if (public_key_hex.size() != crypto_sign_PUBLICKEYBYTES * 2) return 8;
  std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
  std::size_t public_key_length = 0;
  if (sodium_hex2bin(public_key.data(), public_key.size(),
                     public_key_hex.data(), public_key_hex.size(), nullptr,
                     &public_key_length, nullptr) != 0 ||
      public_key_length != public_key.size()) {
    return 8;
  }
  const auto manifest = read_file(manifest_path);
  auto signature_text = read_file(signature_path);
  trim_ascii_space(signature_text);
  if (manifest.empty() || signature_text.empty()) return 9;
  std::array<unsigned char, crypto_sign_BYTES> signature{};
  std::size_t signature_length = 0;
  if (sodium_base642bin(signature.data(), signature.size(),
                        signature_text.data(), signature_text.size(), nullptr,
                        &signature_length, nullptr,
                        sodium_base64_VARIANT_ORIGINAL) != 0 ||
      signature_length != signature.size()) {
    return 10;
  }
  return crypto_sign_verify_detached(
             signature.data(),
             reinterpret_cast<const unsigned char*>(manifest.data()),
             static_cast<unsigned long long>(manifest.size()),
             public_key.data()) == 0
             ? 0
             : 11;
}

}  // namespace

int main(int argc, char* argv[]) {
  const bool print_public_key =
      argc == 3 && std::string_view{argv[1]} == "--print-public-key";
  const bool verify =
      argc == 5 && std::string_view{argv[1]} == "--verify";
  if (!print_public_key && !verify && argc != 4) {
    std::cerr
        << "usage: awj_update_manifest_sign <manifest> <seed-file> <signature>\n"
        << "       awj_update_manifest_sign --print-public-key <seed-file>\n"
        << "       awj_update_manifest_sign --verify <manifest> <public-key-hex> "
           "<signature>\n";
    return 2;
  }
  if (sodium_init() < 0) return 3;
  if (verify) return verify_manifest(argv[2], argv[3], argv[4]);
  auto seed_text = read_file(argv[2]);
  if (seed_text.empty()) return 4;
  std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
  if (!decode_seed(seed_text, seed)) {
    std::cerr << "seed file must contain exactly 64 hexadecimal characters\n";
    return 5;
  }
  std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
  std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret_key{};
  crypto_sign_seed_keypair(public_key.data(), secret_key.data(), seed.data());

  std::array<char, crypto_sign_PUBLICKEYBYTES * 2 + 1> public_hex{};
  sodium_bin2hex(public_hex.data(), public_hex.size(), public_key.data(),
                 public_key.size());
  if (!print_public_key) {
    const auto manifest = read_file(argv[1]);
    if (manifest.empty()) {
      sodium_memzero(seed.data(), seed.size());
      sodium_memzero(secret_key.data(), secret_key.size());
      return 4;
    }
    std::array<unsigned char, crypto_sign_BYTES> signature{};
    crypto_sign_detached(
        signature.data(), nullptr,
        reinterpret_cast<const unsigned char*>(manifest.data()), manifest.size(),
        secret_key.data());
    std::array<char, sodium_base64_ENCODED_LEN(
                         crypto_sign_BYTES, sodium_base64_VARIANT_ORIGINAL)>
        signature_base64{};
    sodium_bin2base64(signature_base64.data(), signature_base64.size(),
                      signature.data(), signature.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    std::ofstream output{argv[3], std::ios::binary | std::ios::trunc};
    if (!output) {
      sodium_memzero(seed.data(), seed.size());
      sodium_memzero(secret_key.data(), secret_key.size());
      return 6;
    }
    output << signature_base64.data() << '\n';
    output.close();
    if (!output) {
      sodium_memzero(seed.data(), seed.size());
      sodium_memzero(secret_key.data(), secret_key.size());
      return 7;
    }
  }
  sodium_memzero(seed.data(), seed.size());
  sodium_memzero(secret_key.data(), secret_key.size());
  std::cout << public_hex.data() << '\n';
  return 0;
}
