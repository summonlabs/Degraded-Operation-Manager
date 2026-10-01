// Degraded Operation Manager - canonical binary writer.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "codec/binary_writer.hpp"

#include "dom/limits.hpp"

namespace dom::internal {

Status BinaryWriter::PutU8(std::uint8_t value) {
  buffer_.push_back(value);
  return Status::Ok();
}

Status BinaryWriter::PutU16(std::uint16_t value) {
  buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  return Status::Ok();
}

Status BinaryWriter::PutU32(std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu));
  }
  return Status::Ok();
}

Status BinaryWriter::PutU64(std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu));
  }
  return Status::Ok();
}

Status BinaryWriter::PutI64(std::int64_t value) {
  return PutU64(static_cast<std::uint64_t>(value));
}

Status BinaryWriter::PutBool(bool value) {
  return PutU8(value ? 1u : 0u);
}

Status BinaryWriter::PutDigest(const Digest& digest) {
  const auto bytes = digest.bytes();
  return PutBytes(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

Status BinaryWriter::PutBytes(std::span<const std::uint8_t> bytes) {
  if (buffer_.size() > limits::kMaxEnvelopeBytes ||
      bytes.size() > limits::kMaxEnvelopeBytes - buffer_.size()) {
    return Status::Error(ErrorCode::LimitExceeded, "encoded envelope exceeds the byte limit");
  }
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  return Status::Ok();
}

Status BinaryWriter::PutString(std::string_view text) {
  if (text.size() > limits::kMaxStringBytes) {
    return Status::Error(ErrorCode::LimitExceeded, "string exceeds the byte limit");
  }
  for (char c : text) {
    if (c == '\0') {
      return Status::Error(ErrorCode::InvalidArgument, "string contains an embedded NUL");
    }
  }
  auto length = NarrowU32(text.size());
  if (!length) {
    return length.status();
  }
  Status status = PutU32(length.value());
  if (!status.ok()) {
    return status;
  }
  return PutBytes(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Status BinaryWriter::PutScalar(const Scalar& scalar) {
  Status status = PutU8(static_cast<std::uint8_t>(scalar.unit()));
  if (!status.ok()) {
    return status;
  }
  return PutI64(scalar.value());
}

Status BinaryWriter::PutCount(std::size_t count) {
  if (count > limits::kMaxCollectionItems) {
    return Status::Error(ErrorCode::LimitExceeded, "collection exceeds the item limit");
  }
  auto value = NarrowU32(count);
  if (!value) {
    return value.status();
  }
  return PutU32(value.value());
}

}  // namespace dom::internal
