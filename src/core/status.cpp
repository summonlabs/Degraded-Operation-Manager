// Degraded Operation Manager - status implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/status.hpp"

namespace dom {

const char* ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "ok";
    case ErrorCode::InvalidArgument:
      return "invalid-argument";
    case ErrorCode::NotFound:
      return "not-found";
    case ErrorCode::AlreadyExists:
      return "already-exists";
    case ErrorCode::Conflict:
      return "conflict";
    case ErrorCode::PreconditionFailed:
      return "precondition-failed";
    case ErrorCode::StaleAuthority:
      return "stale-authority";
    case ErrorCode::Fenced:
      return "fenced";
    case ErrorCode::NotPermitted:
      return "not-permitted";
    case ErrorCode::PolicyRejected:
      return "policy-rejected";
    case ErrorCode::EvidenceUnusable:
      return "evidence-unusable";
    case ErrorCode::Overflow:
      return "overflow";
    case ErrorCode::Underflow:
      return "underflow";
    case ErrorCode::Corrupt:
      return "corrupt";
    case ErrorCode::Truncated:
      return "truncated";
    case ErrorCode::IncompatibleFormat:
      return "incompatible-format";
    case ErrorCode::Locked:
      return "locked";
    case ErrorCode::IoError:
      return "io-error";
    case ErrorCode::DecodeError:
      return "decode-error";
    case ErrorCode::EncodeError:
      return "encode-error";
    case ErrorCode::LimitExceeded:
      return "limit-exceeded";
    case ErrorCode::Unsupported:
      return "unsupported";
    case ErrorCode::Uninitialized:
      return "uninitialized";
    case ErrorCode::Internal:
      return "internal";
  }
  return "unknown";
}

std::string Status::ToString() const {
  if (ok()) {
    return "ok";
  }
  std::string text = ErrorCodeName(code_);
  if (!message_.empty()) {
    text += ": ";
    text += message_;
  }
  return text;
}

}  // namespace dom
