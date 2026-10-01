// Degraded Operation Manager - evaluation engine tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

namespace {

using namespace dom;

EvaluationInputs Inputs(const PolicyDocument& policy, const CommittedView& committed,
                        const EvidenceSnapshot& evidence, std::uint64_t now_ms,
                        std::uint64_t tick = 1) {
  EvaluationInputs inputs;
  inputs.policy = &policy;
  inputs.evidence = &evidence;
  inputs.committed = committed;
  inputs.context.epoch = committed.epoch;
  inputs.context.incarnation = committed.incarnation;
  inputs.context.now_ms = Instant::FromValue(now_ms);
  inputs.context.tick = Tick::FromValue(tick);
  inputs.policy_validated = true;
  return inputs;
}

bool HasReason(const ReasonTrace& trace, ReasonCode code) {
  for (const Reason& reason : trace.reasons()) {
    if (reason.code == code) {
      return true;
    }
  }
  return false;
}

CommittedView CommittedIn(const PolicyDocument& policy, std::uint64_t mode_id, Posture posture,
                          std::uint64_t since_ms) {
  CommittedView committed = dom::test::MakeCommittedView(policy, 1, 1000);
  committed.mode = ModeId::FromValue(mode_id);
  committed.posture = posture;
  committed.since_ms = Instant::FromValue(since_ms);
  return committed;
}

DOM_TEST(healthy_facility_holds_nominal) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const CommittedView committed = CommittedIn(policy, 1, Posture::Nominal, 0);
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 1000;
  auto evidence = dom::test::MakeEvidence(options);
  auto outcome = EvaluateMode(Inputs(policy, committed, evidence, 2000));
  DOM_CHECK_OK(outcome);
  DOM_CHECK_EQ(outcome.value().decision.verdict, Verdict::Hold);
  DOM_CHECK_EQ(outcome.value().decision.posture, Posture::Nominal);
  DOM_CHECK_EQ(outcome.value().decision.mode.value(), 1ull);
  DOM_CHECK(!outcome.value().changed);
  DOM_CHECK(outcome.value().restrictions.restricted_services.empty());
  DOM_CHECK(!outcome.value().decision.input_digest.IsZero());
  DOM_CHECK(!outcome.value().decision.trace.empty());
  DOM_CHECK(HasReason(outcome.value().decision.trace, ReasonCode::Held));
}

DOM_TEST(escalation_is_conservative_and_deterministic) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const CommittedView committed = CommittedIn(policy, 1, Posture::Nominal, 0);
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 1000;
  options.severity = 2;
  auto evidence = dom::test::MakeEvidence(options);

  auto first = EvaluateMode(Inputs(policy, committed, evidence, 2000));
  DOM_CHECK_OK(first);
  auto second = EvaluateMode(Inputs(policy, committed, evidence, 2000));
  DOM_CHECK_OK(second);
  DOM_CHECK_EQ(first.value().decision.digest(), second.value().decision.digest());
  DOM_CHECK_EQ(first.value().decision.trace.digest(), second.value().decision.trace.digest());

  DOM_CHECK_EQ(first.value().decision.verdict, Verdict::Escalate);
  DOM_CHECK_EQ(first.value().decision.mode.value(), 5ull);  // conserve outranks watch
  DOM_CHECK_EQ(first.value().decision.posture, Posture::Conserve);
  DOM_CHECK(first.value().changed);
  DOM_CHECK_EQ(first.value().next.since_ms.value(), 2000ull);
  DOM_CHECK_EQ(first.value().restrictions.restricted_services.size(), 1u);
  DOM_CHECK_EQ(first.value().restrictions.restricted_services.front(), std::string("batch"));
  DOM_CHECK(HasReason(first.value().decision.trace, ReasonCode::Escalated));
  DOM_CHECK(!first.value().next.latch.latched);
}

