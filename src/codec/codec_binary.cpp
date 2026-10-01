// Degraded Operation Manager - canonical binary codec.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/codec.hpp"

#include <algorithm>
#include <string>

#include "codec/binary_reader.hpp"
#include "codec/binary_writer.hpp"
#include "codec/canonical.hpp"
#include "dom/limits.hpp"

namespace dom {
namespace {

constexpr std::uint32_t kPolicyPayloadVersion = 1;
constexpr std::uint32_t kEvidencePayloadVersion = 1;
constexpr std::uint32_t kStatePayloadVersion = 1;

Status PutPredicate(internal::BinaryWriter& writer, const Predicate& predicate) {
  Status status = writer.PutU8(static_cast<std::uint8_t>(predicate.cls));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutString(predicate.subject);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutString(predicate.metric);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(predicate.aggregation));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(predicate.comparator));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutScalar(predicate.threshold);
  if (!status.ok()) {
    return status;
  }
  return writer.PutBool(predicate.require_fresh);
}

Result<Predicate> TakePredicate(internal::BinaryReader& reader) {
  Predicate predicate;
  auto cls = internal::TakeEnum<EvidenceClass>(reader, 8, "evidence class");
  if (!cls) {
    return Result<Predicate>::Err(cls.status());
  }
  predicate.cls = cls.value();
  auto subject = reader.TakeString();
  if (!subject) {
    return Result<Predicate>::Err(subject.status());
  }
  predicate.subject = std::move(subject.value());
  auto metric = reader.TakeString();
  if (!metric) {
    return Result<Predicate>::Err(metric.status());
  }
  predicate.metric = std::move(metric.value());
  auto aggregation = internal::TakeEnum<Aggregation>(reader, 5, "aggregation");
  if (!aggregation) {
    return Result<Predicate>::Err(aggregation.status());
  }
  predicate.aggregation = aggregation.value();
  auto comparator = internal::TakeEnum<Comparator>(reader, 3, "comparator");
  if (!comparator) {
    return Result<Predicate>::Err(comparator.status());
  }
  predicate.comparator = comparator.value();
  auto threshold = reader.TakeScalar();
  if (!threshold) {
    return Result<Predicate>::Err(threshold.status());
  }
  predicate.threshold = threshold.value();
  auto fresh = reader.TakeBool();
  if (!fresh) {
    return Result<Predicate>::Err(fresh.status());
  }
  predicate.require_fresh = fresh.value();
  return Result<Predicate>::Ok(std::move(predicate));
}

Status PutRestrictionRule(internal::BinaryWriter& writer, const RestrictionRule& rule) {
  Status status = writer.PutU64(rule.id.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutString(rule.service_class);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(rule.kind));
  if (!status.ok()) {
    return status;
  }
  return writer.PutScalar(rule.allowance);
}

Result<RestrictionRule> TakeRestrictionRule(internal::BinaryReader& reader) {
  RestrictionRule rule;
  auto id = reader.TakeU64();
  if (!id) {
    return Result<RestrictionRule>::Err(id.status());
  }
  rule.id = RestrictionId::FromValue(id.value());
  auto service = reader.TakeString();
  if (!service) {
    return Result<RestrictionRule>::Err(service.status());
  }
  rule.service_class = std::move(service.value());
  auto kind = internal::TakeEnum<RestrictionKind>(reader, 3, "restriction kind");
  if (!kind) {
    return Result<RestrictionRule>::Err(kind.status());
  }
  rule.kind = kind.value();
  auto allowance = reader.TakeScalar();
  if (!allowance) {
    return Result<RestrictionRule>::Err(allowance.status());
  }
  rule.allowance = allowance.value();
  return Result<RestrictionRule>::Ok(std::move(rule));
}

Status PutRequirement(internal::BinaryWriter& writer, const EvidenceRequirement& requirement) {
  Status status = writer.PutU8(static_cast<std::uint8_t>(requirement.cls));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutString(requirement.subject);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutString(requirement.metric);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(requirement.scope));
  if (!status.ok()) {
    return status;
  }
  return writer.PutU64(requirement.max_age_ms.value());
}

Result<EvidenceRequirement> TakeRequirement(internal::BinaryReader& reader) {
  EvidenceRequirement requirement;
  auto cls = internal::TakeEnum<EvidenceClass>(reader, 8, "evidence class");
  if (!cls) {
    return Result<EvidenceRequirement>::Err(cls.status());
  }
  requirement.cls = cls.value();
  auto subject = reader.TakeString();
  if (!subject) {
    return Result<EvidenceRequirement>::Err(subject.status());
  }
  requirement.subject = std::move(subject.value());
  auto metric = reader.TakeString();
  if (!metric) {
    return Result<EvidenceRequirement>::Err(metric.status());
  }
  requirement.metric = std::move(metric.value());
  auto scope = internal::TakeEnum<RequirementScope>(reader, 3, "requirement scope");
  if (!scope) {
    return Result<EvidenceRequirement>::Err(scope.status());
  }
  requirement.scope = scope.value();
  auto age = reader.TakeU64();
  if (!age) {
    return Result<EvidenceRequirement>::Err(age.status());
  }
  requirement.max_age_ms = Duration::FromValue(age.value());
  return Result<EvidenceRequirement>::Ok(std::move(requirement));
}

bool PredicateLess(const Predicate& a, const Predicate& b) {
  if (a.cls != b.cls) {
    return static_cast<int>(a.cls) < static_cast<int>(b.cls);
  }
  if (a.subject != b.subject) {
    return a.subject < b.subject;
  }
  if (a.metric != b.metric) {
    return a.metric < b.metric;
  }
  if (a.aggregation != b.aggregation) {
    return static_cast<int>(a.aggregation) < static_cast<int>(b.aggregation);
  }
  if (a.comparator != b.comparator) {
    return static_cast<int>(a.comparator) < static_cast<int>(b.comparator);
  }
  if (a.threshold.unit() != b.threshold.unit()) {
    return static_cast<int>(a.threshold.unit()) < static_cast<int>(b.threshold.unit());
  }
  if (a.threshold.value() != b.threshold.value()) {
    return a.threshold.value() < b.threshold.value();
  }
  return static_cast<int>(a.require_fresh) < static_cast<int>(b.require_fresh);
}

bool RestrictionLess(const RestrictionRule& a, const RestrictionRule& b) { return a.id < b.id; }

bool ObligationLess(const ObligationClass& a, const ObligationClass& b) { return a.id < b.id; }

bool ModeLess(const ModeDefinition& a, const ModeDefinition& b) {
  const int rank_a = SeverityRank(a.cls);
  const int rank_b = SeverityRank(b.cls);
  if (rank_a != rank_b) {
    return rank_a < rank_b;
  }
  return a.id < b.id;
}

bool RequirementLess(const EvidenceRequirement& a, const EvidenceRequirement& b) {
  if (a.cls != b.cls) {
    return static_cast<int>(a.cls) < static_cast<int>(b.cls);
  }
  if (a.subject != b.subject) {
    return a.subject < b.subject;
  }
  if (a.metric != b.metric) {
    return a.metric < b.metric;
  }
  return static_cast<int>(a.scope) < static_cast<int>(b.scope);
}

bool HistoryLess(const HistoryEntry& a, const HistoryEntry& b) {
  if (a.state_sequence != b.state_sequence) {
    return a.state_sequence < b.state_sequence;
  }
  return a.decision_sequence < b.decision_sequence;
}

