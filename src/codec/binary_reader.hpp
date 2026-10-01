// Degraded Operation Manager - strict canonical binary reader (private header).
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "dom/ids.hpp"
#include "dom/scalar.hpp"
#include "dom/status.hpp"

namespace dom::internal {

/// Strict decoder. Every read is bounds checked, every enum code validated by
/// the caller helper, and trailing bytes are refused by ExpectEnd.
class BinaryReader {
 public:
  explicit BinaryReader(std::span<const std::uint8_t> data) : data_(data) {}

  Result<std::uint8_t> TakeU8();
  Result<std::uint16_t> TakeU16();
  Result<std::uint32_t> TakeU32();
  Result<std::uint64_t> TakeU64();
  Result<std::int64_t> TakeI64();
  Result<bool> TakeBool();
  Result<Digest> TakeDigest();
  Result<std::string> TakeString();
  Result<Scalar> TakeScalar();
  Result<std::span<const std::uint8_t>> TakeBytes(std::size_t count);
  /// Count prefix; refuses counts beyond the caller supplied bound.
  Result<std::uint32_t> TakeCount(std::uint32_t bound, std::string_view what);

  std::size_t remaining() const noexcept { return data_.size() - offset_; }
  std::size_t offset() const noexcept { return offset_; }
  bool AtEnd() const noexcept { return offset_ == data_.size(); }
  Status ExpectEnd() const;

 private:
  Status Need(std::size_t count, std::string_view what) const;

  std::span<const std::uint8_t> data_;
  std::size_t offset_ = 0;
};

/// Strict UTF-8 validation: rejects overlong forms, surrogates, code points
/// above U+10FFFF and truncated sequences.
Status ValidateUtf8(std::string_view text);

/// Enum helpers: refuse any code outside the known enumerator set.
template <class E>
Result<E> TakeEnum(BinaryReader& reader, std::uint8_t max_code, std::string_view what) {
  auto code = reader.TakeU8();
  if (!code) {
    return Result<E>::Err(code.status());
  }
  if (code.value() > max_code) {
    return Result<E>::Err(ErrorCode::DecodeError,
                          std::string("unknown ") + std::string(what) + " code");
  }
  return Result<E>::Ok(static_cast<E>(code.value()));
}

}  // namespace dom::internal
