// Degraded Operation Manager - deterministic mode evaluation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/engine.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "codec/binary_writer.hpp"
#include "codec/canonical.hpp"
#include "dom/limits.hpp"

namespace dom {
namespace {

/// Traces are bounded so a hostile or degenerate policy cannot grow durable
/// state without limit. Reasons are added in a deterministic order, so a
/// truncated trace is still reproducible.
constexpr std::size_t kMaxTraceReasons = 256;

struct Gate {
  const EvidenceRequirement* requirement = nullptr;
  SelectionPosture posture;
};

struct ModeCriteria {
  const ModeDefinition* mode = nullptr;
  std::vector<PredicateOutcome> entry;
  std::vector<PredicateOutcome> exit;
  bool entry_satisfied = false;
  bool exit_satisfied = false;
};

ReasonCode FreshnessReasonCode(Freshness freshness) {
  switch (freshness) {
    case Freshness::Fresh:
      return ReasonCode::EvidenceFresh;
    case Freshness::Stale:
      return ReasonCode::EvidenceStale;
    case Freshness::Expired:
      return ReasonCode::EvidenceStale;
    case Freshness::FutureDated:
      return ReasonCode::EvidenceFutureDated;
    case Freshness::Missing:
      return ReasonCode::EvidenceMissing;
    case Freshness::Conflicted:
      return ReasonCode::EvidenceConflicted;
  }
  return ReasonCode::EvidenceUnusable;
}

void AddGateReasons(ReasonTrace& trace, const std::vector<Gate>& gates) {
  for (const Gate& gate : gates) {
    if (trace.size() >= kMaxTraceReasons) {
      return;
    }
    Reason reason;
    reason.code = FreshnessReasonCode(gate.posture.freshness);
    reason.has_class = true;
    reason.cls = gate.requirement->cls;
    reason.subject = gate.requirement->subject;
    reason.metric = gate.requirement->metric;
    reason.detail = std::string("requirement scope=") +
                    RequirementScopeName(gate.requirement->scope) + " " + gate.posture.detail;
    trace.Add(std::move(reason));
  }
}

void AddCriterionReasons(ReasonTrace& trace, const ModeDefinition& mode, bool entry,
                         const std::vector<PredicateOutcome>& outcomes) {
  for (std::size_t i = 0; i < outcomes.size(); ++i) {
    if (trace.size() >= kMaxTraceReasons) {
      return;
    }
    const PredicateOutcome& outcome = outcomes[i];
    const Predicate& predicate = entry ? mode.entry[i] : mode.exit[i];
    Reason reason;
    if (!outcome.resolvable) {
      reason.code = ReasonCode::PredicateUnresolvable;
    } else if (entry) {
      reason.code = outcome.satisfied ? ReasonCode::EntrySatisfied : ReasonCode::EntryUnsatisfied;
    } else {
      reason.code = outcome.satisfied ? ReasonCode::ExitSatisfied : ReasonCode::ExitUnsatisfied;
    }
    reason.mode = mode.id;
    reason.has_class = true;
    reason.cls = predicate.cls;
    reason.subject = predicate.subject;
    reason.metric = predicate.metric;
    reason.has_observed = true;
    reason.observed = static_cast<std::int64_t>(outcome.matched);
    reason.detail = outcome.detail;
    trace.Add(std::move(reason));
  }
}

void AddModeReason(ReasonTrace& trace, ReasonCode code, const ModeDefinition& mode,
                   std::string detail) {
  if (trace.size() >= kMaxTraceReasons) {
    return;
  }
  Reason reason;
  reason.code = code;
  reason.mode = mode.id;
  reason.detail = std::move(detail);
  trace.Add(std::move(reason));
}

const ObligationClass* FindObligationById(const PolicyDocument& policy, ObligationClassId id) {
  for (const ObligationClass& obligation : policy.obligations) {
    if (obligation.id == id) {
      return &obligation;
    }
  }
  return nullptr;
}

bool ContainsService(const std::vector<std::string>& services, const std::string& service) {
  return std::find(services.begin(), services.end(), service) != services.end();
}

RestrictionSet BuildRestrictionSet(const PolicyDocument& policy,
                                   std::vector<RestrictionRule> rules) {
  RestrictionSet set;
  std::sort(rules.begin(), rules.end(),
            [](const RestrictionRule& a, const RestrictionRule& b) { return a.id < b.id; });
  rules.erase(std::unique(rules.begin(), rules.end(),
                          [](const RestrictionRule& a, const RestrictionRule& b) {
                            return a.id == b.id;
                          }),
              rules.end());
  set.rules = std::move(rules);
  for (const RestrictionRule& rule : set.rules) {
    if (!ContainsService(set.restricted_services, rule.service_class)) {
      set.restricted_services.push_back(rule.service_class);
    }
  }
  std::sort(set.restricted_services.begin(), set.restricted_services.end());
  for (const std::string& service : policy.service_classes) {
    if (!ContainsService(set.restricted_services, service)) {
      set.permitted_services.push_back(service);
    }
  }
  return set;
}

Digest ComputeInputDigest(const PolicyDocument& policy, const EvidenceSnapshot& evidence,
                          const CommittedView& committed, const EvaluationContext& context,
                          const GenerationVector& generations, bool latch_clear_requested) {
  internal::BinaryWriter writer;
  Status status = internal::PutGenerationVector(writer, generations);
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutDigest(PolicyDigest(policy));
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutDigest(evidence.digest());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(committed.state_sequence.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(committed.decision_sequence.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(committed.mode.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU8(static_cast<std::uint8_t>(committed.posture));
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(committed.since_ms.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(context.now_ms.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutU64(context.tick.value());
  if (!status.ok()) {
    return Digest::Zero();
  }
  status = writer.PutBool(latch_clear_requested);
  if (!status.ok()) {
    return Digest::Zero();
  }
  return Sha256::Of(writer.bytes());
}

}  // namespace

RestrictionSet RestrictionsForMode(const PolicyDocument& policy, const ModeDefinition& mode) {
  std::vector<RestrictionRule> effective;
  effective.reserve(mode.restrictions.size());
  for (const RestrictionRule& rule : mode.restrictions) {
    bool blocked = false;
    for (const ObligationClassId id : mode.protected_obligations) {
      const ObligationClass* obligation = FindObligationById(policy, id);
      if (obligation == nullptr || obligation->protection == ProtectionLevel::BestEffort) {
        continue;
      }
      if (rule.kind == RestrictionKind::Deny &&
          ContainsService(obligation->service_classes, rule.service_class)) {
        blocked = true;
        break;
      }
    }
    if (!blocked) {
      effective.push_back(rule);
    }
  }
  return BuildRestrictionSet(policy, std::move(effective));
}

RestrictionSet UnionRestrictions(const PolicyDocument& policy, const RestrictionSet& a,
                                 const RestrictionSet& b) {
  std::vector<RestrictionRule> rules = a.rules;
  rules.insert(rules.end(), b.rules.begin(), b.rules.end());
  RestrictionSet merged = BuildRestrictionSet(policy, std::move(rules));
  for (const std::string& service : b.restricted_services) {
    if (!ContainsService(merged.restricted_services, service)) {
      merged.restricted_services.push_back(service);
    }
  }
  std::sort(merged.restricted_services.begin(), merged.restricted_services.end());
  merged.permitted_services.clear();
  for (const std::string& service : policy.service_classes) {
    if (!ContainsService(merged.restricted_services, service)) {
      merged.permitted_services.push_back(service);
    }
  }
  return merged;
}

Result<EvaluationOutcome> EvaluateMode(const EvaluationInputs& inputs) {
  if (inputs.policy == nullptr || inputs.evidence == nullptr) {
    return Result<EvaluationOutcome>::Err(ErrorCode::InvalidArgument,
                                          "evaluation requires a policy and an evidence snapshot");
  }
  const PolicyDocument& policy = *inputs.policy;
  const EvidenceSnapshot& evidence = *inputs.evidence;
  const CommittedView& committed = inputs.committed;
  const EvaluationContext& context = inputs.context;

  if (!inputs.policy_validated) {
    Status validation = ValidatePolicy(policy);
    if (!validation.ok()) {
      return Result<EvaluationOutcome>::Err(validation);
    }
  }
  if (context.epoch != committed.epoch || context.incarnation != committed.incarnation) {
    return Result<EvaluationOutcome>::Err(
        ErrorCode::StaleAuthority,
        "evaluation context does not own the committed state (epoch or incarnation mismatch)");
  }
  auto current_mode_result = FindMode(policy, committed.mode);
  if (!current_mode_result) {
    return Result<EvaluationOutcome>::Err(current_mode_result.status());
  }
  const ModeDefinition& current = *current_mode_result.value();
  if (context.now_ms < committed.since_ms) {
    return Result<EvaluationOutcome>::Err(
        ErrorCode::InvalidArgument, "evaluation instant precedes the committed mode start");
  }

  std::vector<AgeRule> ages;
  ages.reserve(policy.requirements.size());
  for (const EvidenceRequirement& requirement : policy.requirements) {
    AgeRule rule;
    rule.cls = requirement.cls;
    rule.subject = requirement.subject;
    rule.metric = requirement.metric;
    rule.max_age_ms = requirement.max_age_ms;
    ages.push_back(std::move(rule));
  }
  const EvidenceView view(evidence, context.now_ms, policy.default_max_age_ms, ages);

  std::vector<Gate> escalation_blockers;
  std::vector<Gate> recovery_blockers;
  for (const EvidenceRequirement& requirement : policy.requirements) {
    Gate gate;
    gate.requirement = &requirement;
    gate.posture = view.PostureFor(requirement.cls, requirement.subject, requirement.metric);
    if (gate.posture.freshness == Freshness::Fresh) {
      continue;
    }
    if (CoversEscalation(requirement.scope)) {
      escalation_blockers.push_back(gate);
    }
    if (CoversRecovery(requirement.scope)) {
      recovery_blockers.push_back(gate);
    }
  }

  const std::vector<const ModeDefinition*> ordered = policy.OrderedModes();
  std::vector<ModeCriteria> criteria;
  criteria.reserve(policy.modes.size());
  for (const ModeDefinition& mode : policy.modes) {
    ModeCriteria entry;
    entry.mode = &mode;
    entry.entry.reserve(mode.entry.size());
    bool entry_satisfied = true;
    for (const Predicate& predicate : mode.entry) {
      PredicateOutcome outcome = EvaluatePredicate(predicate, view);
      entry_satisfied = entry_satisfied && outcome.resolvable && outcome.satisfied;
      entry.entry.push_back(std::move(outcome));
    }
    entry.entry_satisfied = entry_satisfied;
    entry.exit.reserve(mode.exit.size());
    bool exit_satisfied = true;
    for (const Predicate& predicate : mode.exit) {
      PredicateOutcome outcome = EvaluatePredicate(predicate, view);
      exit_satisfied = exit_satisfied && outcome.resolvable && outcome.satisfied;
      entry.exit.push_back(std::move(outcome));
    }
    entry.exit_satisfied = exit_satisfied;
    criteria.push_back(std::move(entry));
  }
  const auto criteria_for = [&criteria](const ModeDefinition& mode) -> const ModeCriteria& {
    for (const ModeCriteria& item : criteria) {
      if (item.mode == &mode) {
        return item;
      }
    }
    return criteria.front();
  };

  const ModeCriteria& current_criteria = criteria_for(current);
  const int current_rank = EffectiveRank(committed.posture, current.cls);

  ReasonTrace trace;
  const ModeDefinition* escalation_target = nullptr;
  for (const ModeDefinition* mode : ordered) {
    const int rank = SeverityRank(mode->cls);
    if (rank <= current_rank) {
      continue;
    }
    if (!criteria_for(*mode).entry_satisfied) {
      continue;
    }
    if (escalation_target == nullptr || rank > SeverityRank(escalation_target->cls)) {
      escalation_target = mode;
    }
  }

  const bool recovery_gate_ok = recovery_blockers.empty();
  const bool latched = committed.latch.latched;
  const bool latch_clear_requested = inputs.latch_clear_requested;
  auto since_dwell = Elapsed(committed.since_ms, context.now_ms);
  if (!since_dwell) {
    return Result<EvaluationOutcome>::Err(since_dwell.status());
  }
  const bool dwell_ok = since_dwell.value() >= current.min_dwell_ms;

  const ModeDefinition* descent_target = nullptr;
  const ModeDefinition* ladder_blocker = nullptr;
  bool step_limited = false;
  for (const ModeDefinition* candidate : ordered) {
    const int candidate_rank = SeverityRank(candidate->cls);
    if (candidate_rank >= current_rank) {
      continue;
    }
    if (!criteria_for(*candidate).entry_satisfied) {
      continue;
    }
    const ModeDefinition* blocker = nullptr;
    if (!current_criteria.exit_satisfied) {
      blocker = &current;
    } else {
      for (const ModeDefinition* middle : ordered) {
        const int middle_rank = SeverityRank(middle->cls);
        if (middle_rank > candidate_rank && middle_rank < current_rank &&
            !criteria_for(*middle).exit_satisfied) {
          blocker = middle;
          break;
        }
      }
    }
    if (blocker != nullptr) {
      if (ladder_blocker == nullptr ||
          SeverityRank(blocker->cls) < SeverityRank(ladder_blocker->cls)) {
        ladder_blocker = blocker;
      }
      continue;
    }
    if (current_rank - candidate_rank > static_cast<int>(policy.max_recovery_step)) {
      step_limited = true;
      continue;
    }
    if (descent_target == nullptr ||
        candidate_rank < SeverityRank(descent_target->cls) ||
        (candidate_rank == SeverityRank(descent_target->cls) &&
         candidate->id < descent_target->id)) {
      descent_target = candidate;
    }
  }

  EvaluationOutcome outcome;
  outcome.next = committed;
  outcome.decision.generations.epoch = context.epoch;
  outcome.decision.generations.incarnation = context.incarnation;
  outcome.decision.generations.policy = policy.generation;
  outcome.decision.generations.evidence = evidence.revision();
  for (const EvidenceClass cls : AllEvidenceClasses()) {
    outcome.decision.generations.Set(cls, evidence.ClassGeneration(cls));
  }
  outcome.decision.policy_digest = PolicyDigest(policy);
  outcome.decision.evidence_digest = evidence.digest();
  outcome.decision.decided_at_ms = context.now_ms;
  outcome.decision.tick = context.tick;
  outcome.decision.previous_decision_digest = committed.last_decision_digest;
  outcome.recovery_preconditions_ok =
      recovery_gate_ok && current_criteria.exit_satisfied && dwell_ok && descent_target != nullptr;

  if (outcome.recovery_preconditions_ok) {
    outcome.next.has_recovery_hold = true;
    outcome.next.recovery_hold_start_ms =
        committed.has_recovery_hold ? committed.recovery_hold_start_ms : context.now_ms;
  } else {
    outcome.next.has_recovery_hold = false;
    outcome.next.recovery_hold_start_ms = Instant();
  }

  Duration hold_elapsed;
  bool hold_ok = false;
  if (outcome.next.has_recovery_hold) {
    auto elapsed = Elapsed(outcome.next.recovery_hold_start_ms, context.now_ms);
    if (!elapsed) {
      return Result<EvaluationOutcome>::Err(elapsed.status());
    }
    hold_elapsed = elapsed.value();
    hold_ok = hold_elapsed >= current.recovery_hold_ms;
  }

  LatchMode latch_mode = LatchMode::None;
  bool latch_blocks = false;
  if (latched) {
    const auto latch_definition = FindMode(policy, committed.latch.mode);
    latch_mode =
        latch_definition ? latch_definition.value()->latch : LatchMode::UntilExplicitClear;
    latch_blocks = latch_mode != LatchMode::UntilRecoveryPermitted;
  }

  if (escalation_target != nullptr) {
    // Escalation proceeds on whatever fresh evidence satisfies the entry
    // criteria, but the trace still names every declared escalation
    // requirement that was not fresh when the decision was made.
    AddGateReasons(trace, escalation_blockers);
    const ModeDefinition& target = *escalation_target;
    outcome.decision.verdict = Verdict::Escalate;
    outcome.decision.mode = target.id;
    outcome.decision.posture = PostureOf(target.cls);
    outcome.next.mode = target.id;
    outcome.next.posture = PostureOf(target.cls);
    outcome.next.since_ms = context.now_ms;
    outcome.next.since_tick = context.tick;
    outcome.next.has_recovery_hold = false;
    outcome.next.recovery_hold_start_ms = Instant();
    if (target.latch == LatchMode::None) {
      outcome.next.latch = LatchState();
      if (latched) {
        outcome.latch_cleared = true;
        AddModeReason(trace, ReasonCode::LatchCleared, current,
                      "latch released because a more restrictive mode superseded it");
      }
    } else {
      outcome.next.latch.latched = true;
      outcome.next.latch.mode = target.id;
      outcome.next.latch.latched_at_ms = context.now_ms;
    }
    AddModeReason(trace, ReasonCode::Escalated, target,
                  "entry criteria are satisfied for a more restrictive mode");
    AddCriterionReasons(trace, target, true, criteria_for(target).entry);
    outcome.restrictions = RestrictionsForMode(policy, target);
  } else if (latch_clear_requested && latched) {
    // An explicit clear never changes the mode: it either releases the latch or
    // is refused with the exact reasons, including unusable evidence.
    if (!recovery_gate_ok) {
      AddGateReasons(trace, recovery_blockers);
    } else {
      AddGateReasons(trace, escalation_blockers);
    }
    outcome.decision.verdict = Verdict::Hold;
    outcome.decision.mode = committed.mode;
    outcome.decision.posture = committed.posture;
    if (outcome.recovery_preconditions_ok) {
      outcome.next.latch = LatchState();
      outcome.latch_cleared = true;
      AddModeReason(trace, ReasonCode::LatchCleared, current,
                    "explicit latch clear accepted; the mode itself is unchanged until the "
                    "next evaluation");
    } else {
      AddModeReason(trace, ReasonCode::LatchClearRefused, current,
                    "recovery preconditions do not hold, so the latch stays in force");
    }
    outcome.restrictions = RestrictionsForMode(policy, current);
  } else if (!recovery_gate_ok) {
    AddGateReasons(trace, recovery_blockers);
    if (IsIndeterminate(committed.posture)) {
      outcome.decision.verdict = Verdict::Hold;
      AddModeReason(trace, ReasonCode::Held, current,
                    "remains indeterminate: declared recovery evidence is not fresh");
    } else {
      outcome.decision.verdict = Verdict::BecomeIndeterminate;
      AddModeReason(trace, ReasonCode::BecameIndeterminate, current,
                    "declared recovery evidence is not fresh; the restrictions already in "
                    "force are retained and cannot be relaxed");
    }
    outcome.decision.mode = committed.mode;
    outcome.decision.posture = Posture::Indeterminate;
    outcome.next.mode = committed.mode;
    outcome.next.posture = Posture::Indeterminate;
    if (!IsIndeterminate(committed.posture)) {
      outcome.next.since_ms = context.now_ms;
      outcome.next.since_tick = context.tick;
    }
    outcome.indeterminate = true;
    const RestrictionSet base = RestrictionsForMode(policy, current);
    const RestrictionSet indeterminate =
        BuildRestrictionSet(policy, policy.indeterminate_restrictions);
    outcome.restrictions = UnionRestrictions(policy, base, indeterminate);
  } else {
    AddGateReasons(trace, escalation_blockers);
    if (latched) {
      AddModeReason(trace, ReasonCode::LatchActive, current,
                    latch_blocks ? "mode is latched until an explicit clear"
                                 : "mode is latched until recovery preconditions hold");
    }
    const bool can_descend = descent_target != nullptr && dwell_ok && hold_ok && !latch_blocks;
    if (can_descend) {
      const ModeDefinition& target = *descent_target;
      outcome.decision.verdict = Verdict::Deescalate;
      outcome.decision.mode = target.id;
      outcome.decision.posture = PostureOf(target.cls);
      outcome.next.mode = target.id;
      outcome.next.posture = PostureOf(target.cls);
      outcome.next.since_ms = context.now_ms;
      outcome.next.since_tick = context.tick;
      outcome.next.has_recovery_hold = false;
      outcome.next.recovery_hold_start_ms = Instant();
      AddCriterionReasons(trace, current, false, current_criteria.exit);
      AddCriterionReasons(trace, target, true, criteria_for(target).entry);
      AddModeReason(trace, ReasonCode::RecoveryHoldSatisfied, current,
                    "recovery hold satisfied continuously with fresh evidence");
      AddModeReason(trace, ReasonCode::Deescalated, target,
                    "exit criteria, dwell and recovery hold satisfied");
      if (target.latch == LatchMode::None) {
        if (latched) {
          AddModeReason(trace, ReasonCode::LatchCleared, current,
                        "latch released by an automatic recovery transition");
          outcome.latch_cleared = true;
        }
        outcome.next.latch = LatchState();
      } else {
        outcome.next.latch.latched = true;
        outcome.next.latch.mode = target.id;
        outcome.next.latch.latched_at_ms = context.now_ms;
      }
      outcome.restrictions = RestrictionsForMode(policy, target);
    } else {
      outcome.decision.verdict = Verdict::Hold;
      outcome.decision.mode = committed.mode;
      outcome.decision.posture = committed.posture;
      if (!current_criteria.exit_satisfied) {
        AddCriterionReasons(trace, current, false, current_criteria.exit);
        AddModeReason(trace, ReasonCode::Held, current, "exit criteria are not satisfied");
      } else if (descent_target == nullptr) {
        if (step_limited) {
          AddModeReason(trace, ReasonCode::RecoveryStepLimit, current,
                        "recovery may descend at most the configured number of classes per "
                        "evaluation");
        } else if (ladder_blocker != nullptr) {
          AddModeReason(trace, ReasonCode::LadderStepBlocked, *ladder_blocker,
                        "an intermediate mode still fails its exit criteria");
        } else {
          AddModeReason(trace, ReasonCode::Held, current,
                        "no less restrictive mode has satisfied entry criteria");
        }
      } else if (latch_blocks) {
        AddModeReason(trace, ReasonCode::LatchActive, current,
                      "recovery is blocked until the latch is cleared with authority");
      } else if (!dwell_ok) {
        AddModeReason(trace, ReasonCode::MinDwellActive, current,
                      "minimum dwell in the current mode has not elapsed");
      } else {
        AddModeReason(trace, ReasonCode::RecoveryHoldActive, current,
                      "recovery hold of " + current.recovery_hold_ms.ToString() +
                          "ms has reached " + hold_elapsed.ToString() + "ms");
      }
      outcome.restrictions = RestrictionsForMode(policy, current);
    }
  }

  if (outcome.decision.posture == Posture::Indeterminate) {
    outcome.indeterminate = true;
  }
  outcome.decision.restrictions_digest = outcome.restrictions.digest();
  outcome.decision.input_digest = ComputeInputDigest(policy, evidence, committed, context,
                                                     outcome.decision.generations,
                                                     inputs.latch_clear_requested);
  trace.Canonicalize();
  outcome.decision.trace = trace;
  outcome.changed = !(outcome.next == committed);
  return Result<EvaluationOutcome>::Ok(std::move(outcome));
}

}  // namespace dom
