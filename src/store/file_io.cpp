// Degraded Operation Manager - file system primitives.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "store/file_io.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

#include "dom/limits.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace dom::internal {

#ifdef _WIN32

/// Converts a UTF-8 path to UTF-16, applying the extended-length prefix so that
/// paths beyond MAX_PATH are usable.
std::wstring ToWidePath(const std::string& path) {
  if (path.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(),
                                       static_cast<int>(path.size()), nullptr, 0);
  if (size <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(),
                      static_cast<int>(path.size()), wide.data(), size);
  const bool absolute = wide.size() >= 2 && wide[1] == L':' &&
                        (wide.size() == 2 || wide[2] == L'\\' || wide[2] == L'/');
  if (!absolute) {
    return wide;
  }
  for (wchar_t& c : wide) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  std::wstring extended = L"\\\\?\\";
  extended += wide;
  return extended;
}

namespace {

std::string LastErrorMessage(const char* what) {
  return std::string(what) + " failed with Windows error " +
         std::to_string(static_cast<unsigned long>(GetLastError()));
}

Status WriteAll(HANDLE handle, std::span<const std::uint8_t> bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (std::min)(static_cast<std::size_t>(1u << 20), bytes.size() - written));
    DWORD done = 0;
    if (!WriteFile(handle, bytes.data() + written, chunk, &done, nullptr)) {
      return Status::Error(ErrorCode::IoError, LastErrorMessage("WriteFile"));
    }
    if (done == 0) {
      return Status::Error(ErrorCode::IoError, "WriteFile wrote zero bytes");
    }
    written += done;
  }
  return Status::Ok();
}

#else

std::string LastErrorMessage(const char* what) {
  return std::string(what) + " failed with errno " + std::to_string(errno);
}

#endif

}  // namespace

std::string JoinPath(const std::string& directory, const std::string& name) {
  if (directory.empty()) {
    return name;
  }
  const char last = directory.back();
  if (last == '\\' || last == '/') {
    return directory + name;
  }
  return directory + "\\" + name;
}

std::string SnapshotFileName(StateSequence sequence) {
  std::string digits = sequence.ToString();
  std::string padded(20 - (std::min)(static_cast<std::size_t>(20), digits.size()), '0');
  return "snapshot-" + padded + digits + kSnapshotSuffix;
}

bool ParseSnapshotFileName(const std::string& name, StateSequence& sequence) {
  const std::string prefix = "snapshot-";
  const std::string suffix = kSnapshotSuffix;
  if (name.size() <= prefix.size() + suffix.size()) {
    return false;
  }
  if (name.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  if (digits.empty() || digits.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return false;
    }
    value = value * 10u + digit;
  }
  sequence = StateSequence::FromValue(value);
  return true;
}

Result<std::vector<std::uint8_t>> ReadFileBytes(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = ToWidePath(path);
  if (wide.empty()) {
    return Result<std::vector<std::uint8_t>>::Err(ErrorCode::InvalidArgument,
                                                  "path is not valid UTF-8");
  }
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    const ErrorCode code = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                               ? ErrorCode::NotFound
                               : ErrorCode::IoError;
    return Result<std::vector<std::uint8_t>>::Err(
        code, "CreateFileW for read failed with Windows error " + std::to_string(error));
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle, &size)) {
    CloseHandle(handle);
    return Result<std::vector<std::uint8_t>>::Err(ErrorCode::IoError, LastErrorMessage("GetFileSizeEx"));
  }
  if (size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > limits::kMaxEnvelopeBytes) {
    CloseHandle(handle);
    return Result<std::vector<std::uint8_t>>::Err(ErrorCode::LimitExceeded,
                                                  "file exceeds the supported byte bound");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t read = 0;
  while (read < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (std::min)(static_cast<std::size_t>(1u << 20), bytes.size() - read));
    DWORD done = 0;
    if (!ReadFile(handle, bytes.data() + read, chunk, &done, nullptr)) {
      CloseHandle(handle);
      return Result<std::vector<std::uint8_t>>::Err(ErrorCode::IoError, LastErrorMessage("ReadFile"));
    }
    if (done == 0) {
      CloseHandle(handle);
      return Result<std::vector<std::uint8_t>>::Err(ErrorCode::Truncated,
                                                    "file ended before its recorded length");
    }
    read += done;
  }
  CloseHandle(handle);
  return Result<std::vector<std::uint8_t>>::Ok(std::move(bytes));
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Result<std::vector<std::uint8_t>>::Err(ErrorCode::NotFound, LastErrorMessage("open"));
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t buffer[65536];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0) {
      ::close(fd);
      return Result<std::vector<std::uint8_t>>::Err(ErrorCode::IoError, LastErrorMessage("read"));
    }
    if (got == 0) {
      break;
    }
    bytes.insert(bytes.end(), buffer, buffer + got);
    if (bytes.size() > limits::kMaxEnvelopeBytes) {
      ::close(fd);
      return Result<std::vector<std::uint8_t>>::Err(ErrorCode::LimitExceeded,
                                                    "file exceeds the supported byte bound");
    }
  }
  ::close(fd);
  return Result<std::vector<std::uint8_t>>::Ok(std::move(bytes));
