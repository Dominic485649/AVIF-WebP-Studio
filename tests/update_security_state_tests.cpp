#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

import awj.update_security_state;

namespace {

namespace fs = std::filesystem;
using awj::update::UpdateSecurityDocument;

int fail(std::string_view message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view{argv[1]} == "--child") {
    const auto accepted = awj::update::accept_verified_update_document(
        UpdateSecurityDocument::archive_manifest_v2, 6,
        "signed-v2-after-restart", 0, fs::path{argv[2]});
    return accepted ? 0 : fail(accepted.error());
  }
  if (argc != 1) return fail("unexpected test arguments");
  const auto root = fs::temp_directory_path() /
                    std::format("awj-update-security-state-test-{}",
                                std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count());
  std::error_code ec;
  fs::create_directory(root, ec);
  if (ec) return fail("cannot create security-state fixture directory");
  const auto clean = [&] { fs::remove_all(root, ec); };

  if (auto accepted = awj::update::accept_verified_update_document(
          UpdateSecurityDocument::archive_manifest_v2, 5, "signed-v2-a", 0, root);
      !accepted) {
    clean();
    return fail(accepted.error());
  }
  if (auto accepted = awj::update::accept_verified_update_document(
          UpdateSecurityDocument::archive_manifest_v2, 5, "signed-v2-a", 0, root);
      !accepted) {
    clean();
    return fail("the same verified v2 document was not idempotent");
  }
  if (awj::update::accept_verified_update_document(
          UpdateSecurityDocument::archive_manifest_v2, 5, "signed-v2-b", 0, root)) {
    clean();
    return fail("a different document at the same sequence was accepted");
  }
  if (awj::update::accept_verified_update_document(
          UpdateSecurityDocument::archive_manifest_v2, 4, "signed-v2-old", 0, root)) {
    clean();
    return fail("a replayed lower v2 sequence was accepted");
  }
  // Reopen the state from a distinct executable process.  The child advances
  // v2; this process must observe that durable monotonic value after it exits.
  // cmd.exe needs an outer quote pair when the executable path starts quoted.
#ifdef _WIN32
  const auto child_command = std::format("\"\"{}\" --child \"{}\"\"",
                                         fs::absolute(argv[0]).string(),
                                         root.string());
#else
  const auto child_command = std::format("'{}' --child '{}'",
                                         fs::absolute(argv[0]).string(),
                                         root.string());
#endif
  if (std::system(child_command.c_str()) != 0) {
    clean();
    return fail("a second process could not persist the update security state");
  }
  if (auto accepted = awj::update::accept_verified_update_document(
          UpdateSecurityDocument::archive_manifest_v2, 6,
          "signed-v2-after-restart", 0, root);
      !accepted) {
    clean();
    return fail("the parent did not observe the child process security state");
  }
  if (awj::update::accept_verified_update_document(
          UpdateSecurityDocument::archive_manifest_v2, 5, "signed-v2-a", 0, root)) {
    clean();
    return fail("a child-persisted sequence was replayable after restart");
  }
  // A legacy UI sequence can seed the new state on the first 1.0.6 run, but
  // it cannot then be used to overwrite the recorded digest.
  if (auto accepted = awj::update::accept_verified_update_document(
          UpdateSecurityDocument::legacy_manifest, 8, "signed-v1", 8, root);
      !accepted) {
    clean();
    return fail("the one-time legacy sequence migration was rejected");
  }

  const auto state_path = root / awj::update::update_security_state_file_name;
#ifdef _WIN32
  if (SetFileAttributesW(state_path.c_str(), FILE_ATTRIBUTE_NORMAL) == FALSE) {
    clean();
    return fail("cannot clear the hidden attribute on the security-state fixture");
  }
#endif
  {
    std::ofstream output{state_path, std::ios::binary | std::ios::trunc};
    output << "{broken";
    output.flush();
    if (!output) {
      clean();
      return fail("cannot corrupt the persisted security-state fixture");
    }
  }
  if (fs::file_size(state_path, ec) != 7 || ec) {
    clean();
    return fail("the corrupt security-state fixture was not written");
  }
  if (awj::update::accept_verified_update_document(
          UpdateSecurityDocument::keyring, 1, "signed-keyring", 0, root)) {
    clean();
    return fail("a corrupt persisted security state did not fail closed");
  }
  fs::remove(state_path, ec);
  fs::create_directory(state_path, ec);
  if (ec || awj::update::accept_verified_update_document(
                UpdateSecurityDocument::keyring, 1, "signed-keyring", 0, root)) {
    clean();
    return fail("a non-regular persisted security state did not fail closed");
  }

  clean();
  std::cout << "update security-state tests passed\n";
  return 0;
}
