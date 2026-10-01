// Degraded Operation Manager - typed scalar quantities.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "dom/status.hpp"

namespace dom {

/// Unit of a measured or declared quantity. Two scalars are only comparable
/// when their units are identical; a mismatch is reported rather than coerced.
enum class Unit : std::uint8_t {
  None = 0,
  Count = 1,
  MilliPercent = 2,
  MilliCelsius = 3,
  MilliRatio = 4,
  Milliseconds = 5,
  Watts = 6,
  Millivolts = 7,
  Milliamperes = 8,
  Boolean = 9,
  Code = 10,
};

const char* UnitName(Unit unit) noexcept;
Result<Unit> UnitFromName(std::string_view name);

/// A quantity with an explicit unit. Values are signed so that deltas such as
/// thermal margin (measured minus threshold) are representable, and every
/// arithmetic use is overflow checked.
class Scalar {
 public:
  constexpr Scalar() = default;
  constexpr Scalar(Unit unit, std::int64_t value) : unit_(unit), value_(value) {}

  static constexpr Scalar Count(std::int64_t value) { return Scalar(Unit::Count, value); }
  static constexpr Scalar Boolean(bool value) {
    return Scalar(Unit::Boolean, value ? 1 : 0);
  }
  static constexpr Scalar Code(std::int64_t value) { return Scalar(Unit::Code, value); }

  constexpr Unit unit() const noexcept { return unit_; }
  constexpr std::int64_t value() const noexcept { return value_; }

  std::string ToString() const;

  friend constexpr bool operator==(const Scalar& a, const Scalar& b) {
    return a.unit_ == b.unit_ && a.value_ == b.value_;
  }

 private:
  Unit unit_ = Unit::None;
  std::int64_t value_ = 0;
};

/// Ordering of two scalars. Fails with InvalidArgument when the units differ.
Result<int> CompareScalar(const Scalar& a, const Scalar& b);

/// Overflow checked sum of two scalars; requires identical units.
Result<Scalar> AddScalar(const Scalar& a, const Scalar& b);

}  // namespace dom
