// Degraded Operation Manager - durable single-writer state store.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dom/authority.hpp"
#include "dom/decision.hpp"
#include "dom/status.hpp"

namespace dom {

/// The whole authoritative state of one facility mode authority. One value is
/// one committed generation; it is published atomically or not at all.
struct PersistedState {
  std::uint16_t format_version = 0;
  StateSequence sequence;
  /// Digest of the previously published generation, chaining generations so a
  /// rollback to an older file is detectable while that file is retained.
  Digest previous_digest;
  CommittedView committed;
  ModeDecision last_decision;
  std::vector<HistoryEntry> history;
  std::vector<RestrictionAuthorization> authorizations;
  std::vector<AcknowledgementRecord> acknowledgements;
  std::vector<EffectVerificationRecord> verifications;
  std::vector<IdempotencyEntry> idempotency;
  std::uint64_t committed_operations = 0;
  std::uint64_t evaluations = 0;

  /// Canonical digest of everything except the framing header. This is the
  /// integrity value written into the file and inherited as previous_digest.
  Digest digest() const;
};

/// Points at which a commit publishes durable state. Exposed for instrumentation
/// and for crash-injection harnesses that terminate a process at a real
/// publication boundary; the observer runs on the committing thread.
enum class CommitStage : std::uint8_t {
  StagedWritten = 0,
  StagedVerified = 1,
  IntentJournaled = 2,
  Published = 3,
  CommitJournaled = 4,
};

const char* CommitStageName(CommitStage stage) noexcept;

/// How a store was recovered when it was opened.
enum class RecoveryOutcome : std::uint8_t {
  /// No prior generation existed; a fresh store was created.
  Fresh = 0,
  /// The newest published generation was loaded unchanged.
  Loaded = 1,
  /// Unpublished staged files or journal intents were discarded.
  DiscardedUnpublished = 2,
  /// A commit marker was missing and was repaired from the published file.
  RepairedCommitMarker = 3,
};

const char* RecoveryOutcomeName(RecoveryOutcome outcome) noexcept;

struct RecoveryReport {
  RecoveryOutcome outcome = RecoveryOutcome::Fresh;
  StateSequence loaded_sequence;
  std::uint32_t discarded_unpublished = 0;
  std::uint32_t repaired_markers = 0;
  std::uint32_t stray_files_removed = 0;
  std::string detail;
};

/// Real OS-level single-writer exclusion over a lock file. The kernel owns the
/// lock: an abrupt holder death releases it, and a second process is refused
/// while the holder lives.
class StoreLock {
 public:
  StoreLock() = default;
  ~StoreLock();

  StoreLock(StoreLock&& other) noexcept;
  StoreLock& operator=(StoreLock&& other) noexcept;
  StoreLock(const StoreLock&) = delete;
  StoreLock& operator=(const StoreLock&) = delete;

  static Result<StoreLock> Acquire(const std::string& path);

  bool held() const noexcept;
  void Release();

 private:
  void* handle_ = nullptr;  // platform handle, never dereferenced here
};

struct StoreOptions {
  std::string directory;
  bool create_if_missing = true;
  std::uint32_t history_capacity = 4096;
  std::uint32_t record_capacity = 4096;
  std::uint32_t idempotency_capacity = 1024;
  /// Optional commit-stage observer. It is called synchronously from Commit with
  /// no store lock held beyond the process-lifetime file lock. It must be a pure
  /// instrument: re-entering the store from the observer is refused.
  std::function<void(CommitStage)> commit_observer;
};

struct CommitOptions {
  /// Sequence the caller believes is current. A mismatch means another
  /// generation was published in between and the commit is refused rather than
  /// silently applied on top.
  StateSequence expected_previous;
};

/// Durable state store. Not internally synchronised: the coordinator serialises
/// every call. One store holds exactly one OS lock for its lifetime.
class StateStore {
 public:
  ~StateStore();
  StateStore(const StateStore&) = delete;
  StateStore& operator=(const StateStore&) = delete;

  static Result<std::unique_ptr<StateStore>> Open(const StoreOptions& options);

  const PersistedState& state() const noexcept { return state_; }
  const RecoveryReport& recovery() const noexcept { return recovery_; }
  const std::string& directory() const noexcept { return directory_; }

  /// Publish a new generation. The store assigns sequence and previous_digest,
  /// stages, flushes, reads back, verifies, journals the intent, atomically
  /// publishes and journals the commit. Any failure leaves the previously
  /// published generation authoritative.
  Result<StateSequence> Commit(PersistedState next, const CommitOptions& options);

  /// Re-read and verify the published generation from disk without changing the
  /// in-memory state. Used by validation paths and tests.
  Result<PersistedState> VerifyOnDisk() const;

  /// Publication statistics since open (real durable operations, not
  /// submissions): completed commits and committed bytes.
  std::uint64_t committed_generations() const noexcept { return committed_generations_; }
  std::uint64_t committed_bytes() const noexcept { return committed_bytes_; }

 private:
  StateStore() = default;

  std::string directory_;
  StoreLock lock_;
  PersistedState state_;
  RecoveryReport recovery_;
  std::uint32_t history_capacity_ = 4096;
  std::uint32_t record_capacity_ = 4096;
  std::uint32_t idempotency_capacity_ = 1024;
  std::function<void(CommitStage)> commit_observer_;
  bool committing_ = false;
  std::uint64_t committed_generations_ = 0;
  std::uint64_t committed_bytes_ = 0;
};

}  // namespace dom
