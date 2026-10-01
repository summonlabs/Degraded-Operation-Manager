// Degraded Operation Manager - canonical binary writer (private header).
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dom/ids.hpp"
#include "dom/scalar.hpp"
#include "dom/status.hpp"

namespace dom::internal {

/// Append-only canonical encoder. Every length is checked against an explicit
/// bound before anything is written, so a hostile value cannot make an encoder
/// allocate without limit.
class BinaryWriter {
 public:
  Status PutU8(std::uint8_t value);
  Status PutU16(std::uint16_t value);
  Status PutU32(std::uint32_t value);
  Status PutU64(std::uint64_t value);
  Status PutI64(std::int64_t value);
  Status PutBool(bool value);
  Status PutDigest(const Digest& digest);
  Status PutString(std::string_view text);
  Status PutBytes(std::span<const std::uint8_t> bytes);
  Status PutScalar(const Scalar& scalar);
  /// Count prefix for a collection, bounded by limits::kMaxCollectionItems.
  Status PutCount(std::size_t count);

  const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
  std::vector<std::uint8_t> Take() { return std::move(buffer_); }
  std::size_t size() const noexcept { return buffer_.size(); }

 private:
  std::vector<std::uint8_t> buffer_;
};

}  // namespace dom::internal
