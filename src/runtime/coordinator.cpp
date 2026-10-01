// Degraded Operation Manager - facility mode authority runtime.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The coordinator owns exactly one durable mode authority. Every mutation:
//   * resolves an idempotent replay before it evaluates any precondition, so a
//     lost response can never cause a second consequential mutation;
//   * fences stale authority by control epoch, incarnation and expected state
//     sequence;
//   * publishes exactly one generation when, and only when, authoritative state
//     changes.
// Concurrency: every public method takes the coordinator mutex once and calls
// only private helpers that assume it is held. Nothing calls back into the
// coordinator while the mutex is held; the only callback is the store's commit
// stage observer, which is documented as instrumentation and cannot re-enter.
//
// Manual deadlock and lock-reentrancy audit of every call path in this file:
//  * read-then-write reacquisition of one lock: none. Commit(), LookupReplay(),
//    EnforceEnvelope(), CurrentRestrictions() and TrimBounded() never lock, and
//    they are the only functions called while the mutex is held.
//  * write-lock re-entry: impossible; std::mutex is not recursive and no path
//    calls a public method from a locked one.
//  * callbacks while a lock is held: only StateStore::Commit's stage observer,
//    invoked with the coordinator mutex held on the committing thread. It is
//    instrumentation: the store refuses a re-entrant commit with an internal
//    error (guarded by StateStore::committing_), an exception escaping the
//    observer is swallowed, and no coordinator method is reachable from it.
//  * nested acquisition and lock ordering: the only other lock is the store's
//    process-lifetime file lock, acquired in StateStore::Open before any
//    coordinator mutex exists, so there is no ordering to invert.
//  * joining workers while holding state: there are no worker threads.
//  * shutdown waiting on work: the destructor releases the store lock and waits
//    for nothing.
//  * cancellation with reversed lock order: no cancellation path exists.
//  * helper reacquiring a held mutex: none, as listed above.
//  * references outliving republished state: private code copies an
//    authorization id out of the state before it publishes, GetStatus, History
//    and Authorizations return values, and the idempotency pointer returned by
//    LookupReplay is dereferenced before any publish. Coordinator::state()
//    returns a reference that is invalidated by the next mutation, which the
//    header documents.

#include "dom/runtime.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "codec/binary_writer.hpp"
#include "dom/limits.hpp"

