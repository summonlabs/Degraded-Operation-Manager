// Degraded Operation Manager - policy vocabulary, validation and criteria.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/policy.hpp"

#include <algorithm>
#include <string>

#include "dom/limits.hpp"

namespace dom {
namespace {

constexpr OperatingClass kAllClasses[kOperatingClassCount] = {
    OperatingClass::Nominal, OperatingClass::Watch,    OperatingClass::Conserve,
    OperatingClass::Restricted, OperatingClass::Critical, OperatingClass::Emergency,
};

constexpr Posture kAllPostures[7] = {
    Posture::Nominal, Posture::Watch,     Posture::Conserve, Posture::Restricted,
    Posture::Critical, Posture::Emergency, Posture::Indeterminate,
};

Status Reject(const std::string& detail) {
  return Status::Error(ErrorCode::PolicyRejected, detail);
}

bool HasServiceClass(const PolicyDocument& policy, const std::string& service) {
  return std::find(policy.service_classes.begin(), policy.service_classes.end(), service) !=
         policy.service_classes.end();
}

const ObligationClass* FindObligation(const PolicyDocument& policy, ObligationClassId id) {
  for (const ObligationClass& obligation : policy.obligations) {
    if (obligation.id == id) {
      return &obligation;
    }
  }
  return nullptr;
}

bool SameSelector(const Predicate& a, const Predicate& b) {
  return a.cls == b.cls && a.subject == b.subject && a.metric == b.metric &&
         a.aggregation == b.aggregation;
}

Status ValidatePredicateShape(const Predicate& predicate, const std::string& where) {
  if (predicate.subject.empty() || predicate.metric.empty()) {
    return Reject(where + ": predicate selector must not be empty");
  }
  if (predicate.subject.size() > limits::kMaxStringBytes ||
      predicate.metric.size() > limits::kMaxStringBytes) {
    return Reject(where + ": predicate selector exceeds the byte limit");
  }
  if (predicate.threshold.unit() == Unit::None) {
    return Reject(where + ": predicate threshold must carry a unit");
  }
  if (predicate.aggregation == Aggregation::Count &&
      predicate.threshold.unit() != Unit::Count) {
    return Reject(where + ": count aggregation requires a count threshold");
  }
  return Status::Ok();
}

Status ValidateRestriction(const PolicyDocument& policy, const RestrictionRule& rule,
                           const std::string& where) {
  if (!rule.id.IsValid()) {
    return Reject(where + ": restriction id must be non-zero");
  }
  if (rule.service_class.empty()) {
    return Reject(where + ": restriction service class must not be empty");
  }
  if (!HasServiceClass(policy, rule.service_class)) {
    return Reject(where + ": restriction names an unknown service class");
  }
  switch (rule.kind) {
    case RestrictionKind::Deny:
      if (rule.allowance.unit() != Unit::None && rule.allowance.unit() != Unit::Count) {
        return Reject(where + ": deny allowance must be unitless or a count");
      }
      break;
    case RestrictionKind::Throttle:
    case RestrictionKind::Degrade:
      if (rule.allowance.unit() != Unit::MilliPercent) {
        return Reject(where + ": throttle and degrade allowances are milli-percent");
      }
      if (rule.allowance.value() < 0 || rule.allowance.value() > 100000) {
        return Reject(where + ": allowance must be between 0 and 100000 milli-percent");
      }
      break;
    case RestrictionKind::Defer:
      if (rule.allowance.unit() != Unit::Milliseconds) {
        return Reject(where + ": defer allowance is a duration in milliseconds");
      }
      break;
  }
  return Status::Ok();
}

int FreshnessSeverity(Freshness freshness) {
  switch (freshness) {
    case Freshness::Fresh:
      return 0;
    case Freshness::Stale:
      return 1;
    case Freshness::Expired:
      return 2;
    case Freshness::FutureDated:
      return 3;
    case Freshness::Missing:
      return 4;
    case Freshness::Conflicted:
      return 5;
  }
  return 5;
}

bool CompareSatisfies(Comparator comparator, int comparison) {
  switch (comparator) {
    case Comparator::AtLeast:
      return comparison >= 0;
    case Comparator::AtMost:
      return comparison <= 0;
    case Comparator::Equals:
      return comparison == 0;
    case Comparator::NotEquals:
      return comparison != 0;
  }
  return false;
}

}  // namespace

const char* OperatingClassName(OperatingClass cls) noexcept {
  switch (cls) {
    case OperatingClass::Nominal:
      return "nominal";
    case OperatingClass::Watch:
      return "watch";
    case OperatingClass::Conserve:
      return "conserve";
    case OperatingClass::Restricted:
      return "restricted";
    case OperatingClass::Critical:
      return "critical";
    case OperatingClass::Emergency:
      return "emergency";
  }
  return "unknown";
}

Result<OperatingClass> OperatingClassFromName(std::string_view name) {
  for (OperatingClass cls : kAllClasses) {
    if (name == OperatingClassName(cls)) {
      return Result<OperatingClass>::Ok(cls);
    }
  }
  return Result<OperatingClass>::Err(ErrorCode::InvalidArgument, "unknown operating class name");
}

std::span<const OperatingClass> AllOperatingClasses() {
  return std::span<const OperatingClass>(kAllClasses, kOperatingClassCount);
}

int SeverityRank(OperatingClass cls) noexcept { return static_cast<int>(cls); }

const char* PostureName(Posture posture) noexcept {
  switch (posture) {
    case Posture::Nominal:
      return "nominal";
    case Posture::Watch:
      return "watch";
    case Posture::Conserve:
      return "conserve";
    case Posture::Restricted:
      return "restricted";
    case Posture::Critical:
      return "critical";
    case Posture::Emergency:
      return "emergency";
    case Posture::Indeterminate:
      return "indeterminate";
  }
  return "unknown";
}

Result<Posture> PostureFromName(std::string_view name) {
  for (Posture posture : kAllPostures) {
    if (name == PostureName(posture)) {
      return Result<Posture>::Ok(posture);
    }
  }
  return Result<Posture>::Err(ErrorCode::InvalidArgument, "unknown posture name");
}

Posture PostureOf(OperatingClass cls) noexcept { return static_cast<Posture>(cls); }

bool IsIndeterminate(Posture posture) noexcept { return posture == Posture::Indeterminate; }

int EffectiveRank(Posture posture, OperatingClass retained) noexcept {
  if (posture == Posture::Indeterminate) {
    return SeverityRank(retained);
  }
  return static_cast<int>(posture);
}

const char* RequirementScopeName(RequirementScope scope) noexcept {
  switch (scope) {
    case RequirementScope::Escalation:
      return "escalation";
    case RequirementScope::Recovery:
      return "recovery";
    case RequirementScope::Both:
      return "both";
  }
  return "unknown";
}

Result<RequirementScope> RequirementScopeFromName(std::string_view name) {
  for (std::uint8_t code = 1; code <= 3; ++code) {
    const auto scope = static_cast<RequirementScope>(code);
    if (name == RequirementScopeName(scope)) {
      return Result<RequirementScope>::Ok(scope);
    }
  }
  return Result<RequirementScope>::Err(ErrorCode::InvalidArgument, "unknown requirement scope");
}

bool CoversEscalation(RequirementScope scope) noexcept {
  return scope == RequirementScope::Escalation || scope == RequirementScope::Both;
}

bool CoversRecovery(RequirementScope scope) noexcept {
  return scope == RequirementScope::Recovery || scope == RequirementScope::Both;
}

const char* LatchModeName(LatchMode mode) noexcept {
  switch (mode) {
    case LatchMode::None:
      return "none";
    case LatchMode::UntilExplicitClear:
      return "until-explicit-clear";
    case LatchMode::UntilRecoveryPermitted:
      return "until-recovery-permitted";
  }
  return "unknown";
}

Result<LatchMode> LatchModeFromName(std::string_view name) {
  for (std::uint8_t code = 0; code <= 2; ++code) {
    const auto mode = static_cast<LatchMode>(code);
    if (name == LatchModeName(mode)) {
      return Result<LatchMode>::Ok(mode);
    }
  }
  return Result<LatchMode>::Err(ErrorCode::InvalidArgument, "unknown latch mode");
}

const char* ProtectionLevelName(ProtectionLevel level) noexcept {
  switch (level) {
    case ProtectionLevel::Inviolable:
      return "inviolable";
    case ProtectionLevel::Protected:
      return "protected";
    case ProtectionLevel::BestEffort:
      return "best-effort";
  }
  return "unknown";
}

Result<ProtectionLevel> ProtectionLevelFromName(std::string_view name) {
  for (std::uint8_t code = 0; code <= 2; ++code) {
    const auto level = static_cast<ProtectionLevel>(code);
    if (name == ProtectionLevelName(level)) {
      return Result<ProtectionLevel>::Ok(level);
    }
  }
  return Result<ProtectionLevel>::Err(ErrorCode::InvalidArgument, "unknown protection level");
}

const char* RestrictionKindName(RestrictionKind kind) noexcept {
  switch (kind) {
    case RestrictionKind::Deny:
      return "deny";
    case RestrictionKind::Throttle:
      return "throttle";
    case RestrictionKind::Defer:
      return "defer";
    case RestrictionKind::Degrade:
      return "degrade";
  }
  return "unknown";
}

Result<RestrictionKind> RestrictionKindFromName(std::string_view name) {
  for (std::uint8_t code = 0; code <= 3; ++code) {
    const auto kind = static_cast<RestrictionKind>(code);
    if (name == RestrictionKindName(kind)) {
      return Result<RestrictionKind>::Ok(kind);
    }
  }
  return Result<RestrictionKind>::Err(ErrorCode::InvalidArgument, "unknown restriction kind");
}

const char* AggregationName(Aggregation aggregation) noexcept {
  switch (aggregation) {
    case Aggregation::Any:
      return "any";
    case Aggregation::All:
      return "all";
    case Aggregation::Count:
      return "count";
    case Aggregation::Maximum:
      return "maximum";
    case Aggregation::Minimum:
      return "minimum";
    case Aggregation::Sum:
      return "sum";
  }
  return "unknown";
}

Result<Aggregation> AggregationFromName(std::string_view name) {
  for (std::uint8_t code = 0; code <= 5; ++code) {
    const auto aggregation = static_cast<Aggregation>(code);
    if (name == AggregationName(aggregation)) {
      return Result<Aggregation>::Ok(aggregation);
    }
  }
  return Result<Aggregation>::Err(ErrorCode::InvalidArgument, "unknown aggregation name");
}

const char* ComparatorName(Comparator comparator) noexcept {
  switch (comparator) {
    case Comparator::AtLeast:
      return "at-least";
    case Comparator::AtMost:
      return "at-most";
    case Comparator::Equals:
      return "equals";
    case Comparator::NotEquals:
      return "not-equals";
  }
  return "unknown";
}

Result<Comparator> ComparatorFromName(std::string_view name) {
  for (std::uint8_t code = 0; code <= 3; ++code) {
    const auto comparator = static_cast<Comparator>(code);
    if (name == ComparatorName(comparator)) {
      return Result<Comparator>::Ok(comparator);
    }
  }
  return Result<Comparator>::Err(ErrorCode::InvalidArgument, "unknown comparator name");
}

bool operator==(const Predicate& a, const Predicate& b) {
  return a.cls == b.cls && a.subject == b.subject && a.metric == b.metric &&
         a.aggregation == b.aggregation && a.comparator == b.comparator &&
         a.threshold == b.threshold && a.require_fresh == b.require_fresh;
}

std::string Predicate::ToString() const {
  std::string text = EvidenceClassName(cls);
  text += " ";
  text += subject;
  text += "/";
  text += metric;
  text += " ";
  text += AggregationName(aggregation);
  text += " ";
  text += ComparatorName(comparator);
  text += " ";
  text += threshold.ToString();
  text += require_fresh ? " fresh" : " any-age";
  return text;
}

PredicateOutcome EvaluatePredicate(const Predicate& predicate, const EvidenceView& view) {
  PredicateOutcome outcome;
  const std::vector<const KeyPosture*> postures =
      view.Select(predicate.cls, predicate.subject, predicate.metric, predicate.require_fresh);
  outcome.considered = postures.size();
  outcome.worst_freshness = view.PostureFor(predicate.cls, predicate.subject,
                                            predicate.metric).freshness;

  if (postures.empty()) {
    if (predicate.aggregation == Aggregation::Count) {
      auto comparison = CompareScalar(Scalar::Count(0), predicate.threshold);
      if (!comparison) {
        outcome.resolvable = false;
        outcome.detail = comparison.status().message();
        return outcome;
      }
      outcome.resolvable = true;
      outcome.satisfied = CompareSatisfies(predicate.comparator, comparison.value());
      outcome.detail = "no usable records; count evaluated as zero";
      return outcome;
    }
    if (predicate.aggregation == Aggregation::Any) {
      outcome.resolvable = true;
      outcome.satisfied = false;
      outcome.detail = "no usable records";
      return outcome;
    }
    outcome.resolvable = false;
    outcome.satisfied = false;
    outcome.detail = "no usable records for an aggregate predicate";
    return outcome;
  }

  std::size_t comparable = 0;
  std::size_t matched = 0;
  bool unit_mismatch = false;
  bool aggregate_overflow = false;
  Scalar aggregate;
  bool has_aggregate = false;
  int worst = 0;
  for (const KeyPosture* posture : postures) {
    worst = (std::max)(worst, FreshnessSeverity(posture->freshness));
    auto comparison = CompareScalar(posture->value, predicate.threshold);
    if (!comparison) {
      unit_mismatch = true;
      continue;
    }
    ++comparable;
    if (CompareSatisfies(predicate.comparator, comparison.value())) {
      ++matched;
    }
    switch (predicate.aggregation) {
      case Aggregation::Maximum:
        if (!has_aggregate || comparison.value() > 0) {
          aggregate = posture->value;
          has_aggregate = true;
        }
        break;
      case Aggregation::Minimum:
        if (!has_aggregate || comparison.value() < 0) {
          aggregate = posture->value;
          has_aggregate = true;
        }
        break;
      case Aggregation::Sum: {
        if (!has_aggregate) {
          aggregate = posture->value;
          has_aggregate = true;
        } else {
          auto sum = AddScalar(aggregate, posture->value);
          if (!sum) {
            aggregate_overflow = true;
            break;
          }
          aggregate = sum.value();
        }
        break;
      }
      default:
        break;
    }
  }
  outcome.matched = matched;

  switch (predicate.aggregation) {
    case Aggregation::Count: {
      auto comparison = CompareScalar(Scalar::Count(static_cast<std::int64_t>(matched)),
                                      predicate.threshold);
      if (!comparison) {
        outcome.resolvable = false;
        outcome.detail = comparison.status().message();
        return outcome;
      }
      outcome.resolvable = true;
      outcome.satisfied = CompareSatisfies(predicate.comparator, comparison.value());
      outcome.detail = "counted " + std::to_string(matched) + " matching records";
      return outcome;
    }
    case Aggregation::Any: {
      if (comparable == 0) {
        outcome.resolvable = false;
        outcome.detail = unit_mismatch ? "every record has a mismatched unit"
                                       : "no usable records";
        return outcome;
      }
      outcome.resolvable = true;
      outcome.satisfied = matched > 0;
      outcome.detail = "matched " + std::to_string(matched) + " of " +
                       std::to_string(comparable) + " records";
      return outcome;
    }
    case Aggregation::All: {
      if (comparable == 0 || unit_mismatch) {
        outcome.resolvable = false;
        outcome.detail = "every record must be comparable for an all predicate";
        return outcome;
      }
      outcome.resolvable = true;
      outcome.satisfied = matched == comparable;
      outcome.detail = "matched " + std::to_string(matched) + " of " +
                       std::to_string(comparable) + " records";
      return outcome;
    }
    case Aggregation::Maximum:
    case Aggregation::Minimum:
    case Aggregation::Sum: {
      if (!has_aggregate || aggregate_overflow || unit_mismatch) {
        outcome.resolvable = false;
        outcome.detail = aggregate_overflow ? "aggregate overflowed"
                                            : "aggregate is not comparable";
        return outcome;
      }
      auto comparison = CompareScalar(aggregate, predicate.threshold);
      if (!comparison) {
        outcome.resolvable = false;
        outcome.detail = comparison.status().message();
        return outcome;
      }
      outcome.resolvable = true;
      outcome.satisfied = CompareSatisfies(predicate.comparator, comparison.value());
      outcome.detail = "aggregate " + aggregate.ToString();
      return outcome;
    }
  }
  outcome.resolvable = false;
  outcome.detail = "unreachable aggregation";
  return outcome;
}

std::vector<const ModeDefinition*> PolicyDocument::OrderedModes() const {
  std::vector<const ModeDefinition*> ordered;
  ordered.reserve(modes.size());
  for (const ModeDefinition& mode : modes) {
    ordered.push_back(&mode);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const ModeDefinition* a, const ModeDefinition* b) {
              const int rank_a = SeverityRank(a->cls);
              const int rank_b = SeverityRank(b->cls);
              if (rank_a != rank_b) {
                return rank_a < rank_b;
              }
              return a->id < b->id;
            });
  return ordered;
}

