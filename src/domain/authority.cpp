// Degraded Operation Manager - authority vocabulary and rendering.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/authority.hpp"

#include <string>

namespace dom {

const char* OperationKindName(OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::Evaluate:
      return "evaluate";
    case OperationKind::Authorize:
      return "authorize";
    case OperationKind::Acknowledge:
      return "acknowledge";
    case OperationKind::VerifyEffect:
      return "verify-effect";
    case OperationKind::ClearLatch:
      return "clear-latch";
    case OperationKind::AdoptEpoch:
      return "adopt-epoch";
  }
  return "unknown";
}

const char* AcceptanceStatusName(AcceptanceStatus status) noexcept {
  switch (status) {
    case AcceptanceStatus::Accepted:
      return "accepted";
    case AcceptanceStatus::PartiallyAccepted:
      return "partially-accepted";
    case AcceptanceStatus::Rejected:
      return "rejected";
  }
  return "unknown";
}

Result<AcceptanceStatus> AcceptanceStatusFromName(std::string_view name) {
  for (std::uint8_t code = 0; code <= 2; ++code) {
    const auto status = static_cast<AcceptanceStatus>(code);
    if (name == AcceptanceStatusName(status)) {
      return Result<AcceptanceStatus>::Ok(status);
    }
  }
  return Result<AcceptanceStatus>::Err(ErrorCode::InvalidArgument,
                                       "unknown acceptance status name");
}

const char* VerificationOutcomeName(VerificationOutcome outcome) noexcept {
  switch (outcome) {
    case VerificationOutcome::Effective:
      return "effective";
    case VerificationOutcome::PartiallyEffective:
      return "partially-effective";
    case VerificationOutcome::Ineffective:
      return "ineffective";
    case VerificationOutcome::Unknown:
      return "unknown";
  }
  return "unknown";
}

Result<VerificationOutcome> VerificationOutcomeFromName(std::string_view name) {
  for (std::uint8_t code = 0; code <= 3; ++code) {
    const auto outcome = static_cast<VerificationOutcome>(code);
    if (name == VerificationOutcomeName(outcome)) {
      return Result<VerificationOutcome>::Ok(outcome);
    }
  }
  return Result<VerificationOutcome>::Err(ErrorCode::InvalidArgument,
                                          "unknown verification outcome name");
}

bool RestrictionAuthorization::IsExpiredAt(Instant now) const {
  return now > expires_at_ms;
}

std::string RestrictionAuthorization::ToText() const {
  std::string text;
  text += "authorization id=" + id.ToString();
  text += " bound-decision=" + bound_decision.ToString();
  text += " decision-digest=" + decision_digest.ToHex();
  text += " epoch=" + epoch.ToString();
  text += " incarnation=" + incarnation.ToString();
  text += " issued=" + issued_at_ms.ToString();
  text += " expires=" + expires_at_ms.ToString();
  text += "\n";
  text += restrictions.ToText();
  return text;
}

std::string AcknowledgementRecord::ToText() const {
  std::string text;
  text += "acknowledgement authorization=" + authorization.ToString();
  text += " owner=\"" + adjacent_owner + "\"";
  text += " status=" + std::string(AcceptanceStatusName(status));
  text += " recorded=" + recorded_at_ms.ToString();
  text += " epoch=" + epoch.ToString();
  if (!detail.empty()) {
    text += " detail=\"" + detail + "\"";
  }
  text += "\n";
  return text;
}

std::string EffectVerificationRecord::ToText() const {
  std::string text;
  text += "verification authorization=" + authorization.ToString();
  text += " owner=\"" + adjacent_owner + "\"";
  text += " outcome=" + std::string(VerificationOutcomeName(outcome));
  text += " observed=" + observed_at_ms.ToString();
  text += " effect-digest=" + effect_digest.ToHex();
  text += " source-digest=" + source_digest.ToHex();
  text += stale_binding ? " stale-binding=true" : " stale-binding=false";
  if (!detail.empty()) {
    text += " detail=\"" + detail + "\"";
  }
  text += "\n";
  return text;
}

}  // namespace dom
