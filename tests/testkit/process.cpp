// Degraded Operation Manager - real process and directory helpers.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "testkit/process.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace dom::test {
namespace {

std::uint64_t& Counter() {
  static std::uint64_t counter = 0;
  return counter;
}

#ifdef _WIN32

std::wstring Wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

/// Applies the extended-length prefix so that removal also works for paths
/// beyond MAX_PATH, which the long-path case deliberately creates.
std::wstring Extended(const std::wstring& absolute) {
  const bool is_absolute = absolute.size() >= 2 && absolute[1] == L':' &&
                           (absolute.size() == 2 || absolute[2] == L'\\' ||
                            absolute[2] == L'/');
  if (!is_absolute) {
    return absolute;
  }
  std::wstring normalized = absolute;
  for (wchar_t& c : normalized) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  std::wstring extended = L"\\\\?\\";
  extended += normalized;
  return extended;
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string narrow(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow.data(), size,
                      nullptr, nullptr);
  return narrow;
}

std::string QuoteArgument(const std::string& argument) {
  std::string quoted = "\"";
  for (char c : argument) {
    if (c == '"') {
      quoted += "\\\"";
    } else {
      quoted.push_back(c);
    }
  }
  quoted += "\"";
  return quoted;
}

#endif

}  // namespace

std::string Join(const std::string& directory, const std::string& name) {
  if (directory.empty()) {
    return name;
  }
  const char last = directory.back();
  if (last == '\\' || last == '/') {
    return directory + name;
  }
  return directory + "\\" + name;
}

TempDir::TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) {
  other.path_.clear();
}

TempDir& TempDir::operator=(TempDir&& other) noexcept {
  if (this != &other) {
    Remove();
    path_ = std::move(other.path_);
    other.path_.clear();
  }
  return *this;
}

TempDir::~TempDir() { Remove(); }

Result<TempDir> TempDir::Create(const std::string& label) {
#ifdef _WIN32
  wchar_t buffer[MAX_PATH + 1] = {};
  const DWORD length = GetTempPathW(MAX_PATH, buffer);
  if (length == 0) {
    return Result<TempDir>::Err(ErrorCode::IoError, "GetTempPathW failed");
  }
  const std::wstring base(buffer, length);
  const std::wstring name = L"dom-test-" + Wide(label) + L"-" +
                            std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) +
                            L"-" + std::to_wstring(++Counter());
  const std::wstring path = base + name;
  if (!CreateDirectoryW(path.c_str(), nullptr)) {
    return Result<TempDir>::Err(ErrorCode::IoError,
                                "CreateDirectoryW failed for the test directory");
  }
  TempDir directory;
  directory.path_ = Narrow(path);
  return Result<TempDir>::Ok(std::move(directory));
#else
  std::string pattern = "/tmp/dom-test-" + label + "-XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  char* created = ::mkdtemp(buffer.data());
  if (created == nullptr) {
    return Result<TempDir>::Err(ErrorCode::IoError, "mkdtemp failed");
  }
  TempDir directory;
  directory.path_ = created;
  return Result<TempDir>::Ok(std::move(directory));
#endif
}

void TempDir::Remove() {
  if (path_.empty()) {
    return;
  }
  RemoveTree(path_);
  path_.clear();
}

void RemoveTree(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = Extended(Wide(path));
  if (wide.empty()) {
    return;
  }
  const std::wstring pattern = wide + L"\\*";
  WIN32_FIND_DATAW data{};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
  if (handle != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = data.cFileName;
      if (name == L"." || name == L"..") {
        continue;
      }
      const std::wstring child = wide + L"\\" + name;
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        RemoveTree(Narrow(child));
      } else {
        SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(child.c_str());
      }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
  }
  RemoveDirectoryW(wide.c_str());
#else
  std::string command = "rm -rf '" + path + "'";
  const int ignored = std::system(command.c_str());
  (void)ignored;
#endif
}

Result<std::string> ExecutablePath() {
#ifdef _WIN32
  // Allocated on the heap: a 64 KB stack buffer is a real limitation.
  std::vector<wchar_t> buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return Result<std::string>::Err(ErrorCode::IoError, "GetModuleFileNameW failed");
  }
  return Result<std::string>::Ok(Narrow(std::wstring(buffer.data(), length)));
#else
  return Result<std::string>::Err(ErrorCode::Unsupported, "not implemented on this platform");
#endif
}

Result<std::string> ExecutableDirectory() {
  auto path = ExecutablePath();
  if (!path) {
    return path;
  }
  const std::size_t separator = path.value().find_last_of("\\/");
  if (separator == std::string::npos) {
    return Result<std::string>::Err(ErrorCode::Internal, "executable path has no directory");
  }
  return Result<std::string>::Ok(path.value().substr(0, separator));
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : process_(other.process_),
      input_(other.input_),
      output_(other.output_),
      exited_(other.exited_),
      exit_code_(other.exit_code_),
      pending_(std::move(other.pending_)) {
  other.process_ = nullptr;
  other.input_ = nullptr;
  other.output_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    Reset();
    process_ = other.process_;
    input_ = other.input_;
    output_ = other.output_;
    exited_ = other.exited_;
    exit_code_ = other.exit_code_;
    pending_ = std::move(other.pending_);
    other.process_ = nullptr;
    other.input_ = nullptr;
    other.output_ = nullptr;
  }
  return *this;
}