DOM_TEST(the_most_restrictive_satisfied_mode_wins) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const CommittedView committed = CommittedIn(policy, 1, Posture::Nominal, 0);
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 1000;
  options.severity = 3;
  options.redundancy = 400;
  options.cooling_margin = 4000;
  auto evidence = dom::test::MakeEvidence(options);
  auto outcome = EvaluateMode(Inputs(policy, committed, evidence, 2000));
  DOM_CHECK_OK(outcome);
  DOM_CHECK_EQ(outcome.value().decision.posture, Posture::Emergency);
  DOM_CHECK_EQ(outcome.value().decision.mode.value(), 9ull);
  DOM_CHECK(outcome.value().next.latch.latched);
  DOM_CHECK_EQ(outcome.value().next.latch.mode.value(), 9ull);
  // The latch cause digest is assigned by the coordinator when it commits.
  DOM_CHECK(outcome.value().next.latch.cause_digest.IsZero());
  // Life safety and durability survive the emergency restriction set.
  DOM_CHECK(std::find(outcome.value().restrictions.permitted_services.begin(),
                      outcome.value().restrictions.permitted_services.end(),
                      "life-safety") != outcome.value().restrictions.permitted_services.end());
}

DOM_TEST(recovery_needs_dwell_and_a_continuous_hold) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const CommittedView committed = CommittedIn(policy, 5, Posture::Conserve, 1000);
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 3000;
  auto evidence = dom::test::MakeEvidence(options);

  auto first = EvaluateMode(Inputs(policy, committed, evidence, 3000));
  DOM_CHECK_OK(first);
  DOM_CHECK_EQ(first.value().decision.verdict, Verdict::Hold);
  DOM_CHECK_EQ(first.value().decision.posture, Posture::Conserve);
  DOM_CHECK(first.value().next.has_recovery_hold);
  DOM_CHECK_EQ(first.value().next.recovery_hold_start_ms.value(), 3000ull);
  DOM_CHECK(first.value().changed);  // the hold start is authoritative state
  DOM_CHECK(HasReason(first.value().decision.trace, ReasonCode::RecoveryHoldActive));

  // The hold has not elapsed yet.
  auto early = EvaluateMode(Inputs(policy, first.value().next, evidence, 31000));
  DOM_CHECK_OK(early);
  DOM_CHECK_EQ(early.value().decision.verdict, Verdict::Hold);

  // After the hold elapses recovery descends to the least restrictive eligible mode.
  dom::test::EvidenceOptions later_options;
  later_options.observed_at_ms = 33000;
  auto later_evidence = dom::test::MakeEvidence(later_options);
  auto recovered = EvaluateMode(Inputs(policy, first.value().next, later_evidence, 33000));
  DOM_CHECK_OK(recovered);
  DOM_CHECK_EQ(recovered.value().decision.verdict, Verdict::Deescalate);
  DOM_CHECK_EQ(recovered.value().decision.mode.value(), 1ull);
  DOM_CHECK_EQ(recovered.value().decision.posture, Posture::Nominal);
  DOM_CHECK(!recovered.value().next.has_recovery_hold);
  DOM_CHECK(HasReason(recovered.value().decision.trace, ReasonCode::RecoveryHoldSatisfied));
}

DOM_TEST(minimum_dwell_blocks_recovery) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const CommittedView committed = CommittedIn(policy, 5, Posture::Conserve, 1000);
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 1500;
  auto evidence = dom::test::MakeEvidence(options);
  auto outcome = EvaluateMode(Inputs(policy, committed, evidence, 1500));
  DOM_CHECK_OK(outcome);
  DOM_CHECK_EQ(outcome.value().decision.verdict, Verdict::Hold);
  DOM_CHECK(HasReason(outcome.value().decision.trace, ReasonCode::MinDwellActive));
  DOM_CHECK(!outcome.value().next.has_recovery_hold);
}

