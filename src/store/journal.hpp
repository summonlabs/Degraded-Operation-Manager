// Degraded Operation Manager - publication journal (private header).
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The journal records the publication protocol, not the application state:
//   Intent(seq, digest)     - a verified staged file is about to be published
//   Committed(seq, digest)  - the atomic publish was observed
//   Abandoned(seq, reason)  - an intent that was never published
// The atomic rename of the snapshot file is the commit point. The journal makes
// crash recovery able to explain what happened, and lets recovery discard an
// intent that never became authoritative.
//
// File layout:
//   header:  magic "DOMJRNL1", format version (u16), reserved (u16)
//   records: kind (u8), sequence (u64), digest (32), detail length (u32),
//            detail bytes, record digest (32)
// A record that is present but does not verify is corruption and is refused. A
// record that is cut short by the end of the file is a torn tail: the last
// append did not complete, and it is reported as such rather than guessed at.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dom/ids.hpp"
#include "dom/status.hpp"

namespace dom::internal {

enum class JournalRecordKind : std::uint8_t {
  Intent = 1,
  Committed = 2,
  Abandoned = 3,
};

const char* JournalRecordKindName(JournalRecordKind kind) noexcept;

struct JournalRecord {
  JournalRecordKind kind = JournalRecordKind::Intent;
  StateSequence sequence;
  Digest digest;
  std::string detail;
};

struct JournalScan {
  std::vector<JournalRecord> records;
  bool torn_tail = false;
  std::size_t records_scanned = 0;
  std::size_t bytes_consumed = 0;
};

std::vector<std::uint8_t> EncodeJournalHeader();
Result<std::vector<std::uint8_t>> EncodeJournalRecord(const JournalRecord& record);
Result<JournalScan> ScanJournal(std::span<const std::uint8_t> bytes);

/// Appends one record and flushes it to stable storage.
Status AppendJournalRecord(const std::string& path, const JournalRecord& record);

}  // namespace dom::internal