ChildProcess::~ChildProcess() { Reset(); }

void ChildProcess::Reset() {
  if (input_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(input_));
    input_ = nullptr;
  }
  if (output_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(output_));
    output_ = nullptr;
  }
  if (process_ != nullptr) {
    HANDLE process = static_cast<HANDLE>(process_);
    if (!exited_) {
      if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        exit_code_ = code;
        exited_ = true;
      } else {
        TerminateProcess(process, 0xC000013A);
        WaitForSingleObject(process, INFINITE);
      }
    }
    CloseHandle(process);
    process_ = nullptr;
  }
}

Result<ChildProcess> ChildProcess::Spawn(const std::string& executable,
                                         const std::vector<std::string>& arguments) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  attributes.bInheritHandle = TRUE;

  HANDLE child_stdout_read = nullptr;
  HANDLE child_stdout_write = nullptr;
  if (!CreatePipe(&child_stdout_read, &child_stdout_write, &attributes, 0)) {
    return Result<ChildProcess>::Err(ErrorCode::IoError, "CreatePipe for stdout failed");
  }
  SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);
  HANDLE child_stdin_read = nullptr;
  HANDLE child_stdin_write = nullptr;
  if (!CreatePipe(&child_stdin_read, &child_stdin_write, &attributes, 0)) {
    CloseHandle(child_stdout_read);
    CloseHandle(child_stdout_write);
    return Result<ChildProcess>::Err(ErrorCode::IoError, "CreatePipe for stdin failed");
  }
  SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);

  std::string command = QuoteArgument(executable);
  for (const std::string& argument : arguments) {
    command += " ";
    command += QuoteArgument(argument);
  }
  std::wstring command_line = Wide(command);

  STARTUPINFOW startup{};
  startup.cb = sizeof(STARTUPINFOW);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = child_stdout_write;
  startup.hStdError = child_stdout_write;
  startup.hStdInput = child_stdin_read;
  PROCESS_INFORMATION information{};
  const BOOL started = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
  CloseHandle(child_stdout_write);
  CloseHandle(child_stdin_read);
  if (!started) {
    CloseHandle(child_stdout_read);
    CloseHandle(child_stdin_write);
    return Result<ChildProcess>::Err(ErrorCode::IoError,
                                     "CreateProcessW failed for " + executable);
  }
  CloseHandle(information.hThread);
  ChildProcess child;
  child.process_ = information.hProcess;
  child.input_ = child_stdin_write;
  child.output_ = child_stdout_read;
  return Result<ChildProcess>::Ok(std::move(child));
#else
  (void)executable;
  (void)arguments;
  return Result<ChildProcess>::Err(ErrorCode::Unsupported, "not implemented on this platform");
#endif
}

Result<std::string> ChildProcess::ReadLine() {
  if (output_ == nullptr) {
    return Result<std::string>::Err(ErrorCode::NotFound, "the child has no output pipe");
  }
  HANDLE pipe = static_cast<HANDLE>(output_);
  for (;;) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return Result<std::string>::Ok(std::move(line));
    }
    std::vector<char> buffer(4096);
    DWORD read = 0;
    if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ||
        read == 0) {
      if (!pending_.empty()) {
        std::string line = pending_;
        pending_.clear();
        return Result<std::string>::Ok(std::move(line));
      }
      return Result<std::string>::Err(ErrorCode::NotFound, "end of child output");
    }
    pending_.append(buffer.data(), read);
  }
}

Status ChildProcess::WriteLine(const std::string& line) {
  if (input_ == nullptr) {
    return Status::Error(ErrorCode::NotFound, "the child has no input pipe");
  }
  const std::string text = line + "\n";
  DWORD written = 0;
  if (!WriteFile(static_cast<HANDLE>(input_), text.data(), static_cast<DWORD>(text.size()),
                 &written, nullptr)) {
    return Status::Error(ErrorCode::IoError, "WriteFile to the child failed");
  }
  if (written != text.size()) {
    return Status::Error(ErrorCode::IoError, "short write to the child");
  }
  return Status::Ok();
}

void ChildProcess::CloseInput() {
  if (input_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(input_));
    input_ = nullptr;
  }
}

Result<std::uint32_t> ChildProcess::WaitForExit() {
  if (process_ == nullptr) {
    return Result<std::uint32_t>::Err(ErrorCode::NotFound, "no child process");
  }
  HANDLE process = static_cast<HANDLE>(process_);
  if (!exited_) {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    exit_code_ = code;
    exited_ = true;
  }
  return Result<std::uint32_t>::Ok(exit_code_);
}

void ChildProcess::Terminate() {
  if (process_ == nullptr || exited_) {
    return;
  }
  HANDLE process = static_cast<HANDLE>(process_);
  // Silent, immediate termination: no crash dialog, no debugger prompt.
  TerminateProcess(process, 0xC000013A);
  WaitForSingleObject(process, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process, &code);
  exit_code_ = code;
  exited_ = true;
}

bool ChildProcess::HasExited() {
  if (process_ == nullptr) {
    return true;
  }
  if (exited_) {
    return true;
  }
  HANDLE process = static_cast<HANDLE>(process_);
  if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    exit_code_ = code;
    exited_ = true;
    return true;
  }
  return false;
}

}  // namespace dom::test
