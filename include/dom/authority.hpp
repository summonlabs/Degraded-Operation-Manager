// Degraded Operation Manager - authority, acknowledgement and effect records.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>

#include "dom/decision.hpp"

namespace dom {

/// Operation identity for idempotent replay. Distinct values so a replayed
/// request of one kind can never be mistaken for another kind.
enum class OperationKind : std::uint8_t {
  Evaluate = 1,
  Authorize = 2,
  Acknowledge = 3,
  VerifyEffect = 4,
  ClearLatch = 5,
  AdoptEpoch = 6,
};

const char* OperationKindName(OperationKind kind) noexcept;

/// Permission to apply a restriction set. An authorization is deliberately a
/// separate object from the decision: a decision states what mode governs, an
/// authorization states that restrictions may be applied, for how long, and
/// bound to exactly which decision.
struct RestrictionAuthorization {
  AuthorizationId id;
  DecisionSequence bound_decision;
  Digest decision_digest;
  ControlEpoch epoch;
  Incarnation incarnation;
  Instant issued_at_ms;
  Instant expires_at_ms;
  RestrictionSet restrictions;

  bool IsExpiredAt(Instant now) const;
  Digest digest() const;
  std::string ToText() const;
};

enum class AcceptanceStatus : std::uint8_t {
  Accepted = 0,
  PartiallyAccepted = 1,
  Rejected = 2,
};

const char* AcceptanceStatusName(AcceptanceStatus status) noexcept;
Result<AcceptanceStatus> AcceptanceStatusFromName(std::string_view name);

/// What an adjacent owner reported about an authorization. Acknowledgement is
/// not evidence of effect.
struct AcknowledgementRecord {
  AuthorizationId authorization;
  std::string adjacent_owner;
  AcceptanceStatus status = AcceptanceStatus::Accepted;
  Instant recorded_at_ms;
  Digest decision_digest;
  ControlEpoch epoch;
  std::string detail;

  Digest digest() const;
  std::string ToText() const;
};

enum class VerificationOutcome : std::uint8_t {
  Effective = 0,
  PartiallyEffective = 1,
  Ineffective = 2,
  Unknown = 3,
};

const char* VerificationOutcomeName(VerificationOutcome outcome) noexcept;
Result<VerificationOutcome> VerificationOutcomeFromName(std::string_view name);

/// Independently observed effect of an authorization. This runtime records the
/// observation; it never performs or infers the effect itself.
struct EffectVerificationRecord {
  AuthorizationId authorization;
  std::string adjacent_owner;
  VerificationOutcome outcome = VerificationOutcome::Unknown;
  Instant observed_at_ms;
  Digest effect_digest;
  Digest source_digest;
  /// True when the bound decision is no longer the current one, so the record
  /// is historical rather than current evidence of effect.
  bool stale_binding = false;
  std::string detail;

  Digest digest() const;
  std::string ToText() const;
};

/// Bounded idempotency record: the same request identity returns the same
/// committed result instead of performing a second consequential mutation.
struct IdempotencyEntry {
  Digest key;
  Digest request_digest;
  OperationKind operation = OperationKind::Evaluate;
  StateSequence resulting_sequence;
  Digest result_digest;
  Instant recorded_at_ms;

  friend bool operator==(const IdempotencyEntry& a, const IdempotencyEntry& b) = default;
};

}  // namespace dom
