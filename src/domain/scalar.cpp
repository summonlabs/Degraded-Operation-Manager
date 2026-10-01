// Degraded Operation Manager - typed scalar quantities.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/scalar.hpp"

#include "dom/ids.hpp"

namespace dom {

const char* UnitName(Unit unit) noexcept {
  switch (unit) {
    case Unit::None:
      return "none";
    case Unit::Count:
      return "count";
    case Unit::MilliPercent:
      return "milli-percent";
    case Unit::MilliCelsius:
      return "milli-celsius";
    case Unit::MilliRatio:
      return "milli-ratio";
    case Unit::Milliseconds:
      return "milliseconds";
    case Unit::Watts:
      return "watts";
    case Unit::Millivolts:
      return "millivolts";
    case Unit::Milliamperes:
      return "milliamperes";
    case Unit::Boolean:
      return "boolean";
    case Unit::Code:
      return "code";
  }
  return "unknown";
}

Result<Unit> UnitFromName(std::string_view name) {
  for (std::uint8_t code = 0; code <= static_cast<std::uint8_t>(Unit::Code); ++code) {
    const auto unit = static_cast<Unit>(code);
    if (name == UnitName(unit)) {
      return Result<Unit>::Ok(unit);
    }
  }
  return Result<Unit>::Err(ErrorCode::InvalidArgument, "unknown unit name");
}

std::string Scalar::ToString() const {
  return std::string(UnitName(unit_)) + ":" + std::to_string(value_);
}

Result<int> CompareScalar(const Scalar& a, const Scalar& b) {
  if (a.unit() != b.unit()) {
    return Result<int>::Err(ErrorCode::InvalidArgument,
                            "scalars have different units and are not comparable");
  }
  if (a.value() < b.value()) {
    return Result<int>::Ok(-1);
  }
  if (a.value() > b.value()) {
    return Result<int>::Ok(1);
  }
  return Result<int>::Ok(0);
}

Result<Scalar> AddScalar(const Scalar& a, const Scalar& b) {
  if (a.unit() != b.unit()) {
    return Result<Scalar>::Err(ErrorCode::InvalidArgument,
                               "scalars have different units and cannot be added");
  }
  auto sum = CheckedAddSigned(a.value(), b.value());
  if (!sum) {
    return Result<Scalar>::Err(sum.status());
  }
  return Result<Scalar>::Ok(Scalar(a.unit(), sum.value()));
}

}  // namespace dom
