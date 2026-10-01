// Degraded Operation Manager - status and result types.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dom {

/// Stable, exhaustive failure classification. The public API never throws for
/// expected failure; every fallible operation returns Status or Result<T>.
enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  NotFound = 2,
  AlreadyExists = 3,
  Conflict = 4,
  PreconditionFailed = 5,
  StaleAuthority = 6,
  Fenced = 7,
  NotPermitted = 8,
  PolicyRejected = 9,
  EvidenceUnusable = 10,
  Overflow = 11,
  Underflow = 12,
  Corrupt = 13,
  Truncated = 14,
  IncompatibleFormat = 15,
  Locked = 16,
  IoError = 17,
  DecodeError = 18,
  EncodeError = 19,
  LimitExceeded = 20,
  Unsupported = 21,
  Uninitialized = 22,
  Internal = 23,
};

/// Human readable, stable name of an error code. Used by reason traces, CLI
/// output and tests; never localized.
const char* ErrorCodeName(ErrorCode code) noexcept;

class Status {
 public:
  Status() = default;

  static Status Ok() { return Status(); }
  static Status Error(ErrorCode code, std::string message) {
    return Status(code, std::move(message));
  }

  bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  ErrorCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

  /// "ok" or "code: message".
  std::string ToString() const;

 private:
  Status(ErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
};

/// Value-or-error carrier. A Result either holds a value and an Ok status or no
/// value and the failure status. Calling value() without a value is a contract
/// violation; callers must check has_value() (or operator bool) first.
template <class T>
class Result {
 public:
  Result() = delete;

  static Result Ok(T value) { return Result(Status::Ok(), std::optional<T>(std::move(value))); }
  static Result Err(Status status) { return Result(std::move(status), std::nullopt); }
  static Result Err(ErrorCode code, std::string message) {
    return Result(Status::Error(code, std::move(message)), std::nullopt);
  }

  bool has_value() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  const Status& status() const noexcept { return status_; }

  T& value() & { return *value_; }
  const T& value() const& { return *value_; }
  T&& value() && { return std::move(*value_); }

  T ValueOr(T fallback) const& { return value_.value_or(fallback); }

 private:
  Result(Status status, std::optional<T> value)
      : status_(std::move(status)), value_(std::move(value)) {}

  Status status_;
  std::optional<T> value_;
};

}  // namespace dom
