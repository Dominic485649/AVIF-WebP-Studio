#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<HANDLE> g_child_process{nullptr};

constexpr UINT kControlExitCode = 0xC000013A;

struct PipePair {
  HANDLE read = nullptr;
  HANDLE write = nullptr;
};

void close_if_valid(HANDLE handle) {
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle);
  }
}

BOOL WINAPI console_control_handler(DWORD control_type) {
  switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
      // The child AWJ.exe owns the real CLI cancellation path. Keep the shim
      // alive so it can drain pipes and return the child's final exit code.
      return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      break;
    default:
      return FALSE;
  }

  if (const HANDLE process = g_child_process.load(std::memory_order_acquire);
      process != nullptr) {
    TerminateProcess(process, kControlExitCode);
    return TRUE;
  }
  return FALSE;
}

std::wstring win32_error_message(DWORD error) {
  LPWSTR message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&message), 0, nullptr);
  if (length == 0 || message == nullptr) {
    return std::format(L"Win32 error {}", error);
  }
  std::wstring text{message, length};
  LocalFree(message);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                           text.back() == L' ' || text.back() == L'\t')) {
    text.pop_back();
  }
  return text;
}

std::expected<PipePair, std::wstring> create_forwarding_pipe(
    std::wstring_view label) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  PipePair pipe{};
  if (!CreatePipe(&pipe.read, &pipe.write, &security, 0)) {
    return std::unexpected{std::format(L"创建 {} pipe 失败：{}", label,
                                       win32_error_message(GetLastError()))};
  }
  if (!SetHandleInformation(pipe.read, HANDLE_FLAG_INHERIT, 0)) {
    const auto error = GetLastError();
    close_if_valid(pipe.read);
    close_if_valid(pipe.write);
    return std::unexpected{std::format(L"设置 {} pipe 继承属性失败：{}", label,
                                       win32_error_message(error))};
  }
  return pipe;
}

void forward_pipe_to_handle(HANDLE pipe, HANDLE target) {
  std::array<std::byte, 16 * 1024> buffer{};
  bool can_write = target != nullptr && target != INVALID_HANDLE_VALUE;

  while (true) {
    DWORD bytes_read = 0;
    const BOOL ok = ReadFile(pipe, buffer.data(),
                             static_cast<DWORD>(buffer.size()), &bytes_read,
                             nullptr);
    if (!ok || bytes_read == 0) {
      break;
    }

    DWORD offset = 0;
    while (can_write && offset < bytes_read) {
      DWORD bytes_written = 0;
      const BOOL wrote = WriteFile(target, buffer.data() + offset,
                                   bytes_read - offset, &bytes_written,
                                   nullptr);
      if (!wrote || bytes_written == 0) {
        can_write = false;
        break;
      }
      offset += bytes_written;
    }
  }

  close_if_valid(pipe);
}

std::wstring quote_windows_command_arg(std::wstring_view arg) {
  if (arg.empty()) {
    return L"\"\"";
  }
  const bool needs_quotes =
      arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needs_quotes) {
    return std::wstring{arg};
  }

  std::wstring quoted;
  quoted.push_back(L'\"');
  std::size_t backslashes = 0;
  for (wchar_t ch : arg) {
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
    if (backslashes != 0) {
      quoted.append(backslashes, L'\\');
      backslashes = 0;
    }
    quoted.push_back(ch);
  }
  if (backslashes != 0) {
    quoted.append(backslashes * 2, L'\\');
  }
  quoted.push_back(L'\"');
  return quoted;
}

std::wstring command_line_from_args(std::span<wchar_t* const> args) {
  std::wstring command;
  for (wchar_t* arg : args) {
    if (!command.empty()) {
      command.push_back(L' ');
    }
    command += quote_windows_command_arg(arg != nullptr ? std::wstring_view{arg}
                                                        : std::wstring_view{});
  }
  return command;
}