#endif
}

Status WriteFileFlushed(const std::string& path, std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
  const std::wstring wide = ToWidePath(path);
  if (wide.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "path is not valid UTF-8");
  }
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("CreateFileW for write"));
  }
  Status status = WriteAll(handle, bytes);
  if (status.ok()) {
    if (!FlushFileBuffers(handle)) {
      status = Status::Error(ErrorCode::IoError, LastErrorMessage("FlushFileBuffers"));
    }
  }
  CloseHandle(handle);
  return status;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("open"));
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t done = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (done <= 0) {
      ::close(fd);
      return Status::Error(ErrorCode::IoError, LastErrorMessage("write"));
    }
    written += static_cast<std::size_t>(done);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return Status::Error(ErrorCode::IoError, LastErrorMessage("fsync"));
  }
  ::close(fd);
  return Status::Ok();
#endif
}

Status AppendFileFlushed(const std::string& path, std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
  const std::wstring wide = ToWidePath(path);
  if (wide.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "path is not valid UTF-8");
  }
  HANDLE handle = CreateFileW(wide.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("CreateFileW for append"));
  }
  SetFilePointer(handle, 0, nullptr, FILE_END);
  Status status = WriteAll(handle, bytes);
  if (status.ok()) {
    if (!FlushFileBuffers(handle)) {
      status = Status::Error(ErrorCode::IoError, LastErrorMessage("FlushFileBuffers"));
    }
  }
  CloseHandle(handle);
  return status;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("open"));
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t done = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (done <= 0) {
      ::close(fd);
      return Status::Error(ErrorCode::IoError, LastErrorMessage("write"));
    }
    written += static_cast<std::size_t>(done);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return Status::Error(ErrorCode::IoError, LastErrorMessage("fsync"));
  }
  ::close(fd);
  return Status::Ok();
#endif
}

Status AtomicReplace(const std::string& staged, const std::string& target) {
#ifdef _WIN32
  const std::wstring wide_staged = ToWidePath(staged);
  const std::wstring wide_target = ToWidePath(target);
  if (wide_staged.empty() || wide_target.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "path is not valid UTF-8");
  }
  if (!MoveFileExW(wide_staged.c_str(), wide_target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("MoveFileExW"));
  }
  return Status::Ok();
#else
  if (::rename(staged.c_str(), target.c_str()) != 0) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("rename"));
  }
  return Status::Ok();
#endif
}

Status RemoveFile(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = ToWidePath(path);
  if (wide.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "path is not valid UTF-8");
  }
  if (DeleteFileW(wide.c_str())) {
    return Status::Ok();
  }
  const DWORD error = GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    return Status::Ok();
  }
  return Status::Error(ErrorCode::IoError,
                       "DeleteFileW failed with Windows error " + std::to_string(error));
#else
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("unlink"));
  }
  return Status::Ok();
#endif
}

Result<bool> FileExists(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = ToWidePath(path);
  if (wide.empty()) {
    return Result<bool>::Err(ErrorCode::InvalidArgument, "path is not valid UTF-8");
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return Result<bool>::Ok(false);
    }
    return Result<bool>::Err(ErrorCode::IoError, LastErrorMessage("GetFileAttributesW"));
  }
  return Result<bool>::Ok((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
#else
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return Result<bool>::Ok(false);
  }
  return Result<bool>::Ok(S_ISREG(info.st_mode));
#endif
}

