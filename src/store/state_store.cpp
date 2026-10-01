// Degraded Operation Manager - durable state store.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Publication protocol for one committed generation:
//   1. write the staged snapshot and flush it to stable storage;
//   2. read the staged file back and verify header, length and digests;
//   3. append the journal intent and flush;
//   4. atomically replace the published snapshot with the staged file;
//      <- this rename is the commit point
//   5. append the journal commit marker and flush (repair aid, best effort).
// Recovery loads the newest snapshot that fully verifies, refuses any snapshot
// file that exists but does not verify, discards intents that were never
// published, and repairs a missing commit marker. It never applies part of a
// generation, and it never silently falls back to an older generation.

#include "dom/store.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dom/limits.hpp"
#include "dom/version.hpp"
#include "store/file_io.hpp"
#include "store/journal.hpp"
#include "store/snapshot.hpp"

namespace dom {
namespace {

constexpr std::size_t kRetainedSnapshots = 2;

std::string StagedSnapshotName(StateSequence sequence) {
  return internal::SnapshotFileName(sequence) + ".tmp";
}

bool EndsWith(const std::string& text, const char* suffix) {
  const std::size_t length = std::strlen(suffix);
  if (text.size() < length) {
    return false;
  }
  return text.compare(text.size() - length, length, suffix) == 0;
}

}  // namespace

const char* CommitStageName(CommitStage stage) noexcept {
  switch (stage) {
    case CommitStage::StagedWritten:
      return "staged-written";
    case CommitStage::StagedVerified:
      return "staged-verified";
    case CommitStage::IntentJournaled:
      return "intent-journaled";
    case CommitStage::Published:
      return "published";
    case CommitStage::CommitJournaled:
      return "commit-journaled";
  }
  return "unknown";
}

const char* RecoveryOutcomeName(RecoveryOutcome outcome) noexcept {
  switch (outcome) {
    case RecoveryOutcome::Fresh:
      return "fresh";
    case RecoveryOutcome::Loaded:
      return "loaded";
    case RecoveryOutcome::DiscardedUnpublished:
      return "discarded-unpublished";
    case RecoveryOutcome::RepairedCommitMarker:
      return "repaired-commit-marker";
  }
  return "unknown";
}

StateStore::~StateStore() = default;

Result<std::unique_ptr<StateStore>> StateStore::Open(const StoreOptions& options) {
  if (options.directory.empty()) {
    return Result<std::unique_ptr<StateStore>>::Err(ErrorCode::InvalidArgument,
                                                    "store directory must not be empty");
  }
  if (options.history_capacity == 0 ||
      options.history_capacity > limits::kMaxHistoryEntries) {
    return Result<std::unique_ptr<StateStore>>::Err(
        ErrorCode::InvalidArgument, "history capacity must be between 1 and the supported bound");
  }
  if (options.record_capacity == 0 || options.record_capacity > limits::kMaxAuthorityRecords) {
    return Result<std::unique_ptr<StateStore>>::Err(
        ErrorCode::InvalidArgument, "record capacity must be between 1 and the supported bound");
  }
  if (options.idempotency_capacity == 0 ||
      options.idempotency_capacity > limits::kMaxIdempotencyEntries) {
    return Result<std::unique_ptr<StateStore>>::Err(
        ErrorCode::InvalidArgument,
        "idempotency capacity must be between 1 and the supported bound");
  }

  const Result<bool> directory_exists = internal::DirectoryExists(options.directory);
  if (!directory_exists) {
    return Result<std::unique_ptr<StateStore>>::Err(directory_exists.status());
  }
  if (!directory_exists.value()) {
    if (!options.create_if_missing) {
      return Result<std::unique_ptr<StateStore>>::Err(
          ErrorCode::NotFound, "store directory does not exist and creation was not requested");
    }
    const Status created = internal::EnsureDirectory(options.directory);
    if (!created.ok()) {
      return Result<std::unique_ptr<StateStore>>::Err(created);
    }
  }

  const std::string lock_path =
      internal::JoinPath(options.directory, internal::kLockFileName);
  auto lock = StoreLock::Acquire(lock_path);
  if (!lock) {
    return Result<std::unique_ptr<StateStore>>::Err(lock.status());
  }

  std::unique_ptr<StateStore> store(new StateStore());
  store->directory_ = options.directory;
  store->lock_ = std::move(lock.value());
  store->history_capacity_ = options.history_capacity;
  store->record_capacity_ = options.record_capacity;
  store->idempotency_capacity_ = options.idempotency_capacity;
  store->commit_observer_ = options.commit_observer;

  auto names = internal::ListDirectory(options.directory);
  if (!names) {
    return Result<std::unique_ptr<StateStore>>::Err(names.status());
  }
  std::vector<StateSequence> snapshots;
  std::uint32_t stray_files = 0;
  for (const std::string& name : names.value()) {
    StateSequence sequence;
    if (internal::ParseSnapshotFileName(name, sequence) && !EndsWith(name, ".tmp")) {
      snapshots.push_back(sequence);
      continue;
    }
    if (EndsWith(name, internal::kStagedSuffix)) {
      // A staged file is never authoritative: it is removed and reported.
      const Status removed = internal::RemoveFile(internal::JoinPath(options.directory, name));
      if (removed.ok()) {
        ++stray_files;
      }
      continue;
    }
  }
  std::sort(snapshots.begin(), snapshots.end(),
            [](StateSequence a, StateSequence b) { return b < a; });

  PersistedState state;
  state.format_version = kStoreFormatVersion;
  RecoveryReport report;

  if (snapshots.empty()) {
    report.outcome = RecoveryOutcome::Fresh;
    report.detail = "no published generation exists yet";
  } else {
    const std::string path =
        internal::JoinPath(options.directory, internal::SnapshotFileName(snapshots.front()));
    auto bytes = internal::ReadFileBytes(path);
    if (!bytes) {
      return Result<std::unique_ptr<StateStore>>::Err(bytes.status());
    }
    StateSequence file_sequence;
    auto decoded = internal::DecodeSnapshotFile(bytes.value(), file_sequence);
    if (!decoded) {
      return Result<std::unique_ptr<StateStore>>::Err(decoded.status());
    }
    state = std::move(decoded.value());
    report.outcome = RecoveryOutcome::Loaded;
    report.detail = "loaded published generation " + state.sequence.ToString();

    if (snapshots.size() >= 2 && snapshots[0].value() == snapshots[1].value() + 1) {
      const std::string previous_path = internal::JoinPath(
          options.directory, internal::SnapshotFileName(snapshots[1]));
      auto previous_bytes = internal::ReadFileBytes(previous_path);
      if (!previous_bytes) {
        return Result<std::unique_ptr<StateStore>>::Err(previous_bytes.status());
      }
      StateSequence previous_sequence;
      auto previous_state =
          internal::DecodeSnapshotFile(previous_bytes.value(), previous_sequence);
      if (!previous_state) {
        return Result<std::unique_ptr<StateStore>>::Err(previous_state.status());
      }
      if (previous_state.value().digest() != state.previous_digest) {
        return Result<std::unique_ptr<StateStore>>::Err(
            ErrorCode::Corrupt,
            "the retained previous generation does not match the published chain digest");
      }
      report.detail += "; generation chain verified against " +
                       previous_state.value().sequence.ToString();
    } else {
      report.detail += "; previous generation is not retained, chain check skipped";
    }
  }

  const std::string journal_path =
      internal::JoinPath(options.directory, internal::kJournalFileName);
  const Result<bool> journal_exists = internal::FileExists(journal_path);
  if (!journal_exists) {
    return Result<std::unique_ptr<StateStore>>::Err(journal_exists.status());
  }
  if (journal_exists.value()) {
    auto journal_bytes = internal::ReadFileBytes(journal_path);
    if (!journal_bytes) {
      return Result<std::unique_ptr<StateStore>>::Err(journal_bytes.status());
    }
    auto scan = internal::ScanJournal(journal_bytes.value());
    if (!scan) {
      return Result<std::unique_ptr<StateStore>>::Err(scan.status());
    }
    if (scan.value().torn_tail) {
      const std::uint64_t keep =
          (std::min)(static_cast<std::uint64_t>(scan.value().bytes_consumed),
                     static_cast<std::uint64_t>(journal_bytes.value().size()));
      const Status truncated = internal::TruncateFile(journal_path, keep);
      if (!truncated.ok()) {
        return Result<std::unique_ptr<StateStore>>::Err(truncated);
      }
      report.detail += "; journal tail was torn and has been discarded";
    }
    bool committed_for_loaded = false;
    for (const internal::JournalRecord& record : scan.value().records) {
      if (record.kind == internal::JournalRecordKind::Committed &&
          record.sequence > state.sequence) {
        return Result<std::unique_ptr<StateStore>>::Err(
            ErrorCode::Corrupt,
            "the journal records a committed generation whose snapshot is missing");
      }
      if (record.kind == internal::JournalRecordKind::Committed &&
          record.sequence == state.sequence) {
        committed_for_loaded = true;
      }
    }
    for (const internal::JournalRecord& record : scan.value().records) {
      if (record.kind != internal::JournalRecordKind::Intent) {
        continue;
      }
      if (record.sequence > state.sequence) {
        internal::JournalRecord abandoned;
        abandoned.kind = internal::JournalRecordKind::Abandoned;
        abandoned.sequence = record.sequence;
        abandoned.digest = record.digest;
        abandoned.detail = "publish was never observed";
        const Status appended = internal::AppendJournalRecord(journal_path, abandoned);
        if (!appended.ok()) {
          return Result<std::unique_ptr<StateStore>>::Err(appended);
        }
        ++report.discarded_unpublished;
      }
    }
    if (state.sequence.value() != 0 && !committed_for_loaded) {
      internal::JournalRecord marker;
      marker.kind = internal::JournalRecordKind::Committed;
      marker.sequence = state.sequence;
      marker.digest = state.digest();
      marker.detail = "commit marker repaired during recovery";
      const Status appended = internal::AppendJournalRecord(journal_path, marker);
      if (!appended.ok()) {
        return Result<std::unique_ptr<StateStore>>::Err(appended);
      }
      ++report.repaired_markers;
    }
    if (report.repaired_markers > 0) {
      report.outcome = RecoveryOutcome::RepairedCommitMarker;
      report.detail += "; a missing commit marker was repaired";
    } else if (report.discarded_unpublished > 0) {
      report.outcome = RecoveryOutcome::DiscardedUnpublished;
      report.detail += "; " + std::to_string(report.discarded_unpublished) +
                       " unpublished intent(s) were discarded";
    }
  }

  report.stray_files_removed = stray_files;
  if (stray_files > 0) {
    report.detail += "; " + std::to_string(stray_files) + " staged file(s) removed";
  }
  report.loaded_sequence = state.sequence;

  store->state_ = std::move(state);
  store->recovery_ = std::move(report);

  // Retain only the newest generations so the store does not grow without
  // bound; older files are deleted on a best-effort basis.
  std::vector<StateSequence> retained;
  for (const std::string& name : names.value()) {
    StateSequence sequence;
    if (internal::ParseSnapshotFileName(name, sequence) && !EndsWith(name, ".tmp")) {
      retained.push_back(sequence);
    }
  }
  std::sort(retained.begin(), retained.end(),
            [](StateSequence a, StateSequence b) { return b < a; });
  for (std::size_t i = kRetainedSnapshots; i < retained.size(); ++i) {
    internal::RemoveFile(internal::JoinPath(options.directory,
                                            internal::SnapshotFileName(retained[i])));
  }

  return Result<std::unique_ptr<StateStore>>::Ok(std::move(store));
}

Result<StateSequence> StateStore::Commit(PersistedState next, const CommitOptions& options) {
  if (committing_) {
    return Result<StateSequence>::Err(
        ErrorCode::Internal, "a commit observer re-entered the store during publication");
  }
  committing_ = true;
  struct Reset {
    bool* flag;
    ~Reset() { *flag = false; }
  } reset{&committing_};

  if (options.expected_previous != state_.sequence) {
    return Result<StateSequence>::Err(
        ErrorCode::Conflict,
        "expected previous sequence " + options.expected_previous.ToString() +
            " does not match the published sequence " + state_.sequence.ToString());
  }
  auto next_sequence = state_.sequence.Next();
  if (!next_sequence) {
    return Result<StateSequence>::Err(next_sequence.status());
  }
  const auto notify = [this](CommitStage stage) {
    if (!commit_observer_) {
      return;
    }
    try {
      commit_observer_(stage);
    } catch (...) {
      // An observer is instrumentation only; it may never break publication.
    }
  };

  next.format_version = kStoreFormatVersion;
  next.sequence = next_sequence.value();
  next.previous_digest = state_.digest();
  // The store owns this invariant: a published generation always carries the
  // sequence it is published under, so a caller cannot publish a mismatch.
  next.committed.state_sequence = next.sequence;

  auto encoded = internal::EncodeSnapshotFile(next);
  if (!encoded) {
    return Result<StateSequence>::Err(encoded.status());
  }

  const std::string staged = internal::JoinPath(directory_, StagedSnapshotName(next.sequence));
  const std::string target =
      internal::JoinPath(directory_, internal::SnapshotFileName(next.sequence));

  Status status = internal::WriteFileFlushed(staged, encoded.value());
  if (!status.ok()) {
    internal::RemoveFile(staged);
    return Result<StateSequence>::Err(status);
  }
  notify(CommitStage::StagedWritten);

  auto readback = internal::ReadFileBytes(staged);
  if (!readback) {
    internal::RemoveFile(staged);
    return Result<StateSequence>::Err(readback.status());
  }
  if (readback.value() != encoded.value()) {
    internal::RemoveFile(staged);
    return Result<StateSequence>::Err(
        ErrorCode::Corrupt, "the staged snapshot did not read back byte for byte");
  }
  StateSequence staged_sequence;
  auto verified = internal::DecodeSnapshotFile(readback.value(), staged_sequence);
  if (!verified) {
    internal::RemoveFile(staged);
    return Result<StateSequence>::Err(verified.status());
  }
  if (staged_sequence != next.sequence) {
    internal::RemoveFile(staged);
    return Result<StateSequence>::Err(ErrorCode::Corrupt,
                                      "the staged snapshot carries the wrong sequence");
  }
  notify(CommitStage::StagedVerified);

  const std::string journal_path =
      internal::JoinPath(directory_, internal::kJournalFileName);
  const Digest payload_digest = next.digest();
  internal::JournalRecord intent;
  intent.kind = internal::JournalRecordKind::Intent;
  intent.sequence = next.sequence;
  intent.digest = payload_digest;
  intent.detail = "staged snapshot verified";
  status = internal::AppendJournalRecord(journal_path, intent);
  if (!status.ok()) {
    internal::RemoveFile(staged);
    return Result<StateSequence>::Err(status);
  }
  notify(CommitStage::IntentJournaled);

  status = internal::AtomicReplace(staged, target);
  if (!status.ok()) {
    internal::RemoveFile(staged);
    return Result<StateSequence>::Err(status);
  }

  // The rename is the commit point: from here the generation is authoritative.
  state_ = std::move(next);
  ++committed_generations_;
  committed_bytes_ += static_cast<std::uint64_t>(encoded.value().size());
  notify(CommitStage::Published);

  internal::JournalRecord marker;
  marker.kind = internal::JournalRecordKind::Committed;
  marker.sequence = state_.sequence;
  marker.digest = payload_digest;
  marker.detail = "publish observed";
  const Status marked = internal::AppendJournalRecord(journal_path, marker);
  if (marked.ok()) {
    notify(CommitStage::CommitJournaled);
  } else {
    recovery_.detail += "; commit marker could not be written after publication";
    recovery_.repaired_markers = 1;
  }

  auto names = internal::ListDirectory(directory_);
  if (names) {
    std::vector<StateSequence> snapshots;
    for (const std::string& name : names.value()) {
      StateSequence sequence;
      if (internal::ParseSnapshotFileName(name, sequence) && !EndsWith(name, ".tmp")) {
        snapshots.push_back(sequence);
      }
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](StateSequence a, StateSequence b) { return b < a; });
    for (std::size_t i = kRetainedSnapshots; i < snapshots.size(); ++i) {
      internal::RemoveFile(
          internal::JoinPath(directory_, internal::SnapshotFileName(snapshots[i])));
    }
  }

  return Result<StateSequence>::Ok(state_.sequence);
}

Result<PersistedState> StateStore::VerifyOnDisk() const {
  auto names = internal::ListDirectory(directory_);
  if (!names) {
    return Result<PersistedState>::Err(names.status());
  }
  std::vector<StateSequence> snapshots;
  for (const std::string& name : names.value()) {
    StateSequence sequence;
    if (internal::ParseSnapshotFileName(name, sequence) && !EndsWith(name, ".tmp")) {
      snapshots.push_back(sequence);
    }
  }
  if (snapshots.empty()) {
    return Result<PersistedState>::Err(ErrorCode::NotFound,
                                       "no published generation exists on disk");
  }
  std::sort(snapshots.begin(), snapshots.end(),
            [](StateSequence a, StateSequence b) { return b < a; });
  const std::string path =
      internal::JoinPath(directory_, internal::SnapshotFileName(snapshots.front()));
  auto bytes = internal::ReadFileBytes(path);
  if (!bytes) {
    return Result<PersistedState>::Err(bytes.status());
  }
  StateSequence sequence;
  return internal::DecodeSnapshotFile(bytes.value(), sequence);
}

}  // namespace dom
