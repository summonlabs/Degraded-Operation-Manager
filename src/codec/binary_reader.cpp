// Degraded Operation Manager - strict canonical binary reader.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "codec/binary_reader.hpp"

#include "dom/limits.hpp"

namespace dom::internal {

Status BinaryReader::Need(std::size_t count, std::string_view what) const {
  if (count > remaining()) {
    return Status::Error(ErrorCode::Truncated,
                         "input ends inside " + std::string(what));
  }
  return Status::Ok();
}

Result<std::span<const std::uint8_t>> BinaryReader::TakeBytes(std::size_t count) {
  Status status = Need(count, "a byte range");
  if (!status.ok()) {
    return Result<std::span<const std::uint8_t>>::Err(status);
  }
  auto view = data_.subspan(offset_, count);
  offset_ += count;
  return Result<std::span<const std::uint8_t>>::Ok(view);
}

Result<std::uint8_t> BinaryReader::TakeU8() {
  auto bytes = TakeBytes(1);
  if (!bytes) {
    return Result<std::uint8_t>::Err(bytes.status());
  }
  return Result<std::uint8_t>::Ok(bytes.value()[0]);
}

Result<std::uint16_t> BinaryReader::TakeU16() {
  auto bytes = TakeBytes(2);
  if (!bytes) {
    return Result<std::uint16_t>::Err(bytes.status());
  }
  const auto view = bytes.value();
  return Result<std::uint16_t>::Ok(static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(view[0]) | (static_cast<std::uint16_t>(view[1]) << 8)));
}

Result<std::uint32_t> BinaryReader::TakeU32() {
  auto bytes = TakeBytes(4);
  if (!bytes) {
    return Result<std::uint32_t>::Err(bytes.status());
  }
  const auto view = bytes.value();
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(view[i]) << (8u * i);
  }
  return Result<std::uint32_t>::Ok(value);
}

Result<std::uint64_t> BinaryReader::TakeU64() {
  auto bytes = TakeBytes(8);
  if (!bytes) {
    return Result<std::uint64_t>::Err(bytes.status());
  }
  const auto view = bytes.value();
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(view[i]) << (8u * i);
  }
  return Result<std::uint64_t>::Ok(value);
}

Result<std::int64_t> BinaryReader::TakeI64() {
  auto value = TakeU64();
  if (!value) {
    return Result<std::int64_t>::Err(value.status());
  }
  return Result<std::int64_t>::Ok(static_cast<std::int64_t>(value.value()));
}

Result<bool> BinaryReader::TakeBool() {
  auto value = TakeU8();
  if (!value) {
    return Result<bool>::Err(value.status());
  }
  if (value.value() > 1u) {
    return Result<bool>::Err(ErrorCode::DecodeError, "boolean must be 0 or 1");
  }
  return Result<bool>::Ok(value.value() == 1u);
}

Result<Digest> BinaryReader::TakeDigest() {
  auto bytes = TakeBytes(Digest::kBytes);
  if (!bytes) {
    return Result<Digest>::Err(bytes.status());
  }
  std::array<std::uint8_t, Digest::kBytes> raw{};
  const auto view = bytes.value();
  for (std::size_t i = 0; i < Digest::kBytes; ++i) {
    raw[i] = view[i];
  }
  return Result<Digest>::Ok(Digest(raw));
}

Result<std::string> BinaryReader::TakeString() {
  auto length = TakeU32();
  if (!length) {
    return Result<std::string>::Err(length.status());
  }
  if (length.value() > limits::kMaxStringBytes) {
    return Result<std::string>::Err(ErrorCode::LimitExceeded,
                                    "string length exceeds the byte limit");
  }
  auto bytes = TakeBytes(length.value());
  if (!bytes) {
    return Result<std::string>::Err(bytes.status());
  }
  std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
  if (text.find('\0') != std::string::npos) {
    return Result<std::string>::Err(ErrorCode::DecodeError,
                                    "string contains an embedded NUL byte");
  }
  Status status = ValidateUtf8(text);
  if (!status.ok()) {
    return Result<std::string>::Err(status);
  }
  return Result<std::string>::Ok(std::move(text));
}

Result<Scalar> BinaryReader::TakeScalar() {
  auto unit = TakeU8();
  if (!unit) {
    return Result<Scalar>::Err(unit.status());
  }
  if (unit.value() > static_cast<std::uint8_t>(Unit::Code)) {
    return Result<Scalar>::Err(ErrorCode::DecodeError, "unknown unit code");
  }
  auto value = TakeI64();
  if (!value) {
    return Result<Scalar>::Err(value.status());
  }
  return Result<Scalar>::Ok(Scalar(static_cast<Unit>(unit.value()), value.value()));
}

Result<std::uint32_t> BinaryReader::TakeCount(std::uint32_t bound, std::string_view what) {
  auto count = TakeU32();
  if (!count) {
    return count;
  }
  if (count.value() > bound) {
    return Result<std::uint32_t>::Err(
        ErrorCode::LimitExceeded,
        std::string(what) + " count exceeds the supported bound");
  }
  return count;
}

Status BinaryReader::ExpectEnd() const {
  if (!AtEnd()) {
    return Status::Error(ErrorCode::DecodeError,
                         "trailing bytes after a complete canonical envelope");
  }
  return Status::Ok();
}

Status ValidateUtf8(std::string_view text) {
  std::size_t i = 0;
  const std::size_t size = text.size();
  while (i < size) {
    const auto byte = static_cast<std::uint8_t>(text[i]);
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (byte < 0x80u) {
      ++i;
      continue;
    } else if ((byte & 0xE0u) == 0xC0u) {
      extra = 1;
      code_point = byte & 0x1Fu;
      minimum = 0x80u;
    } else if ((byte & 0xF0u) == 0xE0u) {
      extra = 2;
      code_point = byte & 0x0Fu;
      minimum = 0x800u;
    } else if ((byte & 0xF8u) == 0xF0u) {
      extra = 3;
      code_point = byte & 0x07u;
      minimum = 0x10000u;
    } else {
      return Status::Error(ErrorCode::DecodeError, "invalid UTF-8 leading byte");
    }
    if (i + extra >= size) {
      return Status::Error(ErrorCode::DecodeError, "truncated UTF-8 sequence");
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto continuation = static_cast<std::uint8_t>(text[i + k]);
      if ((continuation & 0xC0u) != 0x80u) {
        return Status::Error(ErrorCode::DecodeError, "invalid UTF-8 continuation byte");
      }
      code_point = (code_point << 6) | (continuation & 0x3Fu);
    }
    if (code_point < minimum) {
      return Status::Error(ErrorCode::DecodeError, "overlong UTF-8 encoding");
    }
    if (code_point > 0x10FFFFu) {
      return Status::Error(ErrorCode::DecodeError, "code point above U+10FFFF");
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return Status::Error(ErrorCode::DecodeError, "UTF-8 surrogate code point");
    }
    i += extra + 1;
  }
  return Status::Ok();
}

}  // namespace dom::internal
