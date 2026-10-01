// Degraded Operation Manager - facility mode authority runtime.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dom/authority.hpp"
#include "dom/engine.hpp"
#include "dom/policy.hpp"
#include "dom/store.hpp"

namespace dom {

struct CoordinatorOptions {
  std::string directory;
  bool create_if_missing = true;
  PolicyDocument policy;
  /// Incarnation recorded when this process initialises a store that has no
  /// authoritative state yet. It is supplied explicitly; nothing is inferred
  /// from the machine, the path or the process id.
  Incarnation incarnation;
  std::uint32_t history_capacity = 4096;
  std::uint32_t record_capacity = 4096;
  std::uint32_t idempotency_capacity = 1024;
  std::function<void(CommitStage)> commit_observer;
  /// Upper bound on authorization lifetime accepted by Authorize.
  Duration max_authorization_ttl_ms;
};

/// Every request carries an idempotency key, the control epoch it was built
/// against, and the state sequence it expects to be current. The key resolves
/// replay; the epoch and sequence fence stale authority.
struct RequestEnvelope {
  Digest idempotency_key;
  StateSequence expected_state_sequence;
  ControlEpoch expected_epoch;
};

struct EvaluationRequest {
  RequestEnvelope envelope;
  EvaluationContext context;
  EvidenceSnapshot evidence;
};

struct DecisionOutcome {
  ModeDecision decision;
  CommittedView committed;
  RestrictionSet restrictions;
  /// True when the evaluation changed authoritative state and published a new
  /// generation.
  bool state_changed = false;
  bool replayed = false;
  StateSequence state_sequence;
};

struct AuthorizationRequest {
  RequestEnvelope envelope;
  Instant now_ms;
  Duration ttl_ms;
};

struct AuthorizationOutcome {
  RestrictionAuthorization authorization;
  bool replayed = false;
  StateSequence state_sequence;
};

struct AcknowledgementRequest {
  RequestEnvelope envelope;
  AuthorizationId authorization;
  std::string adjacent_owner;
  AcceptanceStatus status = AcceptanceStatus::Accepted;
  Instant recorded_at_ms;
  std::string detail;
};

struct VerificationRequest {
  RequestEnvelope envelope;
  AuthorizationId authorization;
  std::string adjacent_owner;
  VerificationOutcome outcome = VerificationOutcome::Unknown;
  Instant observed_at_ms;
  Digest effect_digest;
  Digest source_digest;
  std::string detail;
};

struct RecordOutcome {
  bool replayed = false;
  bool stale_binding = false;
  StateSequence state_sequence;
  ReasonTrace trace;
};

struct LatchClearRequest {
  RequestEnvelope envelope;
  std::string authority_reference;
  /// Must equal the latch cause digest recorded when the mode was entered.
  Digest latch_cause_digest;
  EvaluationContext context;
  EvidenceSnapshot evidence;
};

struct LatchClearOutcome {
  bool cleared = false;
  bool replayed = false;
  StateSequence state_sequence;
  ReasonTrace trace;
};

struct AdoptEpochRequest {
  /// Epoch the caller believes is currently recorded. Adoption is explicit:
  /// a successor never inherits authority by opening the store.
  ControlEpoch expected_epoch;
  Incarnation new_incarnation;
  std::string authority_reference;
};

struct AdoptEpochOutcome {
  ControlEpoch epoch;
  Incarnation incarnation;
  bool replayed = false;
  StateSequence state_sequence;
};

struct CoordinatorStatus {
  StateSequence state_sequence;
  DecisionSequence decision_sequence;
  ControlEpoch epoch;
  Incarnation incarnation;
  ModeId mode;
  Posture posture = Posture::Nominal;
  Instant since_ms;
  bool latched = false;
  std::uint64_t committed_operations = 0;
  std::uint64_t evaluations = 0;
  std::size_t history_entries = 0;
  std::size_t authorizations = 0;
  std::size_t acknowledgements = 0;
  std::size_t verifications = 0;
  std::size_t idempotency_entries = 0;
  bool epoch_adopted = false;
  std::string ToText() const;
};

/// Facility mode authority. Owns the durable mode state, the evaluation, the
/// authorization to apply restrictions, and the record of what adjacent owners
/// acknowledged and what was independently verified. It owns no actuation.
class Coordinator {
 public:
  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  static Result<std::unique_ptr<Coordinator>> Open(const CoordinatorOptions& options);

  /// Evaluate the current facility posture. A change of mode, posture, recovery
  /// hold or latch publishes exactly one new generation.
  Result<DecisionOutcome> Evaluate(const EvaluationRequest& request);

  /// Issue permission to apply the restrictions of the current decision.
  Result<AuthorizationOutcome> Authorize(const AuthorizationRequest& request);

  /// Record what an adjacent owner reported. Acknowledgement is not effect.
  Result<RecordOutcome> RecordAcknowledgement(const AcknowledgementRequest& request);

  /// Record an independently observed effect. A record bound to a superseded
  /// decision is stored as historical and flagged stale.
  Result<RecordOutcome> RecordEffectVerification(const VerificationRequest& request);

  /// Leave a latched mode. Requires the latch cause digest, an authority
  /// reference, and evidence that the recovery requirements are fresh and the
  /// latched mode's exit criteria hold.
  Result<LatchClearOutcome> ClearLatch(const LatchClearRequest& request);

  /// Explicitly take over mutation authority with a new epoch and incarnation.
  Result<AdoptEpochOutcome> AdoptEpoch(const AdoptEpochRequest& request);

  /// Current authority status. Named GetStatus because a member function named
  /// Status would hide the Status type inside the class scope.
  Result<CoordinatorStatus> GetStatus() const;
  Result<std::vector<HistoryEntry>> History(std::size_t limit) const;
  Result<std::vector<RestrictionAuthorization>> Authorizations(std::size_t limit) const;

  const PolicyDocument& policy() const noexcept { return policy_; }
  /// The committed state. The reference stays valid until the next mutating
  /// call on this coordinator, which republishes it; copy anything that must
  /// outlive a mutation.
  const PersistedState& state() const noexcept { return store_->state(); }
  const RecoveryReport& recovery() const noexcept { return store_->recovery(); }
  /// Real durable publications since this process opened the store, and the
  /// bytes those publications wrote. These count completed durable work, not
  /// submissions.
  std::uint64_t PublishedGenerations() const noexcept {
    return store_->committed_generations();
  }
  std::uint64_t PublishedBytes() const noexcept { return store_->committed_bytes(); }

 private:
  Coordinator() = default;

  /// Idempotent replay resolution. Returns Ok(nullptr) when the key is new,
  /// Ok(entry) when it identifies the same already-committed request, and an
  /// error when the key was reused for a different request.
  Result<IdempotencyEntry*> LookupReplay(const Digest& key, const Digest& request_digest,
                                         OperationKind operation);
  Status EnforceEnvelope(const RequestEnvelope& envelope) const;
  /// Publishes one generation after enforcing the configured bounds.
  Status Commit(PersistedState next);
  /// Restriction set in force for the current committed posture.
  RestrictionSet CurrentRestrictions() const;
  void TrimBounded(PersistedState& state) const;

  std::unique_ptr<StateStore> store_;
  PolicyDocument policy_;
  Digest policy_digest_;
  Duration max_authorization_ttl_ms_;
  std::uint32_t history_capacity_ = 4096;
  std::uint32_t record_capacity_ = 4096;
  std::uint32_t idempotency_capacity_ = 1024;
  mutable std::mutex mutex_;
  bool epoch_adopted_ = false;
  std::atomic<std::uint64_t> committed_operations_{0};
};

}  // namespace dom