Status ValidatePolicy(const PolicyDocument& policy) {
  if (!policy.id.IsValid()) {
    return Reject("policy id must be non-zero");
  }
  if (!policy.generation.IsValid()) {
    return Reject("policy generation must be non-zero");
  }
  if (policy.revision_key.empty()) {
    return Reject("policy revision key must not be empty");
  }
  if (policy.revision_key.size() > limits::kMaxStringBytes) {
    return Reject("policy revision key exceeds the byte limit");
  }
  if (policy.default_max_age_ms.value() == 0) {
    return Reject("default evidence maximum age must be non-zero");
  }
  if (policy.max_recovery_step == 0) {
    return Reject("max_recovery_step must be at least one class");
  }
  if (policy.service_classes.empty()) {
    return Reject("policy must declare at least one service class");
  }
  if (policy.service_classes.size() > limits::kMaxServiceClasses) {
    return Reject("policy declares too many service classes");
  }
  for (std::size_t i = 0; i < policy.service_classes.size(); ++i) {
    if (policy.service_classes[i].empty()) {
      return Reject("service class names must not be empty");
    }
    if (i > 0 && !(policy.service_classes[i - 1] < policy.service_classes[i])) {
      return Reject("service classes must be unique and canonically ordered");
    }
  }

  if (policy.obligations.size() > limits::kMaxObligations) {
    return Reject("policy declares too many obligation classes");
  }
  for (std::size_t i = 0; i < policy.obligations.size(); ++i) {
    const ObligationClass& obligation = policy.obligations[i];
    if (!obligation.id.IsValid()) {
      return Reject("obligation id must be non-zero");
    }
    if (obligation.key.empty()) {
      return Reject("obligation key must not be empty");
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (policy.obligations[j].id == obligation.id) {
        return Reject("obligation ids must be unique");
      }
      if (policy.obligations[j].key == obligation.key) {
        return Reject("obligation keys must be unique");
      }
    }
    for (const std::string& service : obligation.service_classes) {
      if (!HasServiceClass(policy, service)) {
        return Reject("obligation names an unknown service class");
      }
    }
  }

  if (policy.modes.empty()) {
    return Reject("policy must define at least one mode");
  }
  if (policy.modes.size() > limits::kMaxModes) {
    return Reject("policy defines too many modes");
  }

  std::vector<RestrictionId> restriction_ids;
  for (const ModeDefinition& mode : policy.modes) {
    for (const RestrictionRule& rule : mode.restrictions) {
      restriction_ids.push_back(rule.id);
    }
  }
  for (const RestrictionRule& rule : policy.indeterminate_restrictions) {
    restriction_ids.push_back(rule.id);
  }
  std::sort(restriction_ids.begin(), restriction_ids.end());
  for (std::size_t i = 1; i < restriction_ids.size(); ++i) {
    if (restriction_ids[i - 1] == restriction_ids[i]) {
      return Reject("restriction ids must be unique across the whole policy");
    }
  }

  bool has_nominal = false;
  bool has_emergency = false;
  for (const ModeDefinition& mode : policy.modes) {
    const std::string where =
        mode.key.empty() ? std::string("mode (unnamed)") : ("mode " + mode.key);
    if (!mode.id.IsValid()) {
      return Reject("mode id must be non-zero");
    }
    if (mode.key.empty()) {
      return Reject("mode key must not be empty");
    }
    if (!mode.revision.IsValid()) {
      return Reject(where + ": mode revision must be non-zero");
    }
    if (mode.entry.empty()) {
      return Reject(where + ": entry criteria must not be empty");
    }
    if (mode.exit.empty()) {
      return Reject(where + ": exit criteria must not be empty");
    }
    if (mode.entry.size() > limits::kMaxPredicatesPerMode ||
        mode.exit.size() > limits::kMaxPredicatesPerMode) {
      return Reject(where + ": too many criteria");
    }
    if (mode.restrictions.size() > limits::kMaxRestrictionsPerMode) {
      return Reject(where + ": too many restrictions");
    }
    if (mode.cls == OperatingClass::Nominal) {
      has_nominal = true;
    }
    if (mode.cls == OperatingClass::Emergency) {
      has_emergency = true;
    }
    for (std::size_t i = 0; i < policy.modes.size(); ++i) {
      const ModeDefinition& other = policy.modes[i];
      if (&other == &mode) {
        continue;
      }
      if (other.id == mode.id) {
        return Reject("mode ids must be unique");
      }
      if (other.key == mode.key) {
        return Reject("mode keys must be unique");
      }
    }
    for (const Predicate& predicate : mode.entry) {
      Status status = ValidatePredicateShape(predicate, where + " entry criteria");
      if (!status.ok()) {
        return status;
      }
    }
    for (const Predicate& predicate : mode.exit) {
      Status status = ValidatePredicateShape(predicate, where + " exit criteria");
      if (!status.ok()) {
        return status;
      }
    }

    for (const Predicate& exit : mode.exit) {
      for (const Predicate& entry : mode.entry) {
        if (!SameSelector(exit, entry)) {
          continue;
        }
        if (exit.threshold.unit() != entry.threshold.unit()) {
          return Reject(where +
                        ": entry and exit thresholds for one selector must share a unit");
        }
        const bool ordered = exit.comparator == Comparator::AtLeast ||
                             exit.comparator == Comparator::AtMost;
        const bool entry_ordered = entry.comparator == Comparator::AtLeast ||
                                   entry.comparator == Comparator::AtMost;
        if (ordered && entry_ordered) {
          // A mode may leave on the same condition it entered (time hysteresis
          // is enforced by the engine through minimum dwell and recovery hold),
          // but it may never leave more easily than it entered.
          const bool not_weaker =
              exit.comparator == Comparator::AtLeast
                  ? exit.threshold.value() >= entry.threshold.value()
                  : exit.threshold.value() <= entry.threshold.value();
          if (!not_weaker) {
            return Reject(where +
                          ": the exit threshold is weaker than the entry threshold for one "
                          "selector, so the mode could leave more easily than it entered");
          }
        } else if (exit.threshold.value() == entry.threshold.value()) {
          return Reject(where +
                        ": exit and entry criteria for one selector must be distinguishable");
        }
      }
    }

    for (const RestrictionRule& rule : mode.restrictions) {
      Status status = ValidateRestriction(policy, rule, where);
      if (!status.ok()) {
        return status;
      }
    }

    for (const ObligationClassId id : mode.protected_obligations) {
      if (FindObligation(policy, id) == nullptr) {
        return Reject(where + ": protected obligation is not declared by the policy");
      }
      for (std::size_t i = 1; i < mode.protected_obligations.size(); ++i) {
        if (!(mode.protected_obligations[i - 1] < mode.protected_obligations[i])) {
          return Reject(where + ": protected obligations must be unique");
        }
      }
    }
    // An inviolable obligation is facility wide: no mode, whether or not it
    // lists the obligation as protected, may deny a service it depends on.
    for (const ObligationClass& obligation : policy.obligations) {
      if (obligation.protection != ProtectionLevel::Inviolable) {
        continue;
      }
      for (const RestrictionRule& rule : mode.restrictions) {
        const bool targets_obligation =
            std::find(obligation.service_classes.begin(), obligation.service_classes.end(),
                      rule.service_class) != obligation.service_classes.end();
        if (targets_obligation && rule.kind == RestrictionKind::Deny) {
          return Reject(where + ": mode denies a service an inviolable obligation depends on");
        }
      }
    }

    for (const Predicate& predicate : mode.entry) {
      if (!predicate.require_fresh) {
        continue;
      }
      if (FindCoveringRequirement(policy, predicate, false) == nullptr) {
        return Reject(where +
                      ": a fresh-gated entry criterion has no declared escalation "
                      "freshness requirement");
      }
    }
    for (const Predicate& predicate : mode.exit) {
      if (!predicate.require_fresh) {
        continue;
      }
      if (FindCoveringRequirement(policy, predicate, true) == nullptr) {
        return Reject(where +
                      ": a fresh-gated exit criterion has no declared recovery "
                      "freshness requirement");
      }
    }
  }
  if (!has_nominal) {
    return Reject("policy must define a nominal operating mode");
  }
  if (!has_emergency) {
    return Reject("policy must define an emergency operating mode");
  }

  for (const RestrictionRule& rule : policy.indeterminate_restrictions) {
    Status status = ValidateRestriction(policy, rule, "indeterminate restrictions");
    if (!status.ok()) {
      return status;
    }
  }
  for (const ObligationClass& obligation : policy.obligations) {
    if (obligation.protection != ProtectionLevel::Inviolable) {
      continue;
    }
    for (const RestrictionRule& rule : policy.indeterminate_restrictions) {
      const bool targets_obligation =
          std::find(obligation.service_classes.begin(), obligation.service_classes.end(),
                    rule.service_class) != obligation.service_classes.end();
      if (targets_obligation && rule.kind == RestrictionKind::Deny) {
        return Reject(
            "indeterminate restrictions deny a service an inviolable obligation depends on");
      }
    }
  }

  if (policy.requirements.size() > limits::kMaxCollectionItems) {
    return Reject("policy declares too many freshness requirements");
  }
  for (std::size_t i = 0; i < policy.requirements.size(); ++i) {
    const EvidenceRequirement& requirement = policy.requirements[i];
    if (requirement.subject.empty() || requirement.metric.empty()) {
      return Reject("freshness requirement selectors must not be empty");
    }
    if (requirement.max_age_ms.value() == 0) {
      return Reject("freshness requirement maximum age must be non-zero");
    }
    for (std::size_t j = 0; j < i; ++j) {
      const EvidenceRequirement& other = policy.requirements[j];
      if (other.cls == requirement.cls && other.subject == requirement.subject &&
          other.metric == requirement.metric && other.scope == requirement.scope) {
        return Reject("freshness requirements must be unique");
      }
    }
  }
  return Status::Ok();
}

