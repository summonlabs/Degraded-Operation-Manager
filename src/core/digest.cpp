// Degraded Operation Manager - SHA-256 and checked arithmetic.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/ids.hpp"

#include <cstring>
#include <limits>

namespace dom {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::uint32_t RotateRight(std::uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32u - bits));
}

void EncodeBigEndian64(std::uint64_t value, std::uint8_t* out) {
  for (unsigned i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (56u - 8u * i)) & 0xFFu);
  }
}

constexpr char kHexDigits[] = "0123456789abcdef";

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

bool Digest::IsZero() const {
  for (std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::string Digest::ToHex() const {
  std::string text;
  text.resize(kBytes * 2);
  for (std::size_t i = 0; i < kBytes; ++i) {
    text[i * 2] = kHexDigits[bytes_[i] >> 4];
    text[i * 2 + 1] = kHexDigits[bytes_[i] & 0x0Fu];
  }
  return text;
}

Result<Digest> Digest::FromHex(std::string_view hex) {
  if (hex.size() != kBytes * 2) {
    return Result<Digest>::Err(ErrorCode::InvalidArgument,
                               "digest hex must be 64 characters");
  }
  std::array<std::uint8_t, kBytes> bytes{};
  for (std::size_t i = 0; i < kBytes; ++i) {
    const int high = HexValue(hex[i * 2]);
    const int low = HexValue(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return Result<Digest>::Err(ErrorCode::InvalidArgument,
                                 "digest hex contains a non-hex character");
    }
    bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return Result<Digest>::Ok(Digest(bytes));
}

Sha256::Sha256()
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu,
             0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256::Compress(const std::uint8_t* block) {
  std::uint32_t w[64];
  for (unsigned i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (unsigned i = 16; i < 64; ++i) {
    const std::uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (unsigned i = 0; i < 64; ++i) {
    const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(std::span<const std::uint8_t> data) {
  total_bytes_ += data.size();
  std::size_t offset = 0;
  if (buffered_ != 0) {
    while (offset < data.size() && buffered_ < buffer_.size()) {
      buffer_[buffered_++] = data[offset++];
    }
    if (buffered_ == buffer_.size()) {
      Compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (data.size() - offset >= buffer_.size()) {
    Compress(data.data() + offset);
    offset += buffer_.size();
  }
  while (offset < data.size()) {
    // At this point fewer than one block remains and the buffer is either empty
    // or was left below its capacity, so the write cannot overrun it. The guard
    // states that invariant explicitly instead of relying on it.
    if (buffered_ >= buffer_.size()) {
      Compress(buffer_.data());
      buffered_ = 0;
    }
    buffer_[buffered_++] = data[offset++];
  }
}

void Sha256::Update(std::string_view text) {
  Update(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Digest Sha256::Finish() {
  const std::uint64_t bit_length = total_bytes_ * 8u;
  std::uint8_t padding = 0x80u;
  Update(std::span<const std::uint8_t>(&padding, 1));
  const std::uint8_t zero = 0x00u;
  while (buffered_ != 56) {
    Update(std::span<const std::uint8_t>(&zero, 1));
  }
  std::uint8_t length_bytes[8];
  EncodeBigEndian64(bit_length, length_bytes);
  // Bypass Update's length accounting for the trailing length field.
  for (std::uint8_t byte : length_bytes) {
    buffer_[buffered_++] = byte;
  }
  Compress(buffer_.data());
  buffered_ = 0;

  std::array<std::uint8_t, Digest::kBytes> out{};
  for (unsigned i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
  }
  return Digest(out);
}

Digest Sha256::Of(std::span<const std::uint8_t> data) {
  Sha256 hasher;
  hasher.Update(data);
  return hasher.Finish();
}

Digest Sha256::Of(std::string_view text) {
  Sha256 hasher;
  hasher.Update(text);
  return hasher.Finish();
}

Result<std::uint64_t> CheckedAdd(std::uint64_t a, std::uint64_t b) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return Result<std::uint64_t>::Err(ErrorCode::Overflow, "unsigned addition overflow");
  }
  return Result<std::uint64_t>::Ok(a + b);
}

Result<std::uint64_t> CheckedSub(std::uint64_t a, std::uint64_t b) {
  if (b > a) {
    return Result<std::uint64_t>::Err(ErrorCode::Underflow, "unsigned subtraction underflow");
  }
  return Result<std::uint64_t>::Ok(a - b);
}

Result<std::uint64_t> CheckedMul(std::uint64_t a, std::uint64_t b) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return Result<std::uint64_t>::Err(ErrorCode::Overflow, "unsigned multiplication overflow");
  }
  return Result<std::uint64_t>::Ok(a * b);
}

Result<std::int64_t> CheckedAddSigned(std::int64_t a, std::int64_t b) {
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  const std::int64_t min = std::numeric_limits<std::int64_t>::min();
  if (b > 0 && a > max - b) {
    return Result<std::int64_t>::Err(ErrorCode::Overflow, "signed addition overflow");
  }
  if (b < 0 && a < min - b) {
    return Result<std::int64_t>::Err(ErrorCode::Underflow, "signed addition underflow");
  }
  return Result<std::int64_t>::Ok(a + b);
}

Result<std::int64_t> CheckedSubSigned(std::int64_t a, std::int64_t b) {
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  const std::int64_t min = std::numeric_limits<std::int64_t>::min();
  if (b < 0 && a > max + b) {
    return Result<std::int64_t>::Err(ErrorCode::Overflow, "signed subtraction overflow");
  }
  if (b > 0 && a < min + b) {
    return Result<std::int64_t>::Err(ErrorCode::Underflow, "signed subtraction underflow");
  }
  return Result<std::int64_t>::Ok(a - b);
}

Result<std::uint32_t> NarrowU32(std::uint64_t value) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return Result<std::uint32_t>::Err(ErrorCode::Overflow, "value does not fit in 32 bits");
  }
  return Result<std::uint32_t>::Ok(static_cast<std::uint32_t>(value));
}

Result<std::uint8_t> NarrowU8(std::uint64_t value) {
  if (value > std::numeric_limits<std::uint8_t>::max()) {
    return Result<std::uint8_t>::Err(ErrorCode::Overflow, "value does not fit in 8 bits");
  }
  return Result<std::uint8_t>::Ok(static_cast<std::uint8_t>(value));
}

Result<Duration> Elapsed(Instant earlier, Instant later) {
  if (later < earlier) {
    return Result<Duration>::Err(ErrorCode::Underflow,
                                 "instant ordering is reversed; elapsed is not defined");
  }
  return Result<Duration>::Ok(Duration::FromValue(later.value() - earlier.value()));
}

Result<Instant> Advance(Instant instant, Duration duration) {
  auto sum = CheckedAdd(instant.value(), duration.value());
  if (!sum) {
    return Result<Instant>::Err(sum.status());
  }
  return Result<Instant>::Ok(Instant::FromValue(sum.value()));
}

}  // namespace dom