Result<bool> DirectoryExists(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = ToWidePath(path);
  if (wide.empty()) {
    return Result<bool>::Err(ErrorCode::InvalidArgument, "path is not valid UTF-8");
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return Result<bool>::Ok(false);
    }
    return Result<bool>::Err(ErrorCode::IoError, LastErrorMessage("GetFileAttributesW"));
  }
  return Result<bool>::Ok((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
#else
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return Result<bool>::Ok(false);
  }
  return Result<bool>::Ok(S_ISDIR(info.st_mode));
#endif
}

Status TruncateFile(const std::string& path, std::uint64_t length) {
#ifdef _WIN32
  const std::wstring wide = ToWidePath(path);
  if (wide.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "path is not valid UTF-8");
  }
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("CreateFileW for truncate"));
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(length);
  Status status = Status::Ok();
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    status = Status::Error(ErrorCode::IoError, LastErrorMessage("SetFilePointerEx"));
  } else if (!SetEndOfFile(handle)) {
    status = Status::Error(ErrorCode::IoError, LastErrorMessage("SetEndOfFile"));
  } else if (!FlushFileBuffers(handle)) {
    status = Status::Error(ErrorCode::IoError, LastErrorMessage("FlushFileBuffers"));
  }
  CloseHandle(handle);
  return status;
#else
  if (::truncate(path.c_str(), static_cast<off_t>(length)) != 0) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("truncate"));
  }
  return Status::Ok();
#endif
}

Result<std::uint64_t> FileSize(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = ToWidePath(path);
  if (wide.empty()) {
    return Result<std::uint64_t>::Err(ErrorCode::InvalidArgument, "path is not valid UTF-8");
  }
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
    return Result<std::uint64_t>::Err(ErrorCode::NotFound, LastErrorMessage("GetFileAttributesExW"));
  }
  const std::uint64_t size =
      (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  return Result<std::uint64_t>::Ok(size);
#else
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return Result<std::uint64_t>::Err(ErrorCode::NotFound, LastErrorMessage("stat"));
  }
  return Result<std::uint64_t>::Ok(static_cast<std::uint64_t>(info.st_size));
#endif
}

Status EnsureDirectory(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = ToWidePath(path);
  if (wide.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "path is not valid UTF-8");
  }
  if (CreateDirectoryW(wide.c_str(), nullptr)) {
    return Status::Ok();
  }
  const DWORD error = GetLastError();
  if (error == ERROR_ALREADY_EXISTS) {
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return Status::Ok();
    }
    return Status::Error(ErrorCode::AlreadyExists,
                         "the store path exists and is not a directory");
  }
  return Status::Error(ErrorCode::IoError,
                       "CreateDirectoryW failed with Windows error " + std::to_string(error));
#else
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return Status::Error(ErrorCode::IoError, LastErrorMessage("mkdir"));
  }
  return Status::Ok();
#endif
}

Result<std::vector<std::string>> ListDirectory(const std::string& path) {
  std::vector<std::string> names;
#ifdef _WIN32
  const std::wstring wide = ToWidePath(JoinPath(path, "*"));
  if (wide.empty()) {
    return Result<std::vector<std::string>>::Err(ErrorCode::InvalidArgument,
                                                 "path is not valid UTF-8");
  }
  WIN32_FIND_DATAW data{};
  HANDLE handle = FindFirstFileW(wide.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return Result<std::vector<std::string>>::Ok(std::move(names));
    }
    return Result<std::vector<std::string>>::Err(ErrorCode::IoError,
                                                 LastErrorMessage("FindFirstFileW"));
  }
  do {
    const int size = WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
      std::string name(static_cast<std::size_t>(size - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name.data(), size, nullptr, nullptr);
      names.push_back(std::move(name));
    }
  } while (FindNextFileW(handle, &data));
  FindClose(handle);
#else
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    return Result<std::vector<std::string>>::Err(ErrorCode::NotFound, LastErrorMessage("opendir"));
  }
  while (dirent* entry = ::readdir(directory)) {
    names.push_back(entry->d_name);
  }
  ::closedir(directory);
#endif
  std::sort(names.begin(), names.end());
  return Result<std::vector<std::string>>::Ok(std::move(names));
}

}  // namespace dom::internal