DOM_TEST(stale_evidence_never_restores_a_less_restrictive_mode) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const CommittedView committed = CommittedIn(policy, 6, Posture::Restricted, 0);
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 1000;
  options.severity = 0;
  options.redundancy = 1000;
  auto evidence = dom::test::MakeEvidence(options);

  auto outcome = EvaluateMode(Inputs(policy, committed, evidence, 200000));
  DOM_CHECK_OK(outcome);
  DOM_CHECK_EQ(outcome.value().decision.verdict, Verdict::BecomeIndeterminate);
  DOM_CHECK_EQ(outcome.value().decision.posture, Posture::Indeterminate);
  DOM_CHECK_EQ(outcome.value().decision.mode.value(), 6ull);  // the mode is retained
  DOM_CHECK(HasReason(outcome.value().decision.trace, ReasonCode::EvidenceStale));
  DOM_CHECK(HasReason(outcome.value().decision.trace, ReasonCode::BecameIndeterminate));

  // The indeterminate posture is never weaker than the mode it retains.
  const ModeDefinition* restricted = FindMode(policy, ModeId::FromValue(6)).value();
  const RestrictionSet retained = RestrictionsForMode(policy, *restricted);
  for (const std::string& service : retained.restricted_services) {
    DOM_CHECK(std::find(outcome.value().restrictions.restricted_services.begin(),
                        outcome.value().restrictions.restricted_services.end(),
                        service) != outcome.value().restrictions.restricted_services.end());
  }
  for (const std::string& service : outcome.value().restrictions.permitted_services) {
    DOM_CHECK(std::find(retained.permitted_services.begin(), retained.permitted_services.end(),
                        service) != retained.permitted_services.end());
  }
  DOM_CHECK(std::find(outcome.value().restrictions.restricted_services.begin(),
                      outcome.value().restrictions.restricted_services.end(),
                      "batch") != outcome.value().restrictions.restricted_services.end());
}

DOM_TEST(missing_and_conflicted_evidence_become_indeterminate) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const CommittedView committed = CommittedIn(policy, 1, Posture::Nominal, 0);

  dom::test::EvidenceOptions missing_options;
  missing_options.observed_at_ms = 1000;
  missing_options.include_cooling = false;
  auto missing = dom::test::MakeEvidence(missing_options);
  auto outcome = EvaluateMode(Inputs(policy, committed, missing, 2000));
  DOM_CHECK_OK(outcome);
  DOM_CHECK_EQ(outcome.value().decision.verdict, Verdict::BecomeIndeterminate);
  DOM_CHECK(HasReason(outcome.value().decision.trace, ReasonCode::EvidenceMissing));

  dom::test::EvidenceOptions conflicted_options;
  conflicted_options.observed_at_ms = 1000;
  conflicted_options.cooling_conflict = true;
  auto conflicted = dom::test::MakeEvidence(conflicted_options);
  auto conflicted_outcome = EvaluateMode(Inputs(policy, committed, conflicted, 2000));
  DOM_CHECK_OK(conflicted_outcome);
  DOM_CHECK_EQ(conflicted_outcome.value().decision.verdict, Verdict::BecomeIndeterminate);
  DOM_CHECK(HasReason(conflicted_outcome.value().decision.trace, ReasonCode::EvidenceConflicted));

  // Already indeterminate and still unusable: nothing changes.
  CommittedView indeterminate = CommittedIn(policy, 1, Posture::Indeterminate, 0);
  auto held = EvaluateMode(Inputs(policy, indeterminate, missing, 2000));
  DOM_CHECK_OK(held);
  DOM_CHECK_EQ(held.value().decision.verdict, Verdict::Hold);
  DOM_CHECK(!held.value().changed);
}