Status PutStringSequence(internal::BinaryWriter& writer, const std::vector<std::string>& items) {
  Status status = writer.PutCount(items.size());
  if (!status.ok()) {
    return status;
  }
  for (const std::string& item : items) {
    status = writer.PutString(item);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

Result<std::vector<std::string>> TakeStringSequence(internal::BinaryReader& reader,
                                                    std::uint32_t bound,
                                                    std::string_view what) {
  auto count = reader.TakeCount(bound, what);
  if (!count) {
    return Result<std::vector<std::string>>::Err(count.status());
  }
  std::vector<std::string> items;
  items.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto item = reader.TakeString();
    if (!item) {
      return Result<std::vector<std::string>>::Err(item.status());
    }
    items.push_back(std::move(item.value()));
  }
  return Result<std::vector<std::string>>::Ok(std::move(items));
}

}  // namespace

namespace internal {

Status PutCommittedView(BinaryWriter& writer, const CommittedView& committed) {
  Status status = writer.PutU64(committed.state_sequence.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(committed.decision_sequence.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(committed.epoch.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(committed.incarnation.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(committed.mode.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(committed.posture));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(committed.since_ms.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(committed.since_tick.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutBool(committed.has_recovery_hold);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(committed.recovery_hold_start_ms.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutBool(committed.latch.latched);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(committed.latch.mode.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(committed.latch.latched_at_ms.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(committed.latch.cause_digest);
  if (!status.ok()) {
    return status;
  }
  return writer.PutDigest(committed.last_decision_digest);
}

Result<CommittedView> TakeCommittedView(BinaryReader& reader) {
  CommittedView committed;
  auto state_sequence = reader.TakeU64();
  if (!state_sequence) {
    return Result<CommittedView>::Err(state_sequence.status());
  }
  committed.state_sequence = StateSequence::FromValue(state_sequence.value());
  auto decision_sequence = reader.TakeU64();
  if (!decision_sequence) {
    return Result<CommittedView>::Err(decision_sequence.status());
  }
  committed.decision_sequence = DecisionSequence::FromValue(decision_sequence.value());
  auto epoch = reader.TakeU64();
  if (!epoch) {
    return Result<CommittedView>::Err(epoch.status());
  }
  committed.epoch = ControlEpoch::FromValue(epoch.value());
  auto incarnation = reader.TakeU64();
  if (!incarnation) {
    return Result<CommittedView>::Err(incarnation.status());
  }
  committed.incarnation = Incarnation::FromValue(incarnation.value());
  auto mode = reader.TakeU64();
  if (!mode) {
    return Result<CommittedView>::Err(mode.status());
  }
  committed.mode = ModeId::FromValue(mode.value());
  auto posture = TakeEnum<Posture>(reader, 6, "posture");
  if (!posture) {
    return Result<CommittedView>::Err(posture.status());
  }
  committed.posture = posture.value();
  auto since = reader.TakeU64();
  if (!since) {
    return Result<CommittedView>::Err(since.status());
  }
  committed.since_ms = Instant::FromValue(since.value());
  auto tick = reader.TakeU64();
  if (!tick) {
    return Result<CommittedView>::Err(tick.status());
  }
  committed.since_tick = Tick::FromValue(tick.value());
  auto has_hold = reader.TakeBool();
  if (!has_hold) {
    return Result<CommittedView>::Err(has_hold.status());
  }
  committed.has_recovery_hold = has_hold.value();
  auto hold_start = reader.TakeU64();
  if (!hold_start) {
    return Result<CommittedView>::Err(hold_start.status());
  }
  committed.recovery_hold_start_ms = Instant::FromValue(hold_start.value());
  auto latched = reader.TakeBool();
  if (!latched) {
    return Result<CommittedView>::Err(latched.status());
  }
  committed.latch.latched = latched.value();
  auto latch_mode = reader.TakeU64();
  if (!latch_mode) {
    return Result<CommittedView>::Err(latch_mode.status());
  }
  committed.latch.mode = ModeId::FromValue(latch_mode.value());
  auto latched_at = reader.TakeU64();
  if (!latched_at) {
    return Result<CommittedView>::Err(latched_at.status());
  }
  committed.latch.latched_at_ms = Instant::FromValue(latched_at.value());
  auto cause = reader.TakeDigest();
  if (!cause) {
    return Result<CommittedView>::Err(cause.status());
  }
  committed.latch.cause_digest = cause.value();
  auto last = reader.TakeDigest();
  if (!last) {
    return Result<CommittedView>::Err(last.status());
  }
  committed.last_decision_digest = last.value();
  return Result<CommittedView>::Ok(std::move(committed));
}

Status PutRestrictionSet(BinaryWriter& writer, const RestrictionSet& set) {
  Status status = writer.PutCount(set.rules.size());
  if (!status.ok()) {
    return status;
  }
  for (const RestrictionRule& rule : set.rules) {
    status = PutRestrictionRule(writer, rule);
    if (!status.ok()) {
      return status;
    }
  }
  status = PutStringSequence(writer, set.restricted_services);
  if (!status.ok()) {
    return status;
  }
  return PutStringSequence(writer, set.permitted_services);
}

Result<RestrictionSet> TakeRestrictionSet(BinaryReader& reader) {
  RestrictionSet set;
  auto count = reader.TakeCount(limits::kMaxRestrictionsPerMode, "restriction");
  if (!count) {
    return Result<RestrictionSet>::Err(count.status());
  }
  set.rules.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto rule = TakeRestrictionRule(reader);
    if (!rule) {
      return Result<RestrictionSet>::Err(rule.status());
    }
    set.rules.push_back(std::move(rule.value()));
  }
  auto restricted = TakeStringSequence(reader, limits::kMaxServiceClasses, "service");
  if (!restricted) {
    return Result<RestrictionSet>::Err(restricted.status());
  }
  set.restricted_services = std::move(restricted.value());
  auto permitted = TakeStringSequence(reader, limits::kMaxServiceClasses, "service");
  if (!permitted) {
    return Result<RestrictionSet>::Err(permitted.status());
  }
  set.permitted_services = std::move(permitted.value());
  return Result<RestrictionSet>::Ok(std::move(set));
}

Status PutReasonTrace(BinaryWriter& writer, const ReasonTrace& trace) {
  Status status = writer.PutCount(trace.reasons().size());
  if (!status.ok()) {
    return status;
  }
  for (const Reason& reason : trace.reasons()) {
    status = writer.PutU16(static_cast<std::uint16_t>(reason.code));
    if (!status.ok()) {
      return status;
    }
    status = writer.PutU64(reason.mode.value());
    if (!status.ok()) {
      return status;
    }
    status = writer.PutBool(reason.has_class);
    if (!status.ok()) {
      return status;
    }
    status = writer.PutU8(reason.has_class ? static_cast<std::uint8_t>(reason.cls) : 0u);
    if (!status.ok()) {
      return status;
    }
    status = writer.PutString(reason.subject);
    if (!status.ok()) {
      return status;
    }
    status = writer.PutString(reason.metric);
    if (!status.ok()) {
      return status;
    }
    status = writer.PutBool(reason.has_observed);
    if (!status.ok()) {
      return status;
    }
    status = writer.PutI64(reason.observed);
    if (!status.ok()) {
      return status;
    }
    status = writer.PutString(reason.detail);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

Result<ReasonTrace> TakeReasonTrace(BinaryReader& reader) {
  ReasonTrace trace;
  auto count = reader.TakeCount(limits::kMaxCollectionItems, "reason");
  if (!count) {
    return Result<ReasonTrace>::Err(count.status());
  }
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    Reason reason;
    auto code = reader.TakeU16();
    if (!code) {
      return Result<ReasonTrace>::Err(code.status());
    }
    if (code.value() == 0 || code.value() > 57) {
      return Result<ReasonTrace>::Err(ErrorCode::DecodeError, "unknown reason code");
    }
    reason.code = static_cast<ReasonCode>(code.value());
    auto mode = reader.TakeU64();
    if (!mode) {
      return Result<ReasonTrace>::Err(mode.status());
    }
    reason.mode = ModeId::FromValue(mode.value());
    auto has_class = reader.TakeBool();
    if (!has_class) {
      return Result<ReasonTrace>::Err(has_class.status());
    }
    reason.has_class = has_class.value();
    auto cls = reader.TakeU8();
    if (!cls) {
      return Result<ReasonTrace>::Err(cls.status());
    }
    if (reason.has_class) {
      if (cls.value() == 0 || cls.value() > 8) {
        return Result<ReasonTrace>::Err(ErrorCode::DecodeError, "unknown evidence class code");
      }
      reason.cls = static_cast<EvidenceClass>(cls.value());
    } else if (cls.value() != 0) {
      return Result<ReasonTrace>::Err(ErrorCode::DecodeError,
                                      "absent evidence class must be encoded as zero");
    }
    auto subject = reader.TakeString();
    if (!subject) {
      return Result<ReasonTrace>::Err(subject.status());
    }
    reason.subject = std::move(subject.value());
    auto metric = reader.TakeString();
    if (!metric) {
      return Result<ReasonTrace>::Err(metric.status());
    }
    reason.metric = std::move(metric.value());
    auto has_observed = reader.TakeBool();
    if (!has_observed) {
      return Result<ReasonTrace>::Err(has_observed.status());
    }
    reason.has_observed = has_observed.value();
    auto observed = reader.TakeI64();
    if (!observed) {
      return Result<ReasonTrace>::Err(observed.status());
    }
    reason.observed = observed.value();
    auto detail = reader.TakeString();
    if (!detail) {
      return Result<ReasonTrace>::Err(detail.status());
    }
    reason.detail = std::move(detail.value());
    trace.Add(std::move(reason));
  }
  return Result<ReasonTrace>::Ok(std::move(trace));
}

Status PutGenerationVector(BinaryWriter& writer, const GenerationVector& generations) {
  Status status = writer.PutU64(generations.epoch.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(generations.incarnation.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(generations.policy.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(generations.evidence.value());
  if (!status.ok()) {
    return status;
  }
  for (std::size_t i = 0; i < kEvidenceClassCount; ++i) {
    status = writer.PutU64(generations.classes[i].value());
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

Result<GenerationVector> TakeGenerationVector(BinaryReader& reader) {
  GenerationVector generations;
  auto epoch = reader.TakeU64();
  if (!epoch) {
    return Result<GenerationVector>::Err(epoch.status());
  }
  generations.epoch = ControlEpoch::FromValue(epoch.value());
  auto incarnation = reader.TakeU64();
  if (!incarnation) {
    return Result<GenerationVector>::Err(incarnation.status());
  }
  generations.incarnation = Incarnation::FromValue(incarnation.value());
  auto policy = reader.TakeU64();
  if (!policy) {
    return Result<GenerationVector>::Err(policy.status());
  }
  generations.policy = PolicyGeneration::FromValue(policy.value());
  auto evidence = reader.TakeU64();
  if (!evidence) {
    return Result<GenerationVector>::Err(evidence.status());
  }
  generations.evidence = EvidenceRevision::FromValue(evidence.value());
  for (std::size_t i = 0; i < kEvidenceClassCount; ++i) {
    auto item = reader.TakeU64();
    if (!item) {
      return Result<GenerationVector>::Err(item.status());
    }
    generations.classes[i] = Generation::FromValue(item.value());
  }
  return Result<GenerationVector>::Ok(std::move(generations));
}

Status PutDecision(BinaryWriter& writer, const ModeDecision& decision) {
  Status status = writer.PutU64(decision.sequence.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(decision.mode.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(decision.posture));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(decision.verdict));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(decision.decided_at_ms.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(decision.tick.value());
  if (!status.ok()) {
    return status;
  }
  status = PutGenerationVector(writer, decision.generations);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(decision.policy_digest);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(decision.evidence_digest);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(decision.input_digest);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(decision.previous_decision_digest);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(decision.restrictions_digest);
  if (!status.ok()) {
    return status;
  }
  return PutReasonTrace(writer, decision.trace);
}

Result<ModeDecision> TakeDecision(BinaryReader& reader) {
  ModeDecision decision;
  auto sequence = reader.TakeU64();
  if (!sequence) {
    return Result<ModeDecision>::Err(sequence.status());
  }
  decision.sequence = DecisionSequence::FromValue(sequence.value());
  auto mode = reader.TakeU64();
  if (!mode) {
    return Result<ModeDecision>::Err(mode.status());
  }
  decision.mode = ModeId::FromValue(mode.value());
  auto posture = TakeEnum<Posture>(reader, 6, "posture");
  if (!posture) {
    return Result<ModeDecision>::Err(posture.status());
  }
  decision.posture = posture.value();
  auto verdict = TakeEnum<Verdict>(reader, 3, "verdict");
  if (!verdict) {
    return Result<ModeDecision>::Err(verdict.status());
  }
  decision.verdict = verdict.value();
  auto decided_at = reader.TakeU64();
  if (!decided_at) {
    return Result<ModeDecision>::Err(decided_at.status());
  }
  decision.decided_at_ms = Instant::FromValue(decided_at.value());
  auto tick = reader.TakeU64();
  if (!tick) {
    return Result<ModeDecision>::Err(tick.status());
  }
  decision.tick = Tick::FromValue(tick.value());
  auto generations = TakeGenerationVector(reader);
  if (!generations) {
    return Result<ModeDecision>::Err(generations.status());
  }
  decision.generations = generations.value();
  auto policy_digest = reader.TakeDigest();
  if (!policy_digest) {
    return Result<ModeDecision>::Err(policy_digest.status());
  }
  decision.policy_digest = policy_digest.value();
  auto evidence_digest = reader.TakeDigest();
  if (!evidence_digest) {
    return Result<ModeDecision>::Err(evidence_digest.status());
  }
  decision.evidence_digest = evidence_digest.value();
  auto input_digest = reader.TakeDigest();
  if (!input_digest) {
    return Result<ModeDecision>::Err(input_digest.status());
  }
  decision.input_digest = input_digest.value();
  auto previous = reader.TakeDigest();
  if (!previous) {
    return Result<ModeDecision>::Err(previous.status());
  }
  decision.previous_decision_digest = previous.value();
  auto restrictions = reader.TakeDigest();
  if (!restrictions) {
    return Result<ModeDecision>::Err(restrictions.status());
  }
  decision.restrictions_digest = restrictions.value();
  auto trace = TakeReasonTrace(reader);
  if (!trace) {
    return Result<ModeDecision>::Err(trace.status());
  }
  decision.trace = std::move(trace.value());
  return Result<ModeDecision>::Ok(std::move(decision));
}

Status PutHistoryEntry(BinaryWriter& writer, const HistoryEntry& entry) {
  Status status = writer.PutU64(entry.state_sequence.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(entry.decision_sequence.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(entry.mode.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(entry.posture));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(entry.verdict));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(entry.previous_mode.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(entry.previous_posture));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(entry.decided_at_ms.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(entry.decision_digest);
  if (!status.ok()) {
    return status;
  }
  return writer.PutDigest(entry.trace_digest);
}

Result<HistoryEntry> TakeHistoryEntry(BinaryReader& reader) {
  HistoryEntry entry;
  auto state_sequence = reader.TakeU64();
  if (!state_sequence) {
    return Result<HistoryEntry>::Err(state_sequence.status());
  }
  entry.state_sequence = StateSequence::FromValue(state_sequence.value());
  auto decision_sequence = reader.TakeU64();
  if (!decision_sequence) {
    return Result<HistoryEntry>::Err(decision_sequence.status());
  }
  entry.decision_sequence = DecisionSequence::FromValue(decision_sequence.value());
  auto mode = reader.TakeU64();
  if (!mode) {
    return Result<HistoryEntry>::Err(mode.status());
  }
  entry.mode = ModeId::FromValue(mode.value());
  auto posture = TakeEnum<Posture>(reader, 6, "posture");
  if (!posture) {
    return Result<HistoryEntry>::Err(posture.status());
  }
  entry.posture = posture.value();
  auto verdict = TakeEnum<Verdict>(reader, 3, "verdict");
  if (!verdict) {
    return Result<HistoryEntry>::Err(verdict.status());
  }
  entry.verdict = verdict.value();
  auto previous_mode = reader.TakeU64();
  if (!previous_mode) {
    return Result<HistoryEntry>::Err(previous_mode.status());
  }
  entry.previous_mode = ModeId::FromValue(previous_mode.value());
  auto previous_posture = TakeEnum<Posture>(reader, 6, "posture");
  if (!previous_posture) {
    return Result<HistoryEntry>::Err(previous_posture.status());
  }
  entry.previous_posture = previous_posture.value();
  auto decided_at = reader.TakeU64();
  if (!decided_at) {
    return Result<HistoryEntry>::Err(decided_at.status());
  }
  entry.decided_at_ms = Instant::FromValue(decided_at.value());
  auto decision_digest = reader.TakeDigest();
  if (!decision_digest) {
    return Result<HistoryEntry>::Err(decision_digest.status());
  }
  entry.decision_digest = decision_digest.value();
  auto trace_digest = reader.TakeDigest();
  if (!trace_digest) {
    return Result<HistoryEntry>::Err(trace_digest.status());
  }
  entry.trace_digest = trace_digest.value();
  return Result<HistoryEntry>::Ok(std::move(entry));
}

Status PutAuthorization(BinaryWriter& writer, const RestrictionAuthorization& authorization) {
  Status status = writer.PutU64(authorization.id.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(authorization.bound_decision.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(authorization.decision_digest);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(authorization.epoch.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(authorization.incarnation.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(authorization.issued_at_ms.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(authorization.expires_at_ms.value());
  if (!status.ok()) {
    return status;
  }
  return PutRestrictionSet(writer, authorization.restrictions);
}

Result<RestrictionAuthorization> TakeAuthorization(BinaryReader& reader) {
  RestrictionAuthorization authorization;
  auto id = reader.TakeU64();
  if (!id) {
    return Result<RestrictionAuthorization>::Err(id.status());
  }
  authorization.id = AuthorizationId::FromValue(id.value());
  auto bound = reader.TakeU64();
  if (!bound) {
    return Result<RestrictionAuthorization>::Err(bound.status());
  }
  authorization.bound_decision = DecisionSequence::FromValue(bound.value());
  auto decision_digest = reader.TakeDigest();
  if (!decision_digest) {
    return Result<RestrictionAuthorization>::Err(decision_digest.status());
  }
  authorization.decision_digest = decision_digest.value();
  auto epoch = reader.TakeU64();
  if (!epoch) {
    return Result<RestrictionAuthorization>::Err(epoch.status());
  }
  authorization.epoch = ControlEpoch::FromValue(epoch.value());
  auto incarnation = reader.TakeU64();
  if (!incarnation) {
    return Result<RestrictionAuthorization>::Err(incarnation.status());
  }
  authorization.incarnation = Incarnation::FromValue(incarnation.value());
  auto issued = reader.TakeU64();
  if (!issued) {
    return Result<RestrictionAuthorization>::Err(issued.status());
  }
  authorization.issued_at_ms = Instant::FromValue(issued.value());
  auto expires = reader.TakeU64();
  if (!expires) {
    return Result<RestrictionAuthorization>::Err(expires.status());
  }
  authorization.expires_at_ms = Instant::FromValue(expires.value());
  auto restrictions = TakeRestrictionSet(reader);
  if (!restrictions) {
    return Result<RestrictionAuthorization>::Err(restrictions.status());
  }
  authorization.restrictions = std::move(restrictions.value());
  return Result<RestrictionAuthorization>::Ok(std::move(authorization));
}

Status PutAcknowledgement(BinaryWriter& writer, const AcknowledgementRecord& record) {
  Status status = writer.PutU64(record.authorization.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutString(record.adjacent_owner);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(record.status));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(record.recorded_at_ms.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(record.decision_digest);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(record.epoch.value());
  if (!status.ok()) {
    return status;
  }
  return writer.PutString(record.detail);
}

Result<AcknowledgementRecord> TakeAcknowledgement(BinaryReader& reader) {
  AcknowledgementRecord record;
  auto authorization = reader.TakeU64();
  if (!authorization) {
    return Result<AcknowledgementRecord>::Err(authorization.status());
  }
  record.authorization = AuthorizationId::FromValue(authorization.value());
  auto owner = reader.TakeString();
  if (!owner) {
    return Result<AcknowledgementRecord>::Err(owner.status());
  }
  record.adjacent_owner = std::move(owner.value());
  auto acceptance = TakeEnum<AcceptanceStatus>(reader, 2, "acceptance status");
  if (!acceptance) {
    return Result<AcknowledgementRecord>::Err(acceptance.status());
  }
  record.status = acceptance.value();
  auto recorded_at = reader.TakeU64();
  if (!recorded_at) {
    return Result<AcknowledgementRecord>::Err(recorded_at.status());
  }
  record.recorded_at_ms = Instant::FromValue(recorded_at.value());
  auto decision_digest = reader.TakeDigest();
  if (!decision_digest) {
    return Result<AcknowledgementRecord>::Err(decision_digest.status());
  }
  record.decision_digest = decision_digest.value();
  auto epoch = reader.TakeU64();
  if (!epoch) {
    return Result<AcknowledgementRecord>::Err(epoch.status());
  }
  record.epoch = ControlEpoch::FromValue(epoch.value());
  auto detail = reader.TakeString();
  if (!detail) {
    return Result<AcknowledgementRecord>::Err(detail.status());
  }
  record.detail = std::move(detail.value());
  return Result<AcknowledgementRecord>::Ok(std::move(record));
}

Status PutVerification(BinaryWriter& writer, const EffectVerificationRecord& record) {
  Status status = writer.PutU64(record.authorization.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutString(record.adjacent_owner);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(record.outcome));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(record.observed_at_ms.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(record.effect_digest);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(record.source_digest);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutBool(record.stale_binding);
  if (!status.ok()) {
    return status;
  }
  return writer.PutString(record.detail);
}

Result<EffectVerificationRecord> TakeVerification(BinaryReader& reader) {
  EffectVerificationRecord record;
  auto authorization = reader.TakeU64();
  if (!authorization) {
    return Result<EffectVerificationRecord>::Err(authorization.status());
  }
  record.authorization = AuthorizationId::FromValue(authorization.value());
  auto owner = reader.TakeString();
  if (!owner) {
    return Result<EffectVerificationRecord>::Err(owner.status());
  }
  record.adjacent_owner = std::move(owner.value());
  auto outcome = TakeEnum<VerificationOutcome>(reader, 3, "verification outcome");
  if (!outcome) {
    return Result<EffectVerificationRecord>::Err(outcome.status());
  }
  record.outcome = outcome.value();
  auto observed_at = reader.TakeU64();
  if (!observed_at) {
    return Result<EffectVerificationRecord>::Err(observed_at.status());
  }
  record.observed_at_ms = Instant::FromValue(observed_at.value());
  auto effect = reader.TakeDigest();
  if (!effect) {
    return Result<EffectVerificationRecord>::Err(effect.status());
  }
  record.effect_digest = effect.value();
  auto source = reader.TakeDigest();
  if (!source) {
    return Result<EffectVerificationRecord>::Err(source.status());
  }
  record.source_digest = source.value();
  auto stale = reader.TakeBool();
  if (!stale) {
    return Result<EffectVerificationRecord>::Err(stale.status());
  }
  record.stale_binding = stale.value();
  auto detail = reader.TakeString();
  if (!detail) {
    return Result<EffectVerificationRecord>::Err(detail.status());
  }
  record.detail = std::move(detail.value());
  return Result<EffectVerificationRecord>::Ok(std::move(record));
}

Status PutIdempotency(BinaryWriter& writer, const IdempotencyEntry& entry) {
  Status status = writer.PutDigest(entry.key);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(entry.request_digest);
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU8(static_cast<std::uint8_t>(entry.operation));
  if (!status.ok()) {
    return status;
  }
  status = writer.PutU64(entry.resulting_sequence.value());
  if (!status.ok()) {
    return status;
  }
  status = writer.PutDigest(entry.result_digest);
  if (!status.ok()) {
    return status;
  }
  return writer.PutU64(entry.recorded_at_ms.value());
}

Result<IdempotencyEntry> TakeIdempotency(BinaryReader& reader) {
  IdempotencyEntry entry;
  auto key = reader.TakeDigest();
  if (!key) {
    return Result<IdempotencyEntry>::Err(key.status());
  }
  entry.key = key.value();
  auto request = reader.TakeDigest();
  if (!request) {
    return Result<IdempotencyEntry>::Err(request.status());
  }
  entry.request_digest = request.value();
  auto operation = TakeEnum<OperationKind>(reader, 6, "operation kind");
  if (!operation) {
    return Result<IdempotencyEntry>::Err(operation.status());
  }
  entry.operation = operation.value();
  auto sequence = reader.TakeU64();
  if (!sequence) {
    return Result<IdempotencyEntry>::Err(sequence.status());
  }
  entry.resulting_sequence = StateSequence::FromValue(sequence.value());
  auto result = reader.TakeDigest();
  if (!result) {
    return Result<IdempotencyEntry>::Err(result.status());
  }
  entry.result_digest = result.value();
  auto recorded_at = reader.TakeU64();
  if (!recorded_at) {
    return Result<IdempotencyEntry>::Err(recorded_at.status());
  }
  entry.recorded_at_ms = Instant::FromValue(recorded_at.value());
  return Result<IdempotencyEntry>::Ok(std::move(entry));
}

Result<std::vector<std::uint8_t>> EncodePolicyPayload(const PolicyDocument& policy) {
  PolicyDocument canonical = policy;
  CanonicalizePolicy(canonical);

  BinaryWriter writer;
  Status status = writer.PutU32(kPolicyPayloadVersion);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = writer.PutU64(canonical.id.value());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = writer.PutU64(canonical.generation.value());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = writer.PutString(canonical.revision_key);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = PutStringSequence(writer, canonical.service_classes);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = writer.PutCount(canonical.obligations.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const ObligationClass& obligation : canonical.obligations) {
    status = writer.PutU64(obligation.id.value());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutString(obligation.key);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutU8(static_cast<std::uint8_t>(obligation.protection));
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = PutStringSequence(writer, obligation.service_classes);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
  }
  status = writer.PutCount(canonical.modes.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const ModeDefinition& mode : canonical.modes) {
    status = writer.PutU64(mode.id.value());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutString(mode.key);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutU8(static_cast<std::uint8_t>(mode.cls));
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutU64(mode.revision.value());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutU8(static_cast<std::uint8_t>(mode.latch));
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutU64(mode.min_dwell_ms.value());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutU64(mode.recovery_hold_ms.value());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutCount(mode.entry.size());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    for (const Predicate& predicate : mode.entry) {
      status = PutPredicate(writer, predicate);
      if (!status.ok()) {
        return Result<std::vector<std::uint8_t>>::Err(status);
      }
    }
    status = writer.PutCount(mode.exit.size());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    for (const Predicate& predicate : mode.exit) {
      status = PutPredicate(writer, predicate);
      if (!status.ok()) {
        return Result<std::vector<std::uint8_t>>::Err(status);
      }
    }
    status = writer.PutCount(mode.restrictions.size());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    for (const RestrictionRule& rule : mode.restrictions) {
      status = PutRestrictionRule(writer, rule);
      if (!status.ok()) {
        return Result<std::vector<std::uint8_t>>::Err(status);
      }
    }
    status = writer.PutCount(mode.protected_obligations.size());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    for (const ObligationClassId id : mode.protected_obligations) {
      status = writer.PutU64(id.value());
      if (!status.ok()) {
        return Result<std::vector<std::uint8_t>>::Err(status);
      }
    }
  }
  status = writer.PutCount(canonical.indeterminate_restrictions.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const RestrictionRule& rule : canonical.indeterminate_restrictions) {
    status = PutRestrictionRule(writer, rule);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
  }
  status = writer.PutCount(canonical.requirements.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const EvidenceRequirement& requirement : canonical.requirements) {
    status = PutRequirement(writer, requirement);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
  }
  status = writer.PutU8(canonical.max_recovery_step);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = writer.PutU64(canonical.default_max_age_ms.value());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  return Result<std::vector<std::uint8_t>>::Ok(writer.Take());
}

Result<PolicyDocument> DecodePolicyPayload(std::span<const std::uint8_t> bytes) {
  BinaryReader reader(bytes);
  PolicyDocument policy;
  auto version = reader.TakeU32();
  if (!version) {
    return Result<PolicyDocument>::Err(version.status());
  }
  if (version.value() != kPolicyPayloadVersion) {
    return Result<PolicyDocument>::Err(ErrorCode::IncompatibleFormat,
                                       "unsupported policy payload version");
  }
  auto id = reader.TakeU64();
  if (!id) {
    return Result<PolicyDocument>::Err(id.status());
  }
  policy.id = PolicyId::FromValue(id.value());
  auto generation = reader.TakeU64();
  if (!generation) {
    return Result<PolicyDocument>::Err(generation.status());
  }
  policy.generation = PolicyGeneration::FromValue(generation.value());
  auto revision = reader.TakeString();
  if (!revision) {
    return Result<PolicyDocument>::Err(revision.status());
  }
  policy.revision_key = std::move(revision.value());

  auto services = TakeStringSequence(reader, limits::kMaxServiceClasses, "service class");
  if (!services) {
    return Result<PolicyDocument>::Err(services.status());
  }
  policy.service_classes = std::move(services.value());
  for (std::size_t i = 1; i < policy.service_classes.size(); ++i) {
    if (!(policy.service_classes[i - 1] < policy.service_classes[i])) {
      return Result<PolicyDocument>::Err(ErrorCode::DecodeError,
                                         "service classes are not in canonical order");
    }
  }

  auto obligation_count = reader.TakeCount(limits::kMaxObligations, "obligation");
  if (!obligation_count) {
    return Result<PolicyDocument>::Err(obligation_count.status());
  }
  policy.obligations.reserve(obligation_count.value());
  for (std::uint32_t i = 0; i < obligation_count.value(); ++i) {
    ObligationClass obligation;
    auto obligation_id = reader.TakeU64();
    if (!obligation_id) {
      return Result<PolicyDocument>::Err(obligation_id.status());
    }
    obligation.id = ObligationClassId::FromValue(obligation_id.value());
    auto key = reader.TakeString();
    if (!key) {
      return Result<PolicyDocument>::Err(key.status());
    }
    obligation.key = std::move(key.value());
    auto protection = TakeEnum<ProtectionLevel>(reader, 2, "protection level");
    if (!protection) {
      return Result<PolicyDocument>::Err(protection.status());
    }
    obligation.protection = protection.value();
    auto classes = TakeStringSequence(reader, limits::kMaxServiceClasses, "service class");
    if (!classes) {
      return Result<PolicyDocument>::Err(classes.status());
    }
    obligation.service_classes = std::move(classes.value());
    policy.obligations.push_back(std::move(obligation));
  }
  for (std::size_t i = 1; i < policy.obligations.size(); ++i) {
    if (!ObligationLess(policy.obligations[i - 1], policy.obligations[i])) {
      return Result<PolicyDocument>::Err(ErrorCode::DecodeError,
                                         "obligations are not in canonical order");
    }
  }

  auto mode_count = reader.TakeCount(limits::kMaxModes, "mode");
  if (!mode_count) {
    return Result<PolicyDocument>::Err(mode_count.status());
  }
  policy.modes.reserve(mode_count.value());
  for (std::uint32_t i = 0; i < mode_count.value(); ++i) {
    ModeDefinition mode;
    auto mode_id = reader.TakeU64();
    if (!mode_id) {
      return Result<PolicyDocument>::Err(mode_id.status());
    }
    mode.id = ModeId::FromValue(mode_id.value());
    auto key = reader.TakeString();
    if (!key) {
      return Result<PolicyDocument>::Err(key.status());
    }
    mode.key = std::move(key.value());
    auto cls = TakeEnum<OperatingClass>(reader, 5, "operating class");
    if (!cls) {
      return Result<PolicyDocument>::Err(cls.status());
    }
    mode.cls = cls.value();
    auto revision_id = reader.TakeU64();
    if (!revision_id) {
      return Result<PolicyDocument>::Err(revision_id.status());
    }
    mode.revision = RevisionId::FromValue(revision_id.value());
    auto latch = TakeEnum<LatchMode>(reader, 2, "latch mode");
    if (!latch) {
      return Result<PolicyDocument>::Err(latch.status());
    }
    mode.latch = latch.value();
    auto min_dwell = reader.TakeU64();
    if (!min_dwell) {
      return Result<PolicyDocument>::Err(min_dwell.status());
    }
    mode.min_dwell_ms = Duration::FromValue(min_dwell.value());
    auto recovery_hold = reader.TakeU64();
    if (!recovery_hold) {
      return Result<PolicyDocument>::Err(recovery_hold.status());
    }
    mode.recovery_hold_ms = Duration::FromValue(recovery_hold.value());

    auto entry_count = reader.TakeCount(limits::kMaxPredicatesPerMode, "predicate");
    if (!entry_count) {
      return Result<PolicyDocument>::Err(entry_count.status());
    }
    mode.entry.reserve(entry_count.value());
    for (std::uint32_t k = 0; k < entry_count.value(); ++k) {
      auto predicate = TakePredicate(reader);
      if (!predicate) {
        return Result<PolicyDocument>::Err(predicate.status());
      }
      mode.entry.push_back(std::move(predicate.value()));
    }
    for (std::size_t k = 1; k < mode.entry.size(); ++k) {
      if (!PredicateLess(mode.entry[k - 1], mode.entry[k])) {
        return Result<PolicyDocument>::Err(ErrorCode::DecodeError,
                                           "entry predicates are not in canonical order");
      }
    }
    auto exit_count = reader.TakeCount(limits::kMaxPredicatesPerMode, "predicate");
    if (!exit_count) {
      return Result<PolicyDocument>::Err(exit_count.status());
    }
    mode.exit.reserve(exit_count.value());
    for (std::uint32_t k = 0; k < exit_count.value(); ++k) {
      auto predicate = TakePredicate(reader);
      if (!predicate) {
        return Result<PolicyDocument>::Err(predicate.status());
      }
      mode.exit.push_back(std::move(predicate.value()));
    }
    for (std::size_t k = 1; k < mode.exit.size(); ++k) {
      if (!PredicateLess(mode.exit[k - 1], mode.exit[k])) {
        return Result<PolicyDocument>::Err(ErrorCode::DecodeError,
                                           "exit predicates are not in canonical order");
      }
    }
    auto restriction_count = reader.TakeCount(limits::kMaxRestrictionsPerMode, "restriction");
    if (!restriction_count) {
      return Result<PolicyDocument>::Err(restriction_count.status());
    }
    mode.restrictions.reserve(restriction_count.value());
    for (std::uint32_t k = 0; k < restriction_count.value(); ++k) {
      auto rule = TakeRestrictionRule(reader);
      if (!rule) {
        return Result<PolicyDocument>::Err(rule.status());
      }
      mode.restrictions.push_back(std::move(rule.value()));
    }
    for (std::size_t k = 1; k < mode.restrictions.size(); ++k) {
      if (!RestrictionLess(mode.restrictions[k - 1], mode.restrictions[k])) {
        return Result<PolicyDocument>::Err(ErrorCode::DecodeError,
                                           "mode restrictions are not in canonical order");
      }
    }
    auto protected_count = reader.TakeCount(limits::kMaxObligations, "obligation");
    if (!protected_count) {
      return Result<PolicyDocument>::Err(protected_count.status());
    }
    mode.protected_obligations.reserve(protected_count.value());
    for (std::uint32_t k = 0; k < protected_count.value(); ++k) {
      auto obligation_id = reader.TakeU64();
      if (!obligation_id) {
        return Result<PolicyDocument>::Err(obligation_id.status());
      }
      mode.protected_obligations.push_back(ObligationClassId::FromValue(obligation_id.value()));
    }
    for (std::size_t k = 1; k < mode.protected_obligations.size(); ++k) {
      if (!(mode.protected_obligations[k - 1] < mode.protected_obligations[k])) {
        return Result<PolicyDocument>::Err(
            ErrorCode::DecodeError, "protected obligations are not in canonical order");
      }
    }
    policy.modes.push_back(std::move(mode));
  }
  for (std::size_t i = 1; i < policy.modes.size(); ++i) {
    if (!ModeLess(policy.modes[i - 1], policy.modes[i])) {
      return Result<PolicyDocument>::Err(ErrorCode::DecodeError,
                                         "modes are not in canonical order");
    }
  }

  auto indeterminate_count = reader.TakeCount(limits::kMaxRestrictionsPerMode, "restriction");
  if (!indeterminate_count) {
    return Result<PolicyDocument>::Err(indeterminate_count.status());
  }
  policy.indeterminate_restrictions.reserve(indeterminate_count.value());
  for (std::uint32_t i = 0; i < indeterminate_count.value(); ++i) {
    auto rule = TakeRestrictionRule(reader);
    if (!rule) {
      return Result<PolicyDocument>::Err(rule.status());
    }
    policy.indeterminate_restrictions.push_back(std::move(rule.value()));
  }
  for (std::size_t i = 1; i < policy.indeterminate_restrictions.size(); ++i) {
    if (!RestrictionLess(policy.indeterminate_restrictions[i - 1],
                         policy.indeterminate_restrictions[i])) {
      return Result<PolicyDocument>::Err(
          ErrorCode::DecodeError, "indeterminate restrictions are not in canonical order");
    }
  }

  auto requirement_count = reader.TakeCount(limits::kMaxCollectionItems, "requirement");
  if (!requirement_count) {
    return Result<PolicyDocument>::Err(requirement_count.status());
  }
  policy.requirements.reserve(requirement_count.value());
  for (std::uint32_t i = 0; i < requirement_count.value(); ++i) {
    auto requirement = TakeRequirement(reader);
    if (!requirement) {
      return Result<PolicyDocument>::Err(requirement.status());
    }
    policy.requirements.push_back(std::move(requirement.value()));
  }
  for (std::size_t i = 1; i < policy.requirements.size(); ++i) {
    if (!RequirementLess(policy.requirements[i - 1], policy.requirements[i])) {
      return Result<PolicyDocument>::Err(ErrorCode::DecodeError,
                                         "requirements are not in canonical order");
    }
  }

  auto max_step = reader.TakeU8();
  if (!max_step) {
    return Result<PolicyDocument>::Err(max_step.status());
  }
  policy.max_recovery_step = max_step.value();
  auto default_age = reader.TakeU64();
  if (!default_age) {
    return Result<PolicyDocument>::Err(default_age.status());
  }
  policy.default_max_age_ms = Duration::FromValue(default_age.value());

  Status end = reader.ExpectEnd();
  if (!end.ok()) {
    return Result<PolicyDocument>::Err(end);
  }
  return Result<PolicyDocument>::Ok(std::move(policy));
}

Result<std::vector<std::uint8_t>> EncodeEvidencePayload(
    const std::vector<EvidenceRecord>& records, EvidenceRevision revision) {
  BinaryWriter writer;
  Status status = writer.PutU32(kEvidencePayloadVersion);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = writer.PutU64(revision.value());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = writer.PutCount(records.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const EvidenceRecord& record : records) {
    status = writer.PutU8(static_cast<std::uint8_t>(record.cls));
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutString(record.subject);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutString(record.metric);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutScalar(record.value);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutU64(record.observed_at_ms.value());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutU64(record.valid_until_ms.value());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutU64(record.generation.value());
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutDigest(record.source_digest);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
    status = writer.PutString(record.producer);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
  }
  return Result<std::vector<std::uint8_t>>::Ok(writer.Take());
}

Result<std::vector<EvidenceRecord>> DecodeEvidencePayloadRecords(
    std::span<const std::uint8_t> bytes, EvidenceRevision& revision) {
  BinaryReader reader(bytes);
  auto version = reader.TakeU32();
  if (!version) {
    return Result<std::vector<EvidenceRecord>>::Err(version.status());
  }
  if (version.value() != kEvidencePayloadVersion) {
    return Result<std::vector<EvidenceRecord>>::Err(
        ErrorCode::IncompatibleFormat, "unsupported evidence payload version");
  }
  auto revision_value = reader.TakeU64();
  if (!revision_value) {
    return Result<std::vector<EvidenceRecord>>::Err(revision_value.status());
  }
  revision = EvidenceRevision::FromValue(revision_value.value());
  auto count = reader.TakeCount(limits::kMaxEvidenceRecords, "evidence record");
  if (!count) {
    return Result<std::vector<EvidenceRecord>>::Err(count.status());
  }
  std::vector<EvidenceRecord> records;
  records.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    EvidenceRecord record;
    auto cls = TakeEnum<EvidenceClass>(reader, 8, "evidence class");
    if (!cls) {
      return Result<std::vector<EvidenceRecord>>::Err(cls.status());
    }
    record.cls = cls.value();
    auto subject = reader.TakeString();
    if (!subject) {
      return Result<std::vector<EvidenceRecord>>::Err(subject.status());
    }
    record.subject = std::move(subject.value());
    auto metric = reader.TakeString();
    if (!metric) {
      return Result<std::vector<EvidenceRecord>>::Err(metric.status());
    }
    record.metric = std::move(metric.value());
    auto value = reader.TakeScalar();
    if (!value) {
      return Result<std::vector<EvidenceRecord>>::Err(value.status());
    }
    record.value = value.value();
    auto observed = reader.TakeU64();
    if (!observed) {
      return Result<std::vector<EvidenceRecord>>::Err(observed.status());
    }
    record.observed_at_ms = Instant::FromValue(observed.value());
    auto valid_until = reader.TakeU64();
    if (!valid_until) {
      return Result<std::vector<EvidenceRecord>>::Err(valid_until.status());
    }
    record.valid_until_ms = Instant::FromValue(valid_until.value());
    auto generation = reader.TakeU64();
    if (!generation) {
      return Result<std::vector<EvidenceRecord>>::Err(generation.status());
    }
    record.generation = Generation::FromValue(generation.value());
    auto source = reader.TakeDigest();
    if (!source) {
      return Result<std::vector<EvidenceRecord>>::Err(source.status());
    }
    record.source_digest = source.value();
    auto producer = reader.TakeString();
    if (!producer) {
      return Result<std::vector<EvidenceRecord>>::Err(producer.status());
    }
    record.producer = std::move(producer.value());
    records.push_back(std::move(record));
  }
  Status end = reader.ExpectEnd();
  if (!end.ok()) {
    return Result<std::vector<EvidenceRecord>>::Err(end);
  }
  return Result<std::vector<EvidenceRecord>>::Ok(std::move(records));
}

Result<std::vector<std::uint8_t>> EncodeStatePayload(const PersistedState& state) {
  BinaryWriter writer;
  Status status = writer.PutU32(kStatePayloadVersion);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = PutCommittedView(writer, state.committed);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = PutDecision(writer, state.last_decision);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = writer.PutCount(state.history.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const HistoryEntry& entry : state.history) {
    status = PutHistoryEntry(writer, entry);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
  }
  status = writer.PutCount(state.authorizations.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const RestrictionAuthorization& authorization : state.authorizations) {
    status = PutAuthorization(writer, authorization);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
  }
  status = writer.PutCount(state.acknowledgements.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const AcknowledgementRecord& record : state.acknowledgements) {
    status = PutAcknowledgement(writer, record);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
  }
  status = writer.PutCount(state.verifications.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const EffectVerificationRecord& record : state.verifications) {
    status = PutVerification(writer, record);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
  }
  status = writer.PutCount(state.idempotency.size());
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  for (const IdempotencyEntry& entry : state.idempotency) {
    status = PutIdempotency(writer, entry);
    if (!status.ok()) {
      return Result<std::vector<std::uint8_t>>::Err(status);
    }
  }
  status = writer.PutU64(state.committed_operations);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  status = writer.PutU64(state.evaluations);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>::Err(status);
  }
  return Result<std::vector<std::uint8_t>>::Ok(writer.Take());
}

Result<PersistedState> DecodeStatePayload(std::span<const std::uint8_t> bytes) {
  BinaryReader reader(bytes);
  PersistedState state;
  auto version = reader.TakeU32();
  if (!version) {
    return Result<PersistedState>::Err(version.status());
  }
  if (version.value() != kStatePayloadVersion) {
    return Result<PersistedState>::Err(ErrorCode::IncompatibleFormat,
                                       "unsupported state payload version");
  }
  auto committed = TakeCommittedView(reader);
  if (!committed) {
    return Result<PersistedState>::Err(committed.status());
  }
  state.committed = committed.value();
  auto decision = TakeDecision(reader);
  if (!decision) {
    return Result<PersistedState>::Err(decision.status());
  }
  state.last_decision = std::move(decision.value());

  auto history_count = reader.TakeCount(limits::kMaxHistoryEntries, "history entry");
  if (!history_count) {
    return Result<PersistedState>::Err(history_count.status());
  }
  state.history.reserve(history_count.value());
  for (std::uint32_t i = 0; i < history_count.value(); ++i) {
    auto entry = TakeHistoryEntry(reader);
    if (!entry) {
      return Result<PersistedState>::Err(entry.status());
    }
    if (!state.history.empty() && !HistoryLess(state.history.back(), entry.value())) {
      return Result<PersistedState>::Err(ErrorCode::DecodeError,
                                         "history is not in strictly increasing order");
    }
    state.history.push_back(std::move(entry.value()));
  }

  auto authorization_count = reader.TakeCount(limits::kMaxAuthorityRecords, "authorization");
  if (!authorization_count) {
    return Result<PersistedState>::Err(authorization_count.status());
  }
  state.authorizations.reserve(authorization_count.value());
  for (std::uint32_t i = 0; i < authorization_count.value(); ++i) {
    auto authorization = TakeAuthorization(reader);
    if (!authorization) {
      return Result<PersistedState>::Err(authorization.status());
    }
    state.authorizations.push_back(std::move(authorization.value()));
  }

  auto acknowledgement_count = reader.TakeCount(limits::kMaxAuthorityRecords, "acknowledgement");
  if (!acknowledgement_count) {
    return Result<PersistedState>::Err(acknowledgement_count.status());
  }
  state.acknowledgements.reserve(acknowledgement_count.value());
  for (std::uint32_t i = 0; i < acknowledgement_count.value(); ++i) {
    auto record = TakeAcknowledgement(reader);
    if (!record) {
      return Result<PersistedState>::Err(record.status());
    }
    state.acknowledgements.push_back(std::move(record.value()));
  }

  auto verification_count = reader.TakeCount(limits::kMaxAuthorityRecords, "verification");
  if (!verification_count) {
    return Result<PersistedState>::Err(verification_count.status());
  }
  state.verifications.reserve(verification_count.value());
  for (std::uint32_t i = 0; i < verification_count.value(); ++i) {
    auto record = TakeVerification(reader);
    if (!record) {
      return Result<PersistedState>::Err(record.status());
    }
    state.verifications.push_back(std::move(record.value()));
  }

  auto idempotency_count = reader.TakeCount(limits::kMaxIdempotencyEntries, "idempotency entry");
  if (!idempotency_count) {
    return Result<PersistedState>::Err(idempotency_count.status());
  }
  state.idempotency.reserve(idempotency_count.value());
  for (std::uint32_t i = 0; i < idempotency_count.value(); ++i) {
    auto entry = TakeIdempotency(reader);
    if (!entry) {
      return Result<PersistedState>::Err(entry.status());
    }
    state.idempotency.push_back(std::move(entry.value()));
  }

  auto operations = reader.TakeU64();
  if (!operations) {
    return Result<PersistedState>::Err(operations.status());
  }
  state.committed_operations = operations.value();
  auto evaluations = reader.TakeU64();
  if (!evaluations) {
    return Result<PersistedState>::Err(evaluations.status());
  }
  state.evaluations = evaluations.value();

  Status end = reader.ExpectEnd();
  if (!end.ok()) {
    return Result<PersistedState>::Err(end);
  }
  return Result<PersistedState>::Ok(std::move(state));
}

void CanonicalizePolicy(PolicyDocument& policy) {
  std::sort(policy.service_classes.begin(), policy.service_classes.end());
  for (ObligationClass& obligation : policy.obligations) {
    std::sort(obligation.service_classes.begin(), obligation.service_classes.end());
  }
  std::sort(policy.obligations.begin(), policy.obligations.end(), ObligationLess);
  for (ModeDefinition& mode : policy.modes) {
    std::sort(mode.entry.begin(), mode.entry.end(), PredicateLess);
    std::sort(mode.exit.begin(), mode.exit.end(), PredicateLess);
    std::sort(mode.restrictions.begin(), mode.restrictions.end(), RestrictionLess);
    std::sort(mode.protected_obligations.begin(), mode.protected_obligations.end());
  }
  std::sort(policy.modes.begin(), policy.modes.end(), ModeLess);
  std::sort(policy.indeterminate_restrictions.begin(), policy.indeterminate_restrictions.end(),
            RestrictionLess);
  std::sort(policy.requirements.begin(), policy.requirements.end(), RequirementLess);
}

}  // namespace internal

Result<std::vector<std::uint8_t>> EncodePolicyBinary(const PolicyDocument& policy) {
  return internal::EncodePolicyPayload(policy);
}

Result<PolicyDocument> DecodePolicyBinary(std::span<const std::uint8_t> bytes) {
  return internal::DecodePolicyPayload(bytes);
}

Result<std::vector<std::uint8_t>> EncodeEvidenceBinary(const EvidenceSnapshot& snapshot) {
  return internal::EncodeEvidencePayload(snapshot.records(), snapshot.revision());
}

Result<EvidenceSnapshot> DecodeEvidenceBinary(std::span<const std::uint8_t> bytes) {
  EvidenceRevision revision;
  auto records = internal::DecodeEvidencePayloadRecords(bytes, revision);
  if (!records) {
    return Result<EvidenceSnapshot>::Err(records.status());
  }
  return EvidenceSnapshot::Build(std::move(records.value()), revision);
}

Digest GenerationVector::digest() const {
  internal::BinaryWriter writer;
  Status status = internal::PutGenerationVector(writer, *this);
  if (!status.ok()) {
    return Digest::Zero();
  }
  return Sha256::Of(writer.bytes());
}

Digest RestrictionSet::digest() const {
  internal::BinaryWriter writer;
  Status status = internal::PutRestrictionSet(writer, *this);
  if (!status.ok()) {
    return Digest::Zero();
  }
  return Sha256::Of(writer.bytes());
}

Digest ModeDecision::digest() const {
  internal::BinaryWriter writer;
  Status status = internal::PutDecision(writer, *this);
  if (!status.ok()) {
    return Digest::Zero();
  }
  return Sha256::Of(writer.bytes());
}

Digest RestrictionAuthorization::digest() const {
  internal::BinaryWriter writer;
  Status status = internal::PutAuthorization(writer, *this);
  if (!status.ok()) {
    return Digest::Zero();
  }
  return Sha256::Of(writer.bytes());
}

Digest AcknowledgementRecord::digest() const {
  internal::BinaryWriter writer;
  const Status encoded = internal::PutAcknowledgement(writer, *this);
  if (!encoded.ok()) {
    return Digest::Zero();
  }
  return Sha256::Of(writer.bytes());
}

Digest EffectVerificationRecord::digest() const {
  internal::BinaryWriter writer;
  Status status = internal::PutVerification(writer, *this);
  if (!status.ok()) {
    return Digest::Zero();
  }
  return Sha256::Of(writer.bytes());
}

Digest ReasonTrace::digest() const {
  internal::BinaryWriter writer;
  Status status = internal::PutReasonTrace(writer, *this);
  if (!status.ok()) {
    return Digest::Zero();
  }
  return Sha256::Of(writer.bytes());
}

Digest PersistedState::digest() const {
  auto payload = internal::EncodeStatePayload(*this);
  if (!payload) {
    return Digest::Zero();
  }
  return Sha256::Of(payload.value());
}

Digest PolicyDigest(const PolicyDocument& policy) {
  auto payload = internal::EncodePolicyPayload(policy);
  if (!payload) {
    return Digest::Zero();
  }
  return Sha256::Of(payload.value());
}

}  // namespace dom
