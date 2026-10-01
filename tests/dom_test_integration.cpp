// Degraded Operation Manager - end to end authority lifecycle tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

namespace {

using namespace dom;

struct Harness {
  dom::test::TempDir directory;
  PolicyDocument policy;
  std::unique_ptr<Coordinator> coordinator;
};

Result<std::unique_ptr<Harness>> MakeHarness(const std::string& label, std::uint64_t incarnation) {
  auto directory = dom::test::TempDir::Create(label);
  if (!directory) {
    return Result<std::unique_ptr<Harness>>::Err(directory.status());
  }
  std::unique_ptr<Harness> harness(new Harness());
  harness->directory = std::move(directory.value());
  harness->policy = dom::test::MakePolicy();
  auto options = dom::test::MakeCoordinatorOptions(harness->directory.path(), harness->policy,
                                                   incarnation);
  auto coordinator = Coordinator::Open(options);
  if (!coordinator) {
    return Result<std::unique_ptr<Harness>>::Err(coordinator.status());
  }
  harness->coordinator = std::move(coordinator.value());
  return Result<std::unique_ptr<Harness>>::Ok(std::move(harness));
}

Result<DecisionOutcome> EvaluateNow(Coordinator& coordinator, const EvidenceSnapshot& evidence,
                                    const std::string& key, std::uint64_t now_ms,
                                    std::uint64_t tick) {
  auto status = coordinator.GetStatus();
  if (!status) {
    return Result<DecisionOutcome>::Err(status.status());
  }
  EvaluationRequest request;
  request.envelope.idempotency_key = Sha256::Of(key);
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = Instant::FromValue(now_ms);
  request.context.tick = Tick::FromValue(tick);
  request.evidence = evidence;
  return coordinator.Evaluate(request);
}

bool HasReasonCode(const ReasonTrace& trace, ReasonCode code) {
  for (const Reason& reason : trace.reasons()) {
    if (reason.code == code) {
      return true;
    }
  }
  return false;
}

EvidenceSnapshot CriticalEvidence(std::uint64_t revision, std::uint64_t observed_at) {
  dom::test::EvidenceOptions options;
  options.revision = revision;
  options.observed_at_ms = observed_at;
  options.severity = 3;
  options.redundancy = 400;
  return dom::test::MakeEvidence(options);
}

EvidenceSnapshot HealthyEvidence(std::uint64_t revision, std::uint64_t observed_at) {
  dom::test::EvidenceOptions options;
  options.revision = revision;
  options.observed_at_ms = observed_at;
  return dom::test::MakeEvidence(options);
}

DOM_TEST(full_lifecycle_from_decision_to_verified_effect) {
  auto harness = MakeHarness("integration-lifecycle", 9001);
  DOM_CHECK_OK(harness);
  Coordinator& coordinator = *harness.value()->coordinator;

  auto status = coordinator.GetStatus();
  DOM_CHECK_OK(status);
  DOM_CHECK_EQ(status.value().posture, Posture::Nominal);
  DOM_CHECK_EQ(status.value().state_sequence.value(), 1ull);
  DOM_CHECK_EQ(status.value().decision_sequence.value(), 0ull);

  // 1. Evidence drives an escalation.
  auto escalated = EvaluateNow(coordinator, CriticalEvidence(1, 1000), "lifecycle-1", 1000, 1);
  DOM_CHECK_OK(escalated);
  DOM_CHECK(escalated.value().state_changed);
  DOM_CHECK_EQ(escalated.value().decision.posture, Posture::Restricted);
  DOM_CHECK_EQ(escalated.value().decision.policy_digest, PolicyDigest(harness.value()->policy));
  DOM_CHECK_EQ(escalated.value().state_sequence.value(), 2ull);
  DOM_CHECK_EQ(escalated.value().decision.sequence.value(), 1ull);

  // 2. Authorisation to apply the restrictions, bound to that decision.
  AuthorizationRequest authorization;
  authorization.envelope.idempotency_key = Sha256::Of(std::string_view("lifecycle-auth"));
  authorization.envelope.expected_state_sequence = escalated.value().state_sequence;
  authorization.envelope.expected_epoch = escalated.value().committed.epoch;
  authorization.now_ms = Instant::FromValue(1500);
  authorization.ttl_ms = Duration::FromValue(60000);
  auto issued = coordinator.Authorize(authorization);
  DOM_CHECK_OK(issued);
  DOM_CHECK_EQ(issued.value().authorization.bound_decision, escalated.value().decision.sequence);
  DOM_CHECK_EQ(issued.value().authorization.decision_digest, escalated.value().decision.digest());
  DOM_CHECK(!issued.value().authorization.IsExpiredAt(Instant::FromValue(1500)));
  DOM_CHECK(issued.value().authorization.IsExpiredAt(Instant::FromValue(70000)));
  DOM_CHECK_EQ(issued.value().authorization.restrictions.restricted_services.size(), 2u);

  // 3. The adjacent owner acknowledges, then reports an observed effect.
  AcknowledgementRequest acknowledgement;
  acknowledgement.envelope.idempotency_key = Sha256::Of(std::string_view("lifecycle-ack"));
  acknowledgement.envelope.expected_state_sequence = issued.value().state_sequence;
  acknowledgement.envelope.expected_epoch = escalated.value().committed.epoch;
  acknowledgement.authorization = issued.value().authorization.id;
  acknowledgement.adjacent_owner = "load-shedding";
  acknowledgement.status = AcceptanceStatus::PartiallyAccepted;
  acknowledgement.recorded_at_ms = Instant::FromValue(2000);
  acknowledgement.detail = "two of three services throttled";
  auto acknowledged = coordinator.RecordAcknowledgement(acknowledgement);
  DOM_CHECK_OK(acknowledged);
  DOM_CHECK(!acknowledged.value().stale_binding);
  DOM_CHECK(HasReasonCode(acknowledged.value().trace, ReasonCode::AcknowledgementRecorded));

  VerificationRequest verification;
  verification.envelope.idempotency_key = Sha256::Of(std::string_view("lifecycle-verify"));
  verification.envelope.expected_state_sequence = acknowledged.value().state_sequence;
  verification.envelope.expected_epoch = escalated.value().committed.epoch;
  verification.authorization = issued.value().authorization.id;
  verification.adjacent_owner = "load-shedding";
  verification.outcome = VerificationOutcome::PartiallyEffective;
  verification.observed_at_ms = Instant::FromValue(2500);
  verification.effect_digest = Sha256::Of(std::string_view("observed-effect"));
  verification.source_digest = Sha256::Of(std::string_view("observer-snapshot"));
  auto verified = coordinator.RecordEffectVerification(verification);
  DOM_CHECK_OK(verified);
  DOM_CHECK(!verified.value().stale_binding);
  DOM_CHECK(HasReasonCode(verified.value().trace, ReasonCode::EffectVerified));

  // 4. A later decision supersedes the old one, so the old binding is fenced.
  auto recovered = EvaluateNow(coordinator, HealthyEvidence(2, 3000), "lifecycle-2", 3000, 2);
  DOM_CHECK_OK(recovered);
  DOM_CHECK_EQ(recovered.value().decision.posture, Posture::Restricted);
  DOM_CHECK(recovered.value().state_changed);

  VerificationRequest late = verification;
  late.envelope.idempotency_key = Sha256::Of(std::string_view("lifecycle-verify-late"));
  late.envelope.expected_state_sequence = recovered.value().state_sequence;
  late.envelope.expected_epoch = recovered.value().committed.epoch;
  late.observed_at_ms = Instant::FromValue(4000);
  auto fenced = coordinator.RecordEffectVerification(late);
  DOM_CHECK_OK(fenced);
  DOM_CHECK(fenced.value().stale_binding);
  DOM_CHECK(HasReasonCode(fenced.value().trace, ReasonCode::DecisionFenced));

  auto stored = coordinator.GetStatus();
  DOM_CHECK_OK(stored);
  DOM_CHECK_EQ(stored.value().authorizations, 1u);
  DOM_CHECK_EQ(stored.value().acknowledgements, 1u);
  DOM_CHECK_EQ(stored.value().verifications, 2u);
}

DOM_TEST(idempotent_replay_and_conflicting_reuse) {
  auto harness = MakeHarness("integration-replay", 9002);
  DOM_CHECK_OK(harness);
  Coordinator& coordinator = *harness.value()->coordinator;
  auto status = coordinator.GetStatus();
  DOM_CHECK_OK(status);

  EvaluationRequest request;
  request.envelope.idempotency_key = Sha256::Of(std::string_view("replay-key"));
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = Instant::FromValue(1000);
  request.context.tick = Tick::FromValue(1);
  request.evidence = CriticalEvidence(1, 1000);

  auto first = coordinator.Evaluate(request);
  DOM_CHECK_OK(first);
  DOM_CHECK(!first.value().replayed);
  DOM_CHECK_EQ(first.value().state_sequence.value(), 2ull);

  auto replay = coordinator.Evaluate(request);
  DOM_CHECK_OK(replay);
  DOM_CHECK(replay.value().replayed);
  DOM_CHECK(!replay.value().state_changed);
  DOM_CHECK_EQ(replay.value().state_sequence.value(), 2ull);

  auto after_replay = coordinator.GetStatus();
  DOM_CHECK_OK(after_replay);
  DOM_CHECK_EQ(after_replay.value().state_sequence.value(), 2ull);
  DOM_CHECK_EQ(after_replay.value().decision_sequence.value(), 1ull);

  // The same key for a different request is a conflict, not a second mutation.
  EvaluationRequest different = request;
  different.context.now_ms = Instant::FromValue(1100);
  different.evidence = HealthyEvidence(2, 1100);
  auto conflict = coordinator.Evaluate(different);
  DOM_CHECK_ERR(conflict, ErrorCode::Conflict);
  auto unchanged = coordinator.GetStatus();
  DOM_CHECK_OK(unchanged);
  DOM_CHECK_EQ(unchanged.value().state_sequence.value(), 2ull);
}

DOM_TEST(stale_envelopes_are_refused_before_they_mutate) {
  auto harness = MakeHarness("integration-stale", 9003);
  DOM_CHECK_OK(harness);
  Coordinator& coordinator = *harness.value()->coordinator;
  auto status = coordinator.GetStatus();
  DOM_CHECK_OK(status);

  EvaluationRequest request;
  request.envelope.idempotency_key = Sha256::Of(std::string_view("stale-1"));
  request.envelope.expected_state_sequence = StateSequence::FromValue(99);
  request.envelope.expected_epoch = status.value().epoch;
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = Instant::FromValue(1000);
  request.context.tick = Tick::FromValue(1);
  request.evidence = CriticalEvidence(1, 1000);
  DOM_CHECK_ERR(coordinator.Evaluate(request), ErrorCode::PreconditionFailed);

  EvaluationRequest wrong_epoch = request;
  wrong_epoch.envelope.idempotency_key = Sha256::Of(std::string_view("stale-2"));
  wrong_epoch.envelope.expected_state_sequence = status.value().state_sequence;
  wrong_epoch.envelope.expected_epoch = ControlEpoch::FromValue(42);
  wrong_epoch.context.epoch = ControlEpoch::FromValue(42);
  DOM_CHECK_ERR(coordinator.Evaluate(wrong_epoch), ErrorCode::StaleAuthority);

  EvaluationRequest zero_key = request;
  zero_key.envelope.idempotency_key = Digest::Zero();
  zero_key.envelope.expected_state_sequence = status.value().state_sequence;
  DOM_CHECK_ERR(coordinator.Evaluate(zero_key), ErrorCode::InvalidArgument);

  auto unchanged = coordinator.GetStatus();
  DOM_CHECK_OK(unchanged);
  DOM_CHECK_EQ(unchanged.value().state_sequence.value(), 1ull);
}

DOM_TEST(authorization_bounds_are_enforced) {
  auto harness = MakeHarness("integration-bounds", 9004);
  DOM_CHECK_OK(harness);
  Coordinator& coordinator = *harness.value()->coordinator;
  auto status = coordinator.GetStatus();
  DOM_CHECK_OK(status);

  AuthorizationRequest request;
  request.envelope.idempotency_key = Sha256::Of(std::string_view("bound-auth"));
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.now_ms = Instant::FromValue(1000);
  request.ttl_ms = Duration::FromValue(60000);
  // No committed decision exists yet, so nothing may be authorized.
  DOM_CHECK_ERR(coordinator.Authorize(request), ErrorCode::PreconditionFailed);

  DOM_CHECK_OK(EvaluateNow(coordinator, CriticalEvidence(1, 1000), "bounds-1", 1000, 1));
  auto after = coordinator.GetStatus();
  DOM_CHECK_OK(after);
  request.envelope.expected_state_sequence = after.value().state_sequence;
  request.ttl_ms = Duration::FromValue(0);
  DOM_CHECK_ERR(coordinator.Authorize(request), ErrorCode::InvalidArgument);
  request.ttl_ms = Duration::FromValue(600001);
  DOM_CHECK_ERR(coordinator.Authorize(request), ErrorCode::InvalidArgument);

  request.ttl_ms = Duration::FromValue(60000);
  auto issued = coordinator.Authorize(request);
  DOM_CHECK_OK(issued);

  AcknowledgementRequest unknown;
  unknown.envelope.idempotency_key = Sha256::Of(std::string_view("unknown-ack"));
  unknown.envelope.expected_state_sequence = issued.value().state_sequence;
  unknown.envelope.expected_epoch = after.value().epoch;
  unknown.authorization = AuthorizationId::FromValue(999);
  unknown.adjacent_owner = "nobody";
  unknown.recorded_at_ms = Instant::FromValue(2000);
  DOM_CHECK_ERR(coordinator.RecordAcknowledgement(unknown), ErrorCode::NotFound);

  unknown.envelope.idempotency_key = Sha256::Of(std::string_view("late-ack"));
  unknown.authorization = issued.value().authorization.id;
  unknown.recorded_at_ms = Instant::FromValue(1000000);
  DOM_CHECK_ERR(coordinator.RecordAcknowledgement(unknown), ErrorCode::NotPermitted);
}

DOM_TEST(a_latched_mode_requires_authority_and_fresh_evidence_to_leave) {
  auto harness = MakeHarness("integration-latch", 9005);
  DOM_CHECK_OK(harness);
  Coordinator& coordinator = *harness.value()->coordinator;

  dom::test::EvidenceOptions emergency;
  emergency.revision = 1;
  emergency.observed_at_ms = 1000;
  emergency.severity = 3;
  emergency.redundancy = 400;
  emergency.cooling_margin = 4000;
  auto escalated = EvaluateNow(coordinator, dom::test::MakeEvidence(emergency), "latch-1", 1000, 1);
  DOM_CHECK_OK(escalated);
  DOM_CHECK_EQ(escalated.value().decision.posture, Posture::Emergency);

  auto latched = coordinator.GetStatus();
  DOM_CHECK_OK(latched);
  DOM_CHECK(latched.value().latched);
  const Digest cause = coordinator.state().committed.latch.cause_digest;
  DOM_CHECK(!cause.IsZero());

  // A wrong cause digest is refused before anything else happens.
  LatchClearRequest clear;
  clear.envelope.idempotency_key = Sha256::Of(std::string_view("latch-clear-wrong"));
  clear.envelope.expected_state_sequence = latched.value().state_sequence;
  clear.envelope.expected_epoch = latched.value().epoch;
  clear.authority_reference = "facility-operator";
  clear.latch_cause_digest = Sha256::Of(std::string_view("not-the-cause"));
  clear.context.epoch = latched.value().epoch;
  clear.context.incarnation = latched.value().incarnation;
  clear.context.now_ms = Instant::FromValue(2000);
  clear.context.tick = Tick::FromValue(2);
  clear.evidence = HealthyEvidence(2, 2000);
  DOM_CHECK_ERR(coordinator.ClearLatch(clear), ErrorCode::PreconditionFailed);

  // Stale evidence cannot justify leaving the latched mode.
  clear.envelope.idempotency_key = Sha256::Of(std::string_view("latch-clear-stale"));
  clear.latch_cause_digest = cause;
  clear.context.now_ms = Instant::FromValue(400000);
  DOM_CHECK_ERR(coordinator.ClearLatch(clear), ErrorCode::NotPermitted);

  // Fresh evidence and the matching cause digest clear it; the mode itself is
  // unchanged until the next evaluation.
  clear.envelope.idempotency_key = Sha256::Of(std::string_view("latch-clear"));
  clear.context.now_ms = Instant::FromValue(410000);
  clear.context.tick = Tick::FromValue(3);
  clear.evidence = HealthyEvidence(3, 410000);
  auto cleared = coordinator.ClearLatch(clear);
  DOM_CHECK_OK(cleared);
  DOM_CHECK(cleared.value().cleared);
  auto after_clear = coordinator.GetStatus();
  DOM_CHECK_OK(after_clear);
  DOM_CHECK(!after_clear.value().latched);
  DOM_CHECK_EQ(after_clear.value().posture, Posture::Emergency);

  // The next evaluation recovers, staged by dwell and recovery hold.
  auto hold_started = EvaluateNow(coordinator, HealthyEvidence(4, 420000), "latch-2", 420000, 4);
  DOM_CHECK_OK(hold_started);
  DOM_CHECK_EQ(hold_started.value().decision.posture, Posture::Emergency);
  auto recovered = EvaluateNow(coordinator, HealthyEvidence(5, 420000 + 60000 + 1), "latch-3",
                               420000 + 60000 + 1, 5);
  DOM_CHECK_OK(recovered);
  DOM_CHECK_EQ(recovered.value().decision.verdict, Verdict::Deescalate);
  DOM_CHECK_EQ(recovered.value().decision.posture, Posture::Nominal);
}

DOM_TEST(history_reports_the_committed_transitions) {
  auto harness = MakeHarness("integration-history", 9006);
  DOM_CHECK_OK(harness);
  Coordinator& coordinator = *harness.value()->coordinator;
  // A major incident escalates to conserve, then a critical one to restricted.
  dom::test::EvidenceOptions major;
  major.revision = 1;
  major.observed_at_ms = 1000;
  major.severity = 2;
  DOM_CHECK_OK(EvaluateNow(coordinator, dom::test::MakeEvidence(major), "history-1", 1000, 1));
  DOM_CHECK_OK(EvaluateNow(coordinator, CriticalEvidence(2, 2000), "history-2", 2000, 2));
  auto history = coordinator.History(10);
  DOM_CHECK_OK(history);
  DOM_CHECK_EQ(history.value().size(), 2u);
  DOM_CHECK_EQ(history.value()[0].previous_posture, Posture::Nominal);
  DOM_CHECK_EQ(history.value()[0].posture, Posture::Conserve);
  DOM_CHECK_EQ(history.value()[0].verdict, Verdict::Escalate);
  DOM_CHECK_EQ(history.value()[1].previous_posture, Posture::Conserve);
  DOM_CHECK_EQ(history.value()[1].posture, Posture::Restricted);
  DOM_CHECK(history.value()[0].state_sequence < history.value()[1].state_sequence);
  DOM_CHECK(!history.value()[0].decision_digest.IsZero());
  auto limited = coordinator.History(1);
  DOM_CHECK_OK(limited);
  DOM_CHECK_EQ(limited.value().size(), 1u);
  DOM_CHECK_EQ(limited.value()[0].state_sequence, history.value()[1].state_sequence);
}

DOM_TEST(a_reopened_authority_keeps_its_mode_and_requires_adoption) {
  auto harness = MakeHarness("integration-reopen", 9007);
  DOM_CHECK_OK(harness);
  auto escalated = EvaluateNow(*harness.value()->coordinator, CriticalEvidence(1, 1000),
                               "reopen-1", 1000, 1);
  DOM_CHECK_OK(escalated);
  const ModeId escalated_mode = escalated.value().decision.mode;
  const StateSequence sequence = escalated.value().state_sequence;
  harness.value()->coordinator.reset();

  auto options = dom::test::MakeCoordinatorOptions(harness.value()->directory.path(),
                                                   harness.value()->policy, 9008);
  auto reopened = Coordinator::Open(options);
  DOM_CHECK_OK(reopened);
  auto status = reopened.value()->GetStatus();
  DOM_CHECK_OK(status);
  DOM_CHECK_EQ(status.value().state_sequence.value(), sequence.value());
  DOM_CHECK_EQ(status.value().mode.value(), escalated_mode.value());
  DOM_CHECK_EQ(status.value().posture, Posture::Restricted);
  DOM_CHECK(!status.value().epoch_adopted);
  DOM_CHECK_EQ(reopened.value()->recovery().outcome, RecoveryOutcome::Loaded);

  // Authority is not inherited by opening: it must be adopted explicitly.
  DOM_CHECK_ERR(EvaluateNow(*reopened.value(), HealthyEvidence(2, 5000), "reopen-2", 5000, 2),
                ErrorCode::StaleAuthority);
  AdoptEpochRequest adopt;
  adopt.expected_epoch = status.value().epoch;
  adopt.new_incarnation = Incarnation::FromValue(9008);
  adopt.authority_reference = "integration-test";
  auto adopted = reopened.value()->AdoptEpoch(adopt);
  DOM_CHECK_OK(adopted);
  DOM_CHECK(adopted.value().epoch.value() == status.value().epoch.value() + 1);
  auto recovered = EvaluateNow(*reopened.value(), HealthyEvidence(3, 9000), "reopen-3", 9000, 3);
  DOM_CHECK_OK(recovered);
}

}  // namespace
