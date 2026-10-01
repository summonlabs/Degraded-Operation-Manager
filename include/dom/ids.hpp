// Degraded Operation Manager - identities, digests, counters and checked math.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "dom/status.hpp"

namespace dom {

/// 256-bit digest used for content identity, integrity checks and idempotency
/// keys. Produced by the SHA-256 implementation in this library; the digest of
/// a canonical encoding is the identity of that value.
class Digest {
 public:
  static constexpr std::size_t kBytes = 32;

  Digest() = default;
  explicit Digest(std::array<std::uint8_t, kBytes> bytes) : bytes_(bytes) {}

  static Digest Zero() { return Digest(); }
  bool IsZero() const;

  const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
  std::span<const std::uint8_t, kBytes> span() const noexcept { return bytes_; }

  std::string ToHex() const;
  static Result<Digest> FromHex(std::string_view hex);

  friend bool operator==(const Digest& a, const Digest& b) { return a.bytes_ == b.bytes_; }
  friend std::strong_ordering operator<=>(const Digest& a, const Digest& b) {
    return a.bytes_ <=> b.bytes_;
  }

 private:
  std::array<std::uint8_t, kBytes> bytes_{};
};

/// Incremental SHA-256. Used for content digests, file integrity and the
/// canonical digest of encoded state.
class Sha256 {
 public:
  Sha256();

  void Update(std::span<const std::uint8_t> data);
  void Update(std::string_view text);
  Digest Finish();

  static Digest Of(std::span<const std::uint8_t> data);
  static Digest Of(std::string_view text);

 private:
  void Compress(const std::uint8_t* block);

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

/// Checked integer arithmetic. External input (counters, capacities, durations,
/// timestamps, quantities) is never allowed to wrap.
Result<std::uint64_t> CheckedAdd(std::uint64_t a, std::uint64_t b);
Result<std::uint64_t> CheckedSub(std::uint64_t a, std::uint64_t b);
Result<std::uint64_t> CheckedMul(std::uint64_t a, std::uint64_t b);
Result<std::int64_t> CheckedAddSigned(std::int64_t a, std::int64_t b);
Result<std::int64_t> CheckedSubSigned(std::int64_t a, std::int64_t b);
Result<std::uint32_t> NarrowU32(std::uint64_t value);
Result<std::uint8_t> NarrowU8(std::uint64_t value);

/// Strongly typed monotone counter. Distinct tags make different counters
/// different types, so a state sequence can never be passed where a policy
/// generation is expected.
template <class Tag>
class Counter {
 public:
  using Value = std::uint64_t;

  constexpr Counter() = default;
  static constexpr Counter FromValue(Value value) { return Counter(value); }

  constexpr Value value() const noexcept { return value_; }
  constexpr bool IsZero() const noexcept { return value_ == 0; }
  constexpr bool IsValid() const noexcept { return value_ != 0; }

  /// Verified increment: refuses to wrap at the maximum value.
  Result<Counter> Next() const {
    if (value_ == kMax) {
      return Result<Counter>::Err(ErrorCode::Overflow, "counter exhausted");
    }
    return Result<Counter>::Ok(Counter(value_ + 1));
  }

  /// Verified addition of another counter value.
  Result<Counter> Add(Value delta) const {
    auto sum = CheckedAdd(value_, delta);
    if (!sum) {
      return Result<Counter>::Err(sum.status());
    }
    return Result<Counter>::Ok(Counter(sum.value()));
  }

  std::string ToString() const { return std::to_string(value_); }

  friend constexpr bool operator==(const Counter& a, const Counter& b) {
    return a.value_ == b.value_;
  }
  friend constexpr std::strong_ordering operator<=>(const Counter& a, const Counter& b) {
    return a.value_ <=> b.value_;
  }

  static constexpr Value kMax = 0xFFFFFFFFFFFFFFFFull;

 private:
  explicit constexpr Counter(Value value) : value_(value) {}
  Value value_ = 0;
};

using StateSequence = Counter<struct StateSequenceTag>;
using DecisionSequence = Counter<struct DecisionSequenceTag>;
using PolicyGeneration = Counter<struct PolicyGenerationTag>;
using EvidenceRevision = Counter<struct EvidenceRevisionTag>;
using Generation = Counter<struct GenerationTag>;
using ControlEpoch = Counter<struct ControlEpochTag>;
using Incarnation = Counter<struct IncarnationTag>;
using Tick = Counter<struct TickTag>;
using ModeId = Counter<struct ModeIdTag>;
using ObligationClassId = Counter<struct ObligationClassIdTag>;
using RestrictionId = Counter<struct RestrictionIdTag>;
using AuthorizationId = Counter<struct AuthorizationIdTag>;
using PolicyId = Counter<struct PolicyIdTag>;
using RevisionId = Counter<struct RevisionIdTag>;

/// Milliseconds since the Unix epoch, UTC. Wall clock is an input, never an
/// implicit dependency: evaluation functions receive the instant to use.
using Instant = Counter<struct InstantTag>;
/// Milliseconds. Durations are unsigned and never wrap.
using Duration = Counter<struct DurationTag>;

/// Elapsed time between two instants; refuses a reversed interval.
Result<Duration> Elapsed(Instant earlier, Instant later);
/// Absolute instant reached by adding a duration; refuses overflow.
Result<Instant> Advance(Instant instant, Duration duration);

}  // namespace dom