namespace dom {
namespace {

Digest RequestDigest(OperationKind operation, const RequestEnvelope& envelope, Instant now_ms,
                     Tick tick, const Digest& payload_a, const Digest& payload_b,
                     std::uint64_t extra) {
  internal::BinaryWriter writer;
  Status status = writer.PutU8(static_cast<std::uint8_t>(operation));
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(envelope.expected_epoch.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(envelope.expected_state_sequence.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(now_ms.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(tick.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutDigest(payload_a);
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutDigest(payload_b);
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(extra);
  if (!status.ok()) {
    return Digest::Zero();
  }
  return Sha256::Of(writer.bytes());
}

}  // namespace

std::string CoordinatorStatus::ToText() const {
  std::string text;
  text += "state-sequence " + state_sequence.ToString() + "\n";
  text += "decision-sequence " + decision_sequence.ToString() + "\n";
  text += "epoch " + epoch.ToString() + "\n";
  text += "incarnation " + incarnation.ToString() + "\n";
  text += "mode " + mode.ToString() + "\n";
  text += "posture " + std::string(PostureName(posture)) + "\n";
  text += "since-ms " + since_ms.ToString() + "\n";
  text += std::string("latched ") + (latched ? "true" : "false") + "\n";
  text += "committed-operations " + std::to_string(committed_operations) + "\n";
  text += "published-evaluations " + std::to_string(evaluations) + "\n";
  text += "history-entries " + std::to_string(history_entries) + "\n";
  text += "authorizations " + std::to_string(authorizations) + "\n";
  text += "acknowledgements " + std::to_string(acknowledgements) + "\n";
  text += "verifications " + std::to_string(verifications) + "\n";
  text += "idempotency-entries " + std::to_string(idempotency_entries) + "\n";
  text += std::string("epoch-adopted-in-this-process ") + (epoch_adopted ? "true" : "false") +
          "\n";
  return text;
}

Coordinator::~Coordinator() = default;

Result<std::unique_ptr<Coordinator>> Coordinator::Open(const CoordinatorOptions& options) {
  Status validation = ValidatePolicy(options.policy);
  if (!validation.ok()) {
    return Result<std::unique_ptr<Coordinator>>::Err(validation);
  }
  if (options.max_authorization_ttl_ms.value() == 0) {
    return Result<std::unique_ptr<Coordinator>>::Err(
        ErrorCode::InvalidArgument, "the maximum authorization lifetime must be non-zero");
  }
  StoreOptions store_options;
  store_options.directory = options.directory;
  store_options.create_if_missing = options.create_if_missing;
  store_options.history_capacity = options.history_capacity;
  store_options.record_capacity = options.record_capacity;
  store_options.idempotency_capacity = options.idempotency_capacity;
  store_options.commit_observer = options.commit_observer;

  auto store = StateStore::Open(store_options);
  if (!store) {
    return Result<std::unique_ptr<Coordinator>>::Err(store.status());
  }

  std::unique_ptr<Coordinator> coordinator(new Coordinator());
  coordinator->store_ = std::move(store.value());
  coordinator->policy_ = options.policy;
  coordinator->policy_digest_ = PolicyDigest(coordinator->policy_);
  coordinator->max_authorization_ttl_ms_ = options.max_authorization_ttl_ms;
  coordinator->history_capacity_ = options.history_capacity;
  coordinator->record_capacity_ = options.record_capacity;
  coordinator->idempotency_capacity_ = options.idempotency_capacity;

  const PersistedState& state = coordinator->store_->state();
  if (state.committed.mode.IsZero()) {
    // A store with no authoritative state yet: this process establishes the
    // nominal mode and control epoch 1 explicitly.
    if (!options.incarnation.IsValid()) {
      return Result<std::unique_ptr<Coordinator>>::Err(
          ErrorCode::InvalidArgument,
          "initialising a fresh store requires an explicit non-zero incarnation");
    }
    const ModeDefinition* nominal = FindFirstModeOfClass(coordinator->policy_,
                                                         OperatingClass::Nominal);
    if (nominal == nullptr) {
      return Result<std::unique_ptr<Coordinator>>::Err(
          ErrorCode::PolicyRejected, "the policy has no nominal mode to initialise from");
    }
    PersistedState next = state;
    next.committed.mode = nominal->id;
    next.committed.posture = Posture::Nominal;
    next.committed.epoch = ControlEpoch::FromValue(1);
    next.committed.incarnation = options.incarnation;
    next.committed.since_ms = Instant();
    next.committed.since_tick = Tick();
    next.committed.decision_sequence = DecisionSequence();
    next.committed.last_decision_digest = Digest::Zero();
    auto next_sequence = state.sequence.Next();
    if (!next_sequence) {
      return Result<std::unique_ptr<Coordinator>>::Err(next_sequence.status());
    }
    next.committed.state_sequence = next_sequence.value();
    ReasonTrace trace;
    Reason reason;
    reason.code = ReasonCode::StateRecovered;
    reason.mode = nominal->id;
    reason.detail = "initial authority established at control epoch 1";
    trace.Add(std::move(reason));
    trace.Canonicalize();
    next.last_decision = ModeDecision();
    next.last_decision.mode = nominal->id;
    next.last_decision.posture = Posture::Nominal;
    next.last_decision.verdict = Verdict::Hold;
    next.last_decision.generations.epoch = next.committed.epoch;
    next.last_decision.generations.incarnation = options.incarnation;
    next.last_decision.generations.policy = coordinator->policy_.generation;
    next.last_decision.policy_digest = coordinator->policy_digest_;
    next.last_decision.trace = trace;
    auto committed = coordinator->store_->Commit(
        std::move(next), CommitOptions{state.sequence});
    if (!committed) {
      return Result<std::unique_ptr<Coordinator>>::Err(committed.status());
    }
    coordinator->epoch_adopted_ = true;
  }
  return Result<std::unique_ptr<Coordinator>>::Ok(std::move(coordinator));
}

Result<IdempotencyEntry*> Coordinator::LookupReplay(const Digest& key,
                                                    const Digest& request_digest,
                                                    OperationKind operation) {
  const PersistedState& state = store_->state();
  for (const IdempotencyEntry& entry : state.idempotency) {
    if (!(entry.key == key)) {
      continue;
    }
    if (entry.operation != operation || !(entry.request_digest == request_digest)) {
      return Result<IdempotencyEntry*>::Err(
          ErrorCode::Conflict,
          "the idempotency key was already used for a different request");
    }
    return Result<IdempotencyEntry*>::Ok(const_cast<IdempotencyEntry*>(&entry));
  }
  return Result<IdempotencyEntry*>::Ok(nullptr);
}

Status Coordinator::EnforceEnvelope(const RequestEnvelope& envelope) const {
  if (envelope.idempotency_key.IsZero()) {
    return Status::Error(ErrorCode::InvalidArgument, "an idempotency key is required");
  }
  const PersistedState& state = store_->state();
  if (!epoch_adopted_) {
    return Status::Error(
        ErrorCode::StaleAuthority,
        "this process has not adopted mutation authority for the current control epoch");
  }
  if (envelope.expected_epoch != state.committed.epoch) {
    return Status::Error(ErrorCode::StaleAuthority,
                         "request epoch " + envelope.expected_epoch.ToString() +
                             " is not the current control epoch " +
                             state.committed.epoch.ToString());
  }
  if (envelope.expected_state_sequence != state.sequence) {
    return Status::Error(ErrorCode::PreconditionFailed,
                         "request expects state sequence " +
                             envelope.expected_state_sequence.ToString() +
                             " but the published sequence is " + state.sequence.ToString());
  }
  return Status::Ok();
}

Status Coordinator::Commit(PersistedState next) {
  TrimBounded(next);
  auto committed = store_->Commit(std::move(next), CommitOptions{store_->state().sequence});
  if (!committed) {
    return committed.status();
  }
  ++committed_operations_;
  return Status::Ok();
}

Result<DecisionOutcome> Coordinator::Evaluate(const EvaluationRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Digest request_digest = RequestDigest(
      OperationKind::Evaluate, request.envelope, request.context.now_ms, request.context.tick,
      request.evidence.digest(), Digest::Zero(), request.evidence.revision().value());
  auto replay = LookupReplay(request.envelope.idempotency_key, request_digest,
                             OperationKind::Evaluate);
  if (!replay) {
    return Result<DecisionOutcome>::Err(replay.status());
  }
  if (replay.value() != nullptr) {
    DecisionOutcome outcome;
    const PersistedState& state = store_->state();
    outcome.decision = state.last_decision;
    outcome.committed = state.committed;
    outcome.restrictions = CurrentRestrictions();
    outcome.state_changed = false;
    outcome.replayed = true;
    outcome.state_sequence = replay.value()->resulting_sequence;
    return Result<DecisionOutcome>::Ok(std::move(outcome));
  }
  Status envelope = EnforceEnvelope(request.envelope);
  if (!envelope.ok()) {
    return Result<DecisionOutcome>::Err(envelope);
  }

  EvaluationInputs inputs;
  inputs.policy = &policy_;
  inputs.evidence = &request.evidence;
  inputs.committed = store_->state().committed;
  inputs.context = request.context;
  inputs.policy_validated = true;
  auto evaluated = EvaluateMode(inputs);
  if (!evaluated) {
    return Result<DecisionOutcome>::Err(evaluated.status());
  }
  EvaluationOutcome& evaluation = evaluated.value();
  DecisionOutcome outcome;
  outcome.restrictions = evaluation.restrictions;
  outcome.committed = evaluation.next;
  outcome.decision = evaluation.decision;
  outcome.replayed = false;

  if (!evaluation.changed) {
    // Nothing authoritative changed: no generation is published, and the
    // decision is an uncommitted evaluation result with sequence zero.
    outcome.state_changed = false;
    outcome.state_sequence = store_->state().sequence;
    outcome.decision.sequence = DecisionSequence();
    return Result<DecisionOutcome>::Ok(std::move(outcome));
  }

  PersistedState next = store_->state();
  auto next_sequence = next.sequence.Next();
  if (!next_sequence) {
    return Result<DecisionOutcome>::Err(next_sequence.status());
  }
  auto next_decision_sequence = next.committed.decision_sequence.Next();
  if (!next_decision_sequence) {
    return Result<DecisionOutcome>::Err(next_decision_sequence.status());
  }
  ModeDecision decision = evaluation.decision;
  decision.sequence = next_decision_sequence.value();
  next.committed = evaluation.next;
  next.committed.state_sequence = next_sequence.value();
  next.committed.decision_sequence = next_decision_sequence.value();
  next.committed.last_decision_digest = decision.digest();
  if (next.committed.latch.latched && next.committed.latch.cause_digest.IsZero()) {
    next.committed.latch.cause_digest = next.committed.last_decision_digest;
  }
  next.last_decision = decision;
  ++next.evaluations;
  ++next.committed_operations;

  HistoryEntry history;
  history.state_sequence = next_sequence.value();
  history.decision_sequence = next_decision_sequence.value();
  history.mode = decision.mode;
  history.posture = decision.posture;
  history.verdict = decision.verdict;
  history.previous_mode = store_->state().committed.mode;
  history.previous_posture = store_->state().committed.posture;
  history.decided_at_ms = decision.decided_at_ms;
  history.decision_digest = decision.digest();
  history.trace_digest = decision.trace.digest();
  next.history.push_back(history);

  IdempotencyEntry entry;
  entry.key = request.envelope.idempotency_key;
  entry.request_digest = request_digest;
  entry.operation = OperationKind::Evaluate;
  entry.resulting_sequence = next_sequence.value();
  entry.result_digest = decision.digest();
  entry.recorded_at_ms = request.context.now_ms;
  next.idempotency.push_back(entry);

  Status committed = Commit(std::move(next));
  if (!committed.ok()) {
    return Result<DecisionOutcome>::Err(committed);
  }
  outcome.state_changed = true;
  outcome.state_sequence = store_->state().sequence;
  outcome.committed = store_->state().committed;
  outcome.decision = store_->state().last_decision;
  return Result<DecisionOutcome>::Ok(std::move(outcome));
}

RestrictionSet Coordinator::CurrentRestrictions() const {
  const PersistedState& state = store_->state();
  const auto mode = FindMode(policy_, state.committed.mode);
  if (!mode) {
    return RestrictionSet();
  }
  RestrictionSet restrictions = RestrictionsForMode(policy_, *mode.value());
  if (IsIndeterminate(state.committed.posture)) {
    RestrictionSet extra;
    extra.rules = policy_.indeterminate_restrictions;
    std::sort(extra.rules.begin(), extra.rules.end(),
              [](const RestrictionRule& a, const RestrictionRule& b) { return a.id < b.id; });
    for (const RestrictionRule& rule : extra.rules) {
      extra.restricted_services.push_back(rule.service_class);
    }
    std::sort(extra.restricted_services.begin(), extra.restricted_services.end());
    extra.restricted_services.erase(
        std::unique(extra.restricted_services.begin(), extra.restricted_services.end()),
        extra.restricted_services.end());
    for (const std::string& service : policy_.service_classes) {
      if (std::find(extra.restricted_services.begin(), extra.restricted_services.end(),
                    service) == extra.restricted_services.end()) {
        extra.permitted_services.push_back(service);
      }
    }
    restrictions = UnionRestrictions(policy_, restrictions, extra);
  }
  return restrictions;
}

Result<AuthorizationOutcome> Coordinator::Authorize(const AuthorizationRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Digest request_digest =
      RequestDigest(OperationKind::Authorize, request.envelope, request.now_ms, Tick(),
                    Digest::Zero(), Digest::Zero(), request.ttl_ms.value());
  auto replay = LookupReplay(request.envelope.idempotency_key, request_digest,
                             OperationKind::Authorize);
  if (!replay) {
    return Result<AuthorizationOutcome>::Err(replay.status());
  }
  if (replay.value() != nullptr) {
    AuthorizationOutcome outcome;
    const PersistedState& state = store_->state();
    if (!state.authorizations.empty()) {
      outcome.authorization = state.authorizations.back();
    }
    outcome.replayed = true;
    outcome.state_sequence = replay.value()->resulting_sequence;
    return Result<AuthorizationOutcome>::Ok(std::move(outcome));
  }
  Status envelope = EnforceEnvelope(request.envelope);
  if (!envelope.ok()) {
    return Result<AuthorizationOutcome>::Err(envelope);
  }
  if (request.ttl_ms.value() == 0) {
    return Result<AuthorizationOutcome>::Err(ErrorCode::InvalidArgument,
                                             "authorization lifetime must be non-zero");
  }
  if (request.ttl_ms > max_authorization_ttl_ms_) {
    return Result<AuthorizationOutcome>::Err(
        ErrorCode::InvalidArgument,
        "authorization lifetime exceeds the configured maximum of " +
            max_authorization_ttl_ms_.ToString() + "ms");
  }
  PersistedState next = store_->state();
  if (next.last_decision.sequence.IsZero()) {
    return Result<AuthorizationOutcome>::Err(
        ErrorCode::PreconditionFailed,
        "no committed mode decision exists, so no restriction may be authorized");
  }
  auto expires = Advance(request.now_ms, request.ttl_ms);
  if (!expires) {
    return Result<AuthorizationOutcome>::Err(expires.status());
  }
  AuthorizationId identifier = AuthorizationId::FromValue(1);
  for (const RestrictionAuthorization& existing : next.authorizations) {
    if (!(existing.id < identifier)) {
      auto next_id = existing.id.Next();
      if (!next_id) {
        return Result<AuthorizationOutcome>::Err(next_id.status());
      }
      identifier = next_id.value();
    }
  }
  RestrictionAuthorization authorization;
  authorization.id = identifier;
  authorization.bound_decision = next.last_decision.sequence;
  authorization.decision_digest = next.last_decision.digest();
  authorization.epoch = next.committed.epoch;
  authorization.incarnation = next.committed.incarnation;
  authorization.issued_at_ms = request.now_ms;
  authorization.expires_at_ms = expires.value();
  authorization.restrictions = CurrentRestrictions();

  AuthorizationOutcome outcome;
  outcome.authorization = authorization;
  outcome.replayed = false;

  IdempotencyEntry entry;
  entry.key = request.envelope.idempotency_key;
  entry.request_digest = request_digest;
  entry.operation = OperationKind::Authorize;
  auto next_sequence = next.sequence.Next();
  if (!next_sequence) {
    return Result<AuthorizationOutcome>::Err(next_sequence.status());
  }
  entry.resulting_sequence = next_sequence.value();
  entry.result_digest = authorization.digest();
  entry.recorded_at_ms = request.now_ms;

  next.authorizations.push_back(authorization);
  next.idempotency.push_back(entry);
  ++next.committed_operations;
  Status committed = Commit(std::move(next));
  if (!committed.ok()) {
    return Result<AuthorizationOutcome>::Err(committed);
  }
  outcome.state_sequence = store_->state().sequence;
  return Result<AuthorizationOutcome>::Ok(std::move(outcome));
}

Result<RecordOutcome> Coordinator::RecordAcknowledgement(
    const AcknowledgementRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Digest request_digest = RequestDigest(
      OperationKind::Acknowledge, request.envelope, request.recorded_at_ms, Tick(),
      Digest::Zero(), Digest::Zero(),
      static_cast<std::uint64_t>(request.status));
  auto replay = LookupReplay(request.envelope.idempotency_key, request_digest,
                             OperationKind::Acknowledge);
  if (!replay) {
    return Result<RecordOutcome>::Err(replay.status());
  }
  if (replay.value() != nullptr) {
    RecordOutcome outcome;
    outcome.replayed = true;
    outcome.state_sequence = replay.value()->resulting_sequence;
    return Result<RecordOutcome>::Ok(std::move(outcome));
  }
  Status envelope = EnforceEnvelope(request.envelope);
  if (!envelope.ok()) {
    return Result<RecordOutcome>::Err(envelope);
  }
  if (request.adjacent_owner.empty()) {
    return Result<RecordOutcome>::Err(ErrorCode::InvalidArgument,
                                      "an acknowledgement must name the adjacent owner");
  }
  PersistedState next = store_->state();
  const RestrictionAuthorization* authorization = nullptr;
  for (const RestrictionAuthorization& candidate : next.authorizations) {
    if (candidate.id == request.authorization) {
      authorization = &candidate;
    }
  }
  if (authorization == nullptr) {
    return Result<RecordOutcome>::Err(ErrorCode::NotFound,
                                      "the acknowledgement names an unknown authorization");
  }
  if (authorization->IsExpiredAt(request.recorded_at_ms)) {
    return Result<RecordOutcome>::Err(
        ErrorCode::NotPermitted,
        "the authorization expired before the acknowledgement was recorded");
  }
  // Copied out before anything is published: the reference into the current
  // state must not outlive the publication below.
  const AuthorizationId authorization_id = authorization->id;
  const Digest bound_decision_digest = authorization->decision_digest;
  AcknowledgementRecord record;
  record.authorization = authorization_id;
  record.adjacent_owner = request.adjacent_owner;
  record.status = request.status;
  record.recorded_at_ms = request.recorded_at_ms;
  record.decision_digest = bound_decision_digest;
  record.epoch = next.committed.epoch;
  record.detail = request.detail;

  RecordOutcome outcome;
  outcome.replayed = false;
  ReasonTrace trace;
  Reason reason;
  reason.code = ReasonCode::AcknowledgementRecorded;
  reason.detail = "acknowledgement recorded for authorization " + authorization_id.ToString();
  trace.Add(std::move(reason));
  trace.Canonicalize();
  outcome.trace = trace;

  IdempotencyEntry entry;
  entry.key = request.envelope.idempotency_key;
  entry.request_digest = request_digest;
  entry.operation = OperationKind::Acknowledge;
  auto next_sequence = next.sequence.Next();
  if (!next_sequence) {
    return Result<RecordOutcome>::Err(next_sequence.status());
  }
  entry.resulting_sequence = next_sequence.value();
  entry.result_digest = record.digest();
  entry.recorded_at_ms = request.recorded_at_ms;

  next.acknowledgements.push_back(record);
  next.idempotency.push_back(entry);
  ++next.committed_operations;
  Status committed = Commit(std::move(next));
  if (!committed.ok()) {
    return Result<RecordOutcome>::Err(committed);
  }
  outcome.state_sequence = store_->state().sequence;
  return Result<RecordOutcome>::Ok(std::move(outcome));
}

Result<RecordOutcome> Coordinator::RecordEffectVerification(const VerificationRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Digest request_digest = RequestDigest(
      OperationKind::VerifyEffect, request.envelope, request.observed_at_ms, Tick(),
      request.effect_digest, request.source_digest,
      static_cast<std::uint64_t>(request.outcome));
  auto replay = LookupReplay(request.envelope.idempotency_key, request_digest,
                             OperationKind::VerifyEffect);
  if (!replay) {
    return Result<RecordOutcome>::Err(replay.status());
  }
  if (replay.value() != nullptr) {
    RecordOutcome outcome;
    outcome.replayed = true;
    outcome.state_sequence = replay.value()->resulting_sequence;
    return Result<RecordOutcome>::Ok(std::move(outcome));
  }
  Status envelope = EnforceEnvelope(request.envelope);
  if (!envelope.ok()) {
    return Result<RecordOutcome>::Err(envelope);
  }
  if (request.adjacent_owner.empty()) {
    return Result<RecordOutcome>::Err(ErrorCode::InvalidArgument,
                                      "a verification must name the observing owner");
  }
  PersistedState next = store_->state();
  const RestrictionAuthorization* authorization = nullptr;
  for (const RestrictionAuthorization& candidate : next.authorizations) {
    if (candidate.id == request.authorization) {
      authorization = &candidate;
    }
  }
  if (authorization == nullptr) {
    return Result<RecordOutcome>::Err(ErrorCode::NotFound,
                                      "the verification names an unknown authorization");
  }
  // Read out before publication; see the note in RecordAcknowledgement.
  const AuthorizationId authorization_id = authorization->id;
  const bool stale_binding =
      authorization->bound_decision != next.last_decision.sequence ||
      authorization->epoch != next.committed.epoch;
  EffectVerificationRecord record;
  record.authorization = authorization_id;
  record.adjacent_owner = request.adjacent_owner;
  record.outcome = request.outcome;
  record.observed_at_ms = request.observed_at_ms;
  record.effect_digest = request.effect_digest;
  record.source_digest = request.source_digest;
  record.stale_binding = stale_binding;
  record.detail = request.detail;

  RecordOutcome outcome;
  outcome.replayed = false;
  outcome.stale_binding = record.stale_binding;
  ReasonTrace trace;
  Reason reason;
  reason.code = record.stale_binding ? ReasonCode::DecisionFenced : ReasonCode::EffectVerified;
  reason.detail = record.stale_binding
                      ? "the bound decision is no longer current; the record is historical"
                      : "verified effect recorded for the current decision";
  trace.Add(std::move(reason));
  trace.Canonicalize();
  outcome.trace = trace;

  IdempotencyEntry entry;
  entry.key = request.envelope.idempotency_key;
  entry.request_digest = request_digest;
  entry.operation = OperationKind::VerifyEffect;
  auto next_sequence = next.sequence.Next();
  if (!next_sequence) {
    return Result<RecordOutcome>::Err(next_sequence.status());
  }
  entry.resulting_sequence = next_sequence.value();
  entry.result_digest = record.digest();
  entry.recorded_at_ms = request.observed_at_ms;

  next.verifications.push_back(record);
  next.idempotency.push_back(entry);
  ++next.committed_operations;
  Status committed = Commit(std::move(next));
  if (!committed.ok()) {
    return Result<RecordOutcome>::Err(committed);
  }
  outcome.state_sequence = store_->state().sequence;
  return Result<RecordOutcome>::Ok(std::move(outcome));
}

Result<LatchClearOutcome> Coordinator::ClearLatch(const LatchClearRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Digest request_digest =
      RequestDigest(OperationKind::ClearLatch, request.envelope, request.context.now_ms,
                    request.context.tick, request.evidence.digest(), request.latch_cause_digest,
                    request.evidence.revision().value());
  auto replay = LookupReplay(request.envelope.idempotency_key, request_digest,
                             OperationKind::ClearLatch);
  if (!replay) {
    return Result<LatchClearOutcome>::Err(replay.status());
  }
  if (replay.value() != nullptr) {
    LatchClearOutcome outcome;
    outcome.replayed = true;
    outcome.cleared = false;
    outcome.state_sequence = replay.value()->resulting_sequence;
    return Result<LatchClearOutcome>::Ok(std::move(outcome));
  }
  Status envelope = EnforceEnvelope(request.envelope);
  if (!envelope.ok()) {
    return Result<LatchClearOutcome>::Err(envelope);
  }
  const PersistedState& state = store_->state();
  if (!state.committed.latch.latched) {
    return Result<LatchClearOutcome>::Err(ErrorCode::PreconditionFailed,
                                          "the current mode is not latched");
  }
  if (request.latch_cause_digest.IsZero() ||
      !(request.latch_cause_digest == state.committed.latch.cause_digest)) {
    return Result<LatchClearOutcome>::Err(
        ErrorCode::PreconditionFailed,
        "the latch cause digest does not match the decision that latched the mode");
  }
  if (request.authority_reference.empty()) {
    return Result<LatchClearOutcome>::Err(
        ErrorCode::InvalidArgument,
        "clearing a latch requires the authority reference that authorises it");
  }

  EvaluationInputs inputs;
  inputs.policy = &policy_;
  inputs.evidence = &request.evidence;
  inputs.committed = state.committed;
  inputs.context = request.context;
  inputs.policy_validated = true;
  inputs.latch_clear_requested = true;
  auto evaluated = EvaluateMode(inputs);
  if (!evaluated) {
    return Result<LatchClearOutcome>::Err(evaluated.status());
  }
  EvaluationOutcome& evaluation = evaluated.value();
  LatchClearOutcome outcome;
  outcome.trace = evaluation.decision.trace;
  if (!evaluation.recovery_preconditions_ok) {
    return Result<LatchClearOutcome>::Err(Status::Error(
        ErrorCode::NotPermitted,
        "the latch stays in force: recovery preconditions do not hold on the supplied "
        "evidence\n" +
            evaluation.decision.trace.ToText()));
  }

  PersistedState next = state;
  next.committed.latch = LatchState();
  next.committed.has_recovery_hold = evaluation.next.has_recovery_hold;
  next.committed.recovery_hold_start_ms = evaluation.next.recovery_hold_start_ms;
  auto next_sequence = next.sequence.Next();
  if (!next_sequence) {
    return Result<LatchClearOutcome>::Err(next_sequence.status());
  }
  next.committed.state_sequence = next_sequence.value();

  Reason reason;
  reason.code = ReasonCode::LatchCleared;
  reason.mode = next.committed.mode;
  reason.detail = "latch cleared by " + request.authority_reference;
  ReasonTrace trace = outcome.trace;
  trace.Add(std::move(reason));
  trace.Canonicalize();
  outcome.trace = trace;

  IdempotencyEntry entry;
  entry.key = request.envelope.idempotency_key;
  entry.request_digest = request_digest;
  entry.operation = OperationKind::ClearLatch;
  entry.resulting_sequence = next_sequence.value();
  entry.result_digest = trace.digest();
  entry.recorded_at_ms = request.context.now_ms;
  next.idempotency.push_back(entry);
  ++next.committed_operations;

  Status committed = Commit(std::move(next));
  if (!committed.ok()) {
    return Result<LatchClearOutcome>::Err(committed);
  }
  outcome.cleared = true;
  outcome.replayed = false;
  outcome.state_sequence = store_->state().sequence;
  return Result<LatchClearOutcome>::Ok(std::move(outcome));
}

Result<AdoptEpochOutcome> Coordinator::AdoptEpoch(const AdoptEpochRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!request.new_incarnation.IsValid()) {
    return Result<AdoptEpochOutcome>::Err(ErrorCode::InvalidArgument,
                                          "a non-zero incarnation is required to adopt an epoch");
  }
  if (request.authority_reference.empty()) {
    return Result<AdoptEpochOutcome>::Err(
        ErrorCode::InvalidArgument, "adopting an epoch requires an authority reference");
  }
  PersistedState next = store_->state();
  if (request.expected_epoch != next.committed.epoch) {
    return Result<AdoptEpochOutcome>::Err(
        ErrorCode::StaleAuthority,
        "expected epoch " + request.expected_epoch.ToString() +
            " is not the recorded epoch " + next.committed.epoch.ToString());
  }
  auto next_epoch = next.committed.epoch.Next();
  if (!next_epoch) {
    return Result<AdoptEpochOutcome>::Err(next_epoch.status());
  }
  next.committed.epoch = next_epoch.value();
  next.committed.incarnation = request.new_incarnation;
  auto next_sequence = next.sequence.Next();
  if (!next_sequence) {
    return Result<AdoptEpochOutcome>::Err(next_sequence.status());
  }
  next.committed.state_sequence = next_sequence.value();
  ++next.committed_operations;

  AdoptEpochOutcome outcome;
  outcome.epoch = next.committed.epoch;
  outcome.incarnation = request.new_incarnation;
  outcome.state_sequence = next_sequence.value();

  Status committed = Commit(std::move(next));
  if (!committed.ok()) {
    return Result<AdoptEpochOutcome>::Err(committed);
  }
  epoch_adopted_ = true;
  return Result<AdoptEpochOutcome>::Ok(std::move(outcome));
}

Result<CoordinatorStatus> Coordinator::GetStatus() const {
  std::lock_guard<std::mutex> guard(mutex_);
  const PersistedState& state = store_->state();
  CoordinatorStatus status;
  status.state_sequence = state.sequence;
  status.decision_sequence = state.committed.decision_sequence;
  status.epoch = state.committed.epoch;
  status.incarnation = state.committed.incarnation;
  status.mode = state.committed.mode;
  status.posture = state.committed.posture;
  status.since_ms = state.committed.since_ms;
  status.latched = state.committed.latch.latched;
  status.committed_operations = state.committed_operations;
  status.evaluations = state.evaluations;
  status.history_entries = state.history.size();
  status.authorizations = state.authorizations.size();
  status.acknowledgements = state.acknowledgements.size();
  status.verifications = state.verifications.size();
  status.idempotency_entries = state.idempotency.size();
  status.epoch_adopted = epoch_adopted_;
  return Result<CoordinatorStatus>::Ok(std::move(status));
}

Result<std::vector<HistoryEntry>> Coordinator::History(std::size_t limit) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<HistoryEntry>& history = store_->state().history;
  std::vector<HistoryEntry> result;
  const std::size_t take = (std::min)(limit, history.size());
  result.reserve(take);
  for (std::size_t i = history.size() - take; i < history.size(); ++i) {
    result.push_back(history[i]);
  }
  return Result<std::vector<HistoryEntry>>::Ok(std::move(result));
}

Result<std::vector<RestrictionAuthorization>> Coordinator::Authorizations(
    std::size_t limit) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<RestrictionAuthorization>& authorizations = store_->state().authorizations;
  std::vector<RestrictionAuthorization> result;
  const std::size_t take = (std::min)(limit, authorizations.size());
  result.reserve(take);
  for (std::size_t i = authorizations.size() - take; i < authorizations.size(); ++i) {
    result.push_back(authorizations[i]);
  }
  return Result<std::vector<RestrictionAuthorization>>::Ok(std::move(result));
}

void Coordinator::TrimBounded(PersistedState& state) const {
  // Every durable collection is bounded. When a bound is reached the oldest
  // entry is evicted: history, authorizations and their acknowledgement and
  // verification records are historical, and an evicted authorization can never
  // be re-bound because authorizations are fenced by decision and epoch.
  const auto trim = [](auto& entries, std::size_t capacity) {
    if (capacity != 0 && entries.size() > capacity) {
      entries.erase(entries.begin(),
                    entries.end() - static_cast<std::ptrdiff_t>(capacity));
    }
  };
  trim(state.history, history_capacity_);
  trim(state.authorizations, record_capacity_);
  trim(state.acknowledgements, record_capacity_);
  trim(state.verifications, record_capacity_);
  trim(state.idempotency, idempotency_capacity_);
}

}  // namespace dom