Result<const ModeDefinition*> FindMode(const PolicyDocument& policy, ModeId id) {
  for (const ModeDefinition& mode : policy.modes) {
    if (mode.id == id) {
      return Result<const ModeDefinition*>::Ok(&mode);
    }
  }
  return Result<const ModeDefinition*>::Err(ErrorCode::NotFound, "mode is not in the policy");
}

const ModeDefinition* FindFirstModeOfClass(const PolicyDocument& policy, OperatingClass cls) {
  const ModeDefinition* best = nullptr;
  for (const ModeDefinition& mode : policy.modes) {
    if (mode.cls != cls) {
      continue;
    }
    if (best == nullptr || mode.id < best->id) {
      best = &mode;
    }
  }
  return best;
}

const EvidenceRequirement* FindCoveringRequirement(const PolicyDocument& policy,
                                                   const Predicate& predicate,
                                                   bool for_recovery) {
  const EvidenceRequirement* best = nullptr;
  int best_score = -1;
  for (const EvidenceRequirement& requirement : policy.requirements) {
    if (requirement.cls != predicate.cls) {
      continue;
    }
    if (for_recovery ? !CoversRecovery(requirement.scope)
                     : !CoversEscalation(requirement.scope)) {
      continue;
    }
    if (requirement.subject != "*" && requirement.subject != predicate.subject) {
      continue;
    }
    if (requirement.metric != "*" && requirement.metric != predicate.metric) {
      continue;
    }
    int score = 0;
    if (requirement.subject != "*") {
      score += 2;
    }
    if (requirement.metric != "*") {
      score += 1;
    }
    if (score > best_score ||
        (score == best_score && best != nullptr &&
         requirement.max_age_ms < best->max_age_ms)) {
      best_score = score;
      best = &requirement;
    }
  }
  return best;
}

}  // namespace dom