std::expected<fs::path, std::wstring> current_executable_path() {
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return std::unexpected{std::format(L"无法获取 AWJ.com 路径：{}。",
                                         win32_error_message(GetLastError()))};
    }
    if (length < buffer.size()) {
      buffer.resize(length);
      return fs::path{buffer};
    }
    if (buffer.size() >
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max() / 2)) {
      return std::unexpected{L"AWJ.com 路径超过 Windows API 长度限制。"};
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::expected<fs::path, std::wstring> awj_exe_path() {
  auto self = current_executable_path();
  if (!self) {
    return std::unexpected{self.error()};
  }
  auto exe = *self;
  exe.replace_extension(L".exe");
  std::error_code ec;
  if (!fs::is_regular_file(exe, ec) || ec) {
    return std::unexpected{std::format(L"找不到 AWJ.exe：{}。", exe.wstring())};
  }
  return exe;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  const auto exe = awj_exe_path();
  if (!exe) {
    std::wcerr << exe.error() << L'\n';
    return 1;
  }

  std::vector<std::wstring> owned_args;
  owned_args.reserve(static_cast<std::size_t>(std::max(argc, 1)));
  owned_args.push_back(exe->wstring());
  for (int i = 1; i < argc; ++i) {
    owned_args.emplace_back(argv[i]);
  }

  std::vector<wchar_t*> forwarded_args;
  forwarded_args.reserve(owned_args.size());
  for (auto& arg : owned_args) {
    forwarded_args.push_back(arg.data());
  }

  auto command_line = command_line_from_args(
      std::span<wchar_t* const>{forwarded_args.data(), forwarded_args.size()});
  if (command_line.size() >= 32767) {
    std::wcerr << L"AWJ 命令行超过 Windows 长度限制。\n";
    return 1;
  }

  auto stdout_pipe = create_forwarding_pipe(L"stdout");
  if (!stdout_pipe) {
    std::wcerr << stdout_pipe.error() << L'\n';
    return 1;
  }
  auto stderr_pipe = create_forwarding_pipe(L"stderr");
  if (!stderr_pipe) {
    close_if_valid(stdout_pipe->read);
    close_if_valid(stdout_pipe->write);
    std::wcerr << stderr_pipe.error() << L'\n';
    return 1;
  }

  SetConsoleCtrlHandler(console_control_handler, TRUE);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = stdout_pipe->write;
  startup.hStdError = stderr_pipe->write;

  PROCESS_INFORMATION process{};
  const auto cwd = exe->parent_path().wstring();
  const BOOL created = CreateProcessW(
      exe->c_str(), command_line.data(), nullptr, nullptr, TRUE, 0, nullptr,
      cwd.c_str(), &startup, &process);

  close_if_valid(stdout_pipe->write);
  stdout_pipe->write = nullptr;
  close_if_valid(stderr_pipe->write);
  stderr_pipe->write = nullptr;

  if (!created) {
    const auto error = GetLastError();
    close_if_valid(stdout_pipe->read);
    close_if_valid(stderr_pipe->read);
    std::wcerr << L"启动 AWJ.exe 失败：" << win32_error_message(error)
               << L'\n';
    return 1;
  }

  CloseHandle(process.hThread);
  g_child_process.store(process.hProcess, std::memory_order_release);

  std::thread stdout_thread{forward_pipe_to_handle, stdout_pipe->read,
                            GetStdHandle(STD_OUTPUT_HANDLE)};
  stdout_pipe->read = nullptr;
  std::thread stderr_thread{forward_pipe_to_handle, stderr_pipe->read,
                            GetStdHandle(STD_ERROR_HANDLE)};
  stderr_pipe->read = nullptr;

  const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
  g_child_process.store(nullptr, std::memory_order_release);

  if (stdout_thread.joinable()) {
    stdout_thread.join();
  }
  if (stderr_thread.joinable()) {
    stderr_thread.join();
  }

  if (wait != WAIT_OBJECT_0) {
    const auto error = GetLastError();
    CloseHandle(process.hProcess);
    std::wcerr << L"等待 AWJ.exe 结束失败：" << win32_error_message(error)
               << L'\n';
    return 1;
  }

  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
    const auto error = GetLastError();
    CloseHandle(process.hProcess);
    std::wcerr << L"读取 AWJ.exe 退出码失败：" << win32_error_message(error)
               << L'\n';
    return 1;
  }
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
}