DOM_TEST(a_latch_holds_the_mode_until_it_is_cleared) {
  const PolicyDocument policy = dom::test::MakePolicy();
  CommittedView committed = CommittedIn(policy, 9, Posture::Emergency, 0);
  committed.latch.latched = true;
  committed.latch.mode = ModeId::FromValue(9);
  committed.latch.latched_at_ms = Instant::FromValue(0);
  committed.latch.cause_digest = Sha256::Of(std::string_view("latch-cause"));
  committed.has_recovery_hold = true;
  committed.recovery_hold_start_ms = Instant();

  dom::test::EvidenceOptions options;
  options.observed_at_ms = 100000;
  auto evidence = dom::test::MakeEvidence(options);

  auto blocked = EvaluateMode(Inputs(policy, committed, evidence, 100000));
  DOM_CHECK_OK(blocked);
  DOM_CHECK_EQ(blocked.value().decision.verdict, Verdict::Hold);
  DOM_CHECK_EQ(blocked.value().decision.posture, Posture::Emergency);
  DOM_CHECK(HasReason(blocked.value().decision.trace, ReasonCode::LatchActive));
  DOM_CHECK(blocked.value().next.latch.latched);

  EvaluationInputs clearing = Inputs(policy, committed, evidence, 100000);
  clearing.latch_clear_requested = true;
  auto cleared = EvaluateMode(clearing);
  DOM_CHECK_OK(cleared);
  DOM_CHECK(cleared.value().recovery_preconditions_ok);
  DOM_CHECK(cleared.value().latch_cleared);
  DOM_CHECK(!cleared.value().next.latch.latched);
  DOM_CHECK_EQ(cleared.value().decision.posture, Posture::Emergency);  // unchanged by the clear
  DOM_CHECK_EQ(cleared.value().decision.verdict, Verdict::Hold);

  // The same request against stale evidence must not clear anything.
  EvaluationInputs stale_clearing = Inputs(policy, committed, evidence, 500000);
  stale_clearing.latch_clear_requested = true;
  auto refused = EvaluateMode(stale_clearing);
  DOM_CHECK_OK(refused);
  DOM_CHECK(!refused.value().recovery_preconditions_ok);
  DOM_CHECK(!refused.value().latch_cleared);
  DOM_CHECK(HasReason(refused.value().decision.trace, ReasonCode::LatchClearRefused));
}

DOM_TEST(recovery_step_limit_and_ladder_blocking_are_reported) {
  dom::test::PolicyOptions step_options;
  step_options.max_recovery_step = 1;
  const PolicyDocument policy = dom::test::MakePolicy(step_options);
  CommittedView committed = CommittedIn(policy, 5, Posture::Conserve, 0);
  committed.has_recovery_hold = true;
  committed.recovery_hold_start_ms = Instant();
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 100000;
  auto evidence = dom::test::MakeEvidence(options);
  auto limited = EvaluateMode(Inputs(policy, committed, evidence, 100000));
  DOM_CHECK_OK(limited);
  DOM_CHECK_EQ(limited.value().decision.verdict, Verdict::Hold);
  DOM_CHECK(HasReason(limited.value().decision.trace, ReasonCode::RecoveryStepLimit));

  // An intermediate mode that still fails its exit criteria blocks the descent.
  const PolicyDocument ladder = dom::test::MakePolicy();
  CommittedView restricted = CommittedIn(ladder, 6, Posture::Restricted, 0);
  restricted.has_recovery_hold = true;
  restricted.recovery_hold_start_ms = Instant();
  dom::test::EvidenceOptions advisory;
  advisory.observed_at_ms = 100000;
  advisory.severity = 1;
  advisory.redundancy = 1000;
  auto advisory_evidence = dom::test::MakeEvidence(advisory);
  auto blocked = EvaluateMode(Inputs(ladder, restricted, advisory_evidence, 100000));
  DOM_CHECK_OK(blocked);
  DOM_CHECK_EQ(blocked.value().decision.verdict, Verdict::Hold);
  DOM_CHECK_EQ(blocked.value().decision.posture, Posture::Restricted);
  DOM_CHECK(HasReason(blocked.value().decision.trace, ReasonCode::LadderStepBlocked));
}

