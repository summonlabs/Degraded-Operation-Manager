// Degraded Operation Manager - operating system single-writer exclusion.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The lock is owned by the kernel, not by a marker file: while one process
// holds it every other process is refused, and if the holder dies abruptly the
// kernel releases it. Nothing about ownership is inferred from the file's
// existence.

#include "dom/store.hpp"

#include <string>

#include "store/file_io.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace dom {

StoreLock::~StoreLock() { Release(); }

StoreLock::StoreLock(StoreLock&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

StoreLock& StoreLock::operator=(StoreLock&& other) noexcept {
  if (this != &other) {
    Release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool StoreLock::held() const noexcept { return handle_ != nullptr; }

void StoreLock::Release() {
  if (handle_ == nullptr) {
    return;
  }
#ifdef _WIN32
  HANDLE handle = static_cast<HANDLE>(handle_);
  OVERLAPPED overlapped{};
  UnlockFileEx(handle, 0, 1, 0, &overlapped);
  CloseHandle(handle);
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  ::flock(fd, LOCK_UN);
  ::close(fd);
#endif
  handle_ = nullptr;
}

Result<StoreLock> StoreLock::Acquire(const std::string& path) {
#ifdef _WIN32
  const std::wstring wide = internal::ToWidePath(path);
  if (wide.empty()) {
    return Result<StoreLock>::Err(ErrorCode::InvalidArgument, "lock path is not valid UTF-8");
  }
  // Sharing is refused outright, so a second process cannot even open the file.
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
      return Result<StoreLock>::Err(
          ErrorCode::Locked,
          "another process holds the single-writer store lock (Windows error " +
              std::to_string(error) + ")");
    }
    return Result<StoreLock>::Err(ErrorCode::IoError,
                                  "CreateFileW for the store lock failed with Windows error " +
                                      std::to_string(error));
  }
  OVERLAPPED overlapped{};
  if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                  &overlapped)) {
    const DWORD error = GetLastError();
    CloseHandle(handle);
    return Result<StoreLock>::Err(
        ErrorCode::Locked,
        "another process holds the store lock range (Windows error " + std::to_string(error) +
            ")");
  }
  StoreLock lock;
  lock.handle_ = handle;
  return Result<StoreLock>::Ok(std::move(lock));
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return Result<StoreLock>::Err(ErrorCode::IoError, "open of the store lock failed");
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return Result<StoreLock>::Err(ErrorCode::Locked,
                                  "another process holds the single-writer store lock");
  }
  StoreLock lock;
  lock.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  return Result<StoreLock>::Ok(std::move(lock));
#endif
}

}  // namespace dom
