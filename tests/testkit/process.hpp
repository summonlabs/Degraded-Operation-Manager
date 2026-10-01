// Degraded Operation Manager - real process and directory helpers.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// These helpers use the real operating system: real directories under the
// process temporary area, real child processes with real pipes, and real abrupt
// termination. Nothing here is simulated.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dom/dom.hpp"

namespace dom::test {

/// Owned temporary directory. Removal is recursive and happens on destruction.
class TempDir {
 public:
  TempDir() = default;
  static Result<TempDir> Create(const std::string& label);
  TempDir(TempDir&& other) noexcept;
  TempDir& operator=(TempDir&& other) noexcept;
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir();

  bool valid() const { return !path_.empty(); }
  const std::string& path() const { return path_; }
  void Remove();

 private:
  std::string path_;
};

/// Absolute path of the running test executable.
Result<std::string> ExecutablePath();
/// Directory containing the running test executable, where helper binaries live.
Result<std::string> ExecutableDirectory();
/// Joins a test store path under a temporary directory.
std::string Join(const std::string& directory, const std::string& name);

/// A real child process whose standard output is a pipe. Reading a line blocks
/// until the line, or reports end of stream, so a dead child can never hang the
/// test.
class ChildProcess {
 public:
  ChildProcess() = default;
  static Result<ChildProcess> Spawn(const std::string& executable,
                                    const std::vector<std::string>& arguments);
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess();

  bool valid() const { return process_ != nullptr; }
  /// Reads one line of standard output. Returns NotFound at end of stream.
  Result<std::string> ReadLine();
  Status WriteLine(const std::string& line);
  void CloseInput();
  /// Blocks until the child exits.
  Result<std::uint32_t> WaitForExit();
  /// Abrupt silent termination: no dialogs, no signal handling.
  void Terminate();
  bool HasExited();

 private:
  void Reset();
  void* process_ = nullptr;
  void* input_ = nullptr;
  void* output_ = nullptr;
  bool exited_ = false;
  std::uint32_t exit_code_ = 0;
  std::string pending_;
};

/// Recursively removes a directory tree. Used only by tests on their own
/// temporary directories.
void RemoveTree(const std::string& path);

}  // namespace dom::test
