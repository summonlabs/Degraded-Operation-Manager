// Degraded Operation Manager - publication journal.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "store/journal.hpp"

#include <cstring>

#include "dom/limits.hpp"
#include "store/file_io.hpp"

namespace dom::internal {
namespace {

constexpr char kMagic[8] = {'D', 'O', 'M', 'J', 'R', 'N', 'L', '1'};
constexpr std::uint16_t kJournalVersion = 1;
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kRecordFixedSize = 1 + 8 + Digest::kBytes + 4 + Digest::kBytes;

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu));
  }
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu));
  }
}

std::uint16_t ReadU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) |
                                    (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8u * i);
  }
  return value;
}

std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8u * i);
  }
  return value;
}

}  // namespace

const char* JournalRecordKindName(JournalRecordKind kind) noexcept {
  switch (kind) {
    case JournalRecordKind::Intent:
      return "intent";
    case JournalRecordKind::Committed:
      return "committed";
    case JournalRecordKind::Abandoned:
      return "abandoned";
  }
  return "unknown";
}

std::vector<std::uint8_t> EncodeJournalHeader() {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
  PutU16(out, kJournalVersion);
  PutU16(out, 0);
  return out;
}

Result<std::vector<std::uint8_t>> EncodeJournalRecord(const JournalRecord& record) {
  if (record.detail.size() > limits::kMaxStringBytes) {
    return Result<std::vector<std::uint8_t>>::Err(ErrorCode::LimitExceeded,
                                                  "journal detail exceeds the byte limit");
  }
  std::vector<std::uint8_t> out;
  out.reserve(kRecordFixedSize + record.detail.size());
  out.push_back(static_cast<std::uint8_t>(record.kind));
  PutU64(out, record.sequence.value());
  const auto digest_bytes = record.digest.bytes();
  out.insert(out.end(), digest_bytes.begin(), digest_bytes.end());
  PutU32(out, static_cast<std::uint32_t>(record.detail.size()));
  out.insert(out.end(), record.detail.begin(), record.detail.end());
  const Digest record_digest = Sha256::Of(out);
  const auto record_bytes = record_digest.bytes();
  out.insert(out.end(), record_bytes.begin(), record_bytes.end());
  return Result<std::vector<std::uint8_t>>::Ok(std::move(out));
}

Result<JournalScan> ScanJournal(std::span<const std::uint8_t> bytes) {
  JournalScan scan;
  if (bytes.empty()) {
    return Result<JournalScan>::Ok(std::move(scan));
  }
  if (bytes.size() < kHeaderSize) {
    scan.torn_tail = true;
    return Result<JournalScan>::Ok(std::move(scan));
  }
  if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
    return Result<JournalScan>::Err(ErrorCode::IncompatibleFormat,
                                    "journal magic does not match");
  }
  const std::uint16_t version = ReadU16(bytes, 8);
  if (version != kJournalVersion) {
    return Result<JournalScan>::Err(
        ErrorCode::IncompatibleFormat,
        "journal format version " + std::to_string(version) + " is not supported");
  }
  if (ReadU16(bytes, 10) != 0) {
    return Result<JournalScan>::Err(ErrorCode::Corrupt, "journal reserved field is not zero");
  }
  std::size_t offset = kHeaderSize;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    if (remaining < kRecordFixedSize) {
      scan.torn_tail = true;
      break;
    }
    const std::uint8_t raw_kind = bytes[offset];
    if (raw_kind < 1 || raw_kind > 3) {
      return Result<JournalScan>::Err(ErrorCode::Corrupt, "journal record kind is not known");
    }
    const std::uint32_t detail_length = ReadU32(bytes, offset + 41);
    if (detail_length > limits::kMaxStringBytes) {
      return Result<JournalScan>::Err(ErrorCode::Corrupt,
                                      "journal record detail length exceeds the byte limit");
    }
    const std::size_t record_size = kRecordFixedSize + static_cast<std::size_t>(detail_length);
    if (remaining < record_size) {
      scan.torn_tail = true;
      break;
    }
    const Digest recorded_digest = Sha256::Of(bytes.subspan(offset, record_size - Digest::kBytes));
    if (std::memcmp(recorded_digest.bytes().data(), bytes.data() + offset + record_size -
                                                          Digest::kBytes,
                    Digest::kBytes) != 0) {
      return Result<JournalScan>::Err(ErrorCode::Corrupt,
                                      "journal record digest does not verify");
    }
    JournalRecord record;
    record.kind = static_cast<JournalRecordKind>(raw_kind);
    record.sequence = StateSequence::FromValue(ReadU64(bytes, offset + 1));
    std::array<std::uint8_t, Digest::kBytes> digest_bytes{};
    std::memcpy(digest_bytes.data(), bytes.data() + offset + 9, Digest::kBytes);
    record.digest = Digest(digest_bytes);
    record.detail.assign(reinterpret_cast<const char*>(bytes.data() + offset + 45),
                         detail_length);
    scan.records.push_back(std::move(record));
    offset += record_size;
  }
  scan.records_scanned = scan.records.size();
  scan.bytes_consumed = offset;
  return Result<JournalScan>::Ok(std::move(scan));
}

Status AppendJournalRecord(const std::string& path, const JournalRecord& record) {
  auto encoded = EncodeJournalRecord(record);
  if (!encoded) {
    return encoded.status();
  }
  const Result<bool> exists = FileExists(path);
  if (!exists) {
    return exists.status();
  }
  if (!exists.value()) {
    const std::vector<std::uint8_t> header = EncodeJournalHeader();
    Status status = WriteFileFlushed(path, header);
    if (!status.ok()) {
      return status;
    }
  }
  return AppendFileFlushed(path, encoded.value());
}

}  // namespace dom::internal