DOM_TEST(structural_inputs_are_refused_without_mutating_state) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const CommittedView committed = CommittedIn(policy, 1, Posture::Nominal, 1000);
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 2000;
  auto evidence = dom::test::MakeEvidence(options);

  EvaluationInputs wrong_epoch = Inputs(policy, committed, evidence, 2000);
  wrong_epoch.context.epoch = ControlEpoch::FromValue(2);
  DOM_CHECK_ERR(EvaluateMode(wrong_epoch), ErrorCode::StaleAuthority);

  EvaluationInputs wrong_incarnation = Inputs(policy, committed, evidence, 2000);
  wrong_incarnation.context.incarnation = Incarnation::FromValue(9999);
  DOM_CHECK_ERR(EvaluateMode(wrong_incarnation), ErrorCode::StaleAuthority);

  CommittedView unknown = committed;
  unknown.mode = ModeId::FromValue(42);
  DOM_CHECK_ERR(EvaluateMode(Inputs(policy, unknown, evidence, 2000)), ErrorCode::NotFound);

  DOM_CHECK_ERR(EvaluateMode(Inputs(policy, committed, evidence, 500)), ErrorCode::InvalidArgument);

  EvaluationInputs no_policy = Inputs(policy, committed, evidence, 2000);
  no_policy.policy = nullptr;
  DOM_CHECK_ERR(EvaluateMode(no_policy), ErrorCode::InvalidArgument);
  EvaluationInputs no_evidence = Inputs(policy, committed, evidence, 2000);
  no_evidence.evidence = nullptr;
  DOM_CHECK_ERR(EvaluateMode(no_evidence), ErrorCode::InvalidArgument);

  // An unvalidated policy is validated by the engine itself.
  PolicyDocument broken = policy;
  broken.modes.clear();
  EvaluationInputs unvalidated = Inputs(policy, committed, evidence, 2000);
  unvalidated.policy = &broken;
  unvalidated.policy_validated = false;
  DOM_CHECK_ERR(EvaluateMode(unvalidated), ErrorCode::PolicyRejected);
}

DOM_TEST(escalation_from_an_indeterminate_posture_is_allowed) {
  const PolicyDocument policy = dom::test::MakePolicy();
  CommittedView committed = CommittedIn(policy, 5, Posture::Indeterminate, 0);
  committed.mode = ModeId::FromValue(5);
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 1000;
  options.severity = 3;
  options.redundancy = 400;
  auto evidence = dom::test::MakeEvidence(options);
  auto outcome = EvaluateMode(Inputs(policy, committed, evidence, 2000));
  DOM_CHECK_OK(outcome);
  DOM_CHECK_EQ(outcome.value().decision.verdict, Verdict::Escalate);
  DOM_CHECK_EQ(outcome.value().decision.mode.value(), 6ull);
  DOM_CHECK_EQ(outcome.value().decision.posture, Posture::Restricted);
}

DOM_TEST(unit_mismatch_never_coerces_a_criterion) {
  PolicyDocument policy = dom::test::MakePolicy();
  // The conserve criteria are expressed in watts while the evidence is a code,
  // so the numeric comparison that would otherwise hold must not be applied.
  for (ModeDefinition& mode : policy.modes) {
    if (mode.cls == OperatingClass::Conserve) {
      for (Predicate& predicate : mode.entry) {
        predicate.threshold = Scalar(Unit::Watts, 1);
      }
      for (Predicate& predicate : mode.exit) {
        predicate.threshold = Scalar(Unit::Watts, 1);
      }
    }
  }
  DOM_CHECK_OK(ValidatePolicy(policy));
  const CommittedView committed = CommittedIn(policy, 1, Posture::Nominal, 0);
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 1000;
  options.severity = 3;  // numerically higher than the threshold, but not comparable
  auto evidence = dom::test::MakeEvidence(options);
  auto outcome = EvaluateMode(Inputs(policy, committed, evidence, 2000));
  DOM_CHECK_OK(outcome);
  // The engine stops at the advisory class because the conserve criterion is
  // not comparable; a coerced comparison would have selected conserve.
  DOM_CHECK_EQ(outcome.value().decision.posture, Posture::Watch);
  DOM_CHECK_EQ(outcome.value().decision.mode.value(), 3ull);
}

DOM_TEST(future_dated_evidence_is_never_authoritative) {
  const PolicyDocument policy = dom::test::MakePolicy();
  CommittedView committed = CommittedIn(policy, 5, Posture::Conserve, 0);
  committed.has_recovery_hold = true;
  committed.recovery_hold_start_ms = Instant();
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 500000;  // observed after the evaluation instant
  auto evidence = dom::test::MakeEvidence(options);
  auto outcome = EvaluateMode(Inputs(policy, committed, evidence, 100000));
  DOM_CHECK_OK(outcome);
  DOM_CHECK_EQ(outcome.value().decision.verdict, Verdict::BecomeIndeterminate);
  DOM_CHECK(HasReason(outcome.value().decision.trace, ReasonCode::EvidenceFutureDated));
  DOM_CHECK_EQ(outcome.value().decision.posture, Posture::Indeterminate);
}

}  // namespace
