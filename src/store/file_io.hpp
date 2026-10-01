// Degraded Operation Manager - file system primitives (private header).
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real operating system primitives: create/truncate, append, flush to stable
// storage, read back, atomic replace and delete. On Windows the Win32 API is
// used directly so that flushing semantics are explicit; long absolute paths
// are opened through the extended-length prefix.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dom/ids.hpp"
#include "dom/status.hpp"

namespace dom::internal {

/// Reads a whole file. Fails when the file does not exist or exceeds the bound.
Result<std::vector<std::uint8_t>> ReadFileBytes(const std::string& path);

/// Creates or truncates a file, writes all bytes and flushes them to stable
/// storage before returning.
Status WriteFileFlushed(const std::string& path, std::span<const std::uint8_t> bytes);

/// Opens (creating when needed), appends all bytes and flushes them.
Status AppendFileFlushed(const std::string& path, std::span<const std::uint8_t> bytes);

/// Atomically replaces target with the already flushed staged file.
Status AtomicReplace(const std::string& staged, const std::string& target);

Status RemoveFile(const std::string& path);
Result<bool> FileExists(const std::string& path);
Result<bool> DirectoryExists(const std::string& path);
Result<std::uint64_t> FileSize(const std::string& path);
/// Cuts a file back to a length, flushing the result. Used to drop a torn
/// journal tail before appending again.
Status TruncateFile(const std::string& path, std::uint64_t length);
Status EnsureDirectory(const std::string& path);
Result<std::vector<std::string>> ListDirectory(const std::string& path);

std::string JoinPath(const std::string& directory, const std::string& name);

#ifdef _WIN32
/// Converts a UTF-8 path to UTF-16 with the extended-length prefix applied to
/// absolute paths. Returns an empty string when the input is not valid UTF-8.
std::wstring ToWidePath(const std::string& path);
#endif

/// Snapshot file naming: snapshot-<20 digit sequence>.doms
std::string SnapshotFileName(StateSequence sequence);
/// Returns true and fills sequence when name matches the snapshot pattern.
bool ParseSnapshotFileName(const std::string& name, StateSequence& sequence);
inline constexpr const char* kSnapshotSuffix = ".doms";
inline constexpr const char* kStagedSuffix = ".doms.tmp";
inline constexpr const char* kJournalFileName = "journal.domj";
inline constexpr const char* kLockFileName = "store.lock";

}  // namespace dom::internal
