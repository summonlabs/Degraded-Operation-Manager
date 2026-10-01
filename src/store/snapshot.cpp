// Degraded Operation Manager - snapshot file format.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "store/snapshot.hpp"

#include <array>
#include <cstring>

#include "codec/binary_writer.hpp"
#include "codec/canonical.hpp"
#include "dom/limits.hpp"
#include "dom/version.hpp"

namespace dom::internal {
namespace {

constexpr char kMagic[8] = {'D', 'O', 'M', 'S', 'N', 'A', 'P', '1'};

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
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

std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8u * i);
  }
  return value;
}

}  // namespace

Result<std::vector<std::uint8_t>> EncodeSnapshotFile(const PersistedState& state) {
  auto payload = EncodeStatePayload(state);
  if (!payload) {
    return Result<std::vector<std::uint8_t>>::Err(payload.status());
  }
  if (payload.value().size() > limits::kMaxEnvelopeBytes) {
    return Result<std::vector<std::uint8_t>>::Err(ErrorCode::LimitExceeded,
                                                  "state payload exceeds the byte bound");
  }
  const Digest payload_digest = Sha256::Of(payload.value());

  std::vector<std::uint8_t> out;
  out.reserve(kSnapshotHeaderSize + payload.value().size() + kSnapshotFooterSize);
  out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
  PutU16(out, kStoreFormatVersion);
  PutU16(out, 0);
  PutU64(out, state.sequence.value());
  PutU64(out, static_cast<std::uint64_t>(payload.value().size()));
  const auto previous = state.previous_digest.bytes();
  out.insert(out.end(), previous.begin(), previous.end());
  const auto payload_bytes = payload_digest.bytes();
  out.insert(out.end(), payload_bytes.begin(), payload_bytes.end());
  out.insert(out.end(), payload.value().begin(), payload.value().end());
  const Digest footer = Sha256::Of(out);
  const auto footer_bytes = footer.bytes();
  out.insert(out.end(), footer_bytes.begin(), footer_bytes.end());
  return Result<std::vector<std::uint8_t>>::Ok(std::move(out));
}

Result<Digest> SnapshotChainDigest(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kSnapshotHeaderSize + kSnapshotFooterSize) {
    return Result<Digest>::Err(ErrorCode::Truncated, "snapshot file is shorter than its framing");
  }
  if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
    return Result<Digest>::Err(ErrorCode::IncompatibleFormat, "snapshot magic does not match");
  }
  const std::uint64_t payload_length = ReadU64(bytes, 20);
  const std::size_t expected =
      kSnapshotHeaderSize + static_cast<std::size_t>(payload_length) + kSnapshotFooterSize;
  if (bytes.size() != expected) {
    return Result<Digest>::Err(ErrorCode::Corrupt,
                               "snapshot length does not match its recorded payload length");
  }
  const Digest footer = Sha256::Of(bytes.first(bytes.size() - kSnapshotFooterSize));
  const auto recorded = footer.bytes();
  if (std::memcmp(recorded.data(), bytes.data() + bytes.size() - kSnapshotFooterSize,
                  Digest::kBytes) != 0) {
    return Result<Digest>::Err(ErrorCode::Corrupt, "snapshot footer digest does not verify");
  }
  return Result<Digest>::Ok(Sha256::Of(bytes.first(bytes.size() - kSnapshotFooterSize)));
}

Result<PersistedState> DecodeSnapshotFile(std::span<const std::uint8_t> bytes,
                                          StateSequence& file_sequence) {
  if (bytes.size() < kSnapshotHeaderSize + kSnapshotFooterSize) {
    return Result<PersistedState>::Err(ErrorCode::Truncated,
                                       "snapshot file is shorter than its framing");
  }
  if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
    return Result<PersistedState>::Err(ErrorCode::IncompatibleFormat,
                                       "snapshot magic does not match");
  }
  const std::uint16_t version = ReadU16(bytes, 8);
  if (version != kStoreFormatVersion) {
    return Result<PersistedState>::Err(
        ErrorCode::IncompatibleFormat,
        "snapshot format version " + std::to_string(version) + " is not supported");
  }
  if (ReadU16(bytes, 10) != 0) {
    return Result<PersistedState>::Err(ErrorCode::Corrupt, "snapshot reserved field is not zero");
  }
  file_sequence = StateSequence::FromValue(ReadU64(bytes, 12));
  const std::uint64_t payload_length = ReadU64(bytes, 20);
  if (payload_length > limits::kMaxEnvelopeBytes) {
    return Result<PersistedState>::Err(ErrorCode::LimitExceeded,
                                       "snapshot payload length exceeds the byte bound");
  }
  const std::size_t expected =
      kSnapshotHeaderSize + static_cast<std::size_t>(payload_length) + kSnapshotFooterSize;
  if (bytes.size() != expected) {
    return Result<PersistedState>::Err(
        ErrorCode::Corrupt, "snapshot length does not match its recorded payload length");
  }
  const std::span<const std::uint8_t> payload =
      bytes.subspan(kSnapshotHeaderSize, static_cast<std::size_t>(payload_length));
  const Digest payload_digest = Sha256::Of(payload);
  if (std::memcmp(payload_digest.bytes().data(), bytes.data() + 60, Digest::kBytes) != 0) {
    return Result<PersistedState>::Err(ErrorCode::Corrupt,
                                       "snapshot payload digest does not verify");
  }
  const Digest footer = Sha256::Of(bytes.first(bytes.size() - kSnapshotFooterSize));
  if (std::memcmp(footer.bytes().data(), bytes.data() + bytes.size() - kSnapshotFooterSize,
                  Digest::kBytes) != 0) {
    return Result<PersistedState>::Err(ErrorCode::Corrupt,
                                       "snapshot footer digest does not verify");
  }
  auto state = DecodeStatePayload(payload);
  if (!state) {
    return Result<PersistedState>::Err(state.status());
  }
  state.value().sequence = file_sequence;
  if (state.value().committed.state_sequence != file_sequence) {
    return Result<PersistedState>::Err(
        ErrorCode::Corrupt,
        "the committed state sequence inside the payload disagrees with the file sequence");
  }
  std::array<std::uint8_t, Digest::kBytes> previous{};
  std::memcpy(previous.data(), bytes.data() + 28, Digest::kBytes);
  state.value().previous_digest = Digest(previous);
  state.value().format_version = version;
  return state;
}

}  // namespace dom::internal
