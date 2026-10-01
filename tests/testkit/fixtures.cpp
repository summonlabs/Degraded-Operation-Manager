// Degraded Operation Manager - shared synthetic facility fixtures.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "testkit/fixtures.hpp"

#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace dom::test {
namespace {

Predicate Criterion(EvidenceClass cls, const std::string& subject, const std::string& metric,
                    Aggregation aggregation, Comparator comparator, Scalar threshold,
                    bool require_fresh = true) {
  Predicate predicate;
  predicate.cls = cls;
  predicate.subject = subject;
  predicate.metric = metric;
  predicate.aggregation = aggregation;
  predicate.comparator = comparator;
  predicate.threshold = threshold;
  predicate.require_fresh = require_fresh;
  return predicate;
}

RestrictionRule Rule(std::uint64_t id, const std::string& service, RestrictionKind kind,
                     Scalar allowance) {
  RestrictionRule rule;
  rule.id = RestrictionId::FromValue(id);
  rule.service_class = service;
  rule.kind = kind;
  rule.allowance = allowance;
  return rule;
}

ModeDefinition Mode(std::uint64_t id, const std::string& key, OperatingClass cls, LatchMode latch,
                    std::uint64_t dwell_ms, std::uint64_t hold_ms) {
  ModeDefinition mode;
  mode.id = ModeId::FromValue(id);
  mode.key = key;
  mode.cls = cls;
  mode.revision = RevisionId::FromValue(1);
  mode.latch = latch;
  mode.min_dwell_ms = Duration::FromValue(dwell_ms);
  mode.recovery_hold_ms = Duration::FromValue(hold_ms);
  return mode;
}

EvidenceRecord MakeRecord(EvidenceClass cls, const std::string& subject, const std::string& metric,
                          Scalar value, const EvidenceOptions& options) {
  EvidenceRecord record;
  record.cls = cls;
  record.subject = subject;
  record.metric = metric;
  record.value = value;
  record.observed_at_ms = Instant::FromValue(options.observed_at_ms);
  record.valid_until_ms = Instant::FromValue(options.valid_until_ms);
  record.generation = Generation::FromValue(options.generation == 0 ? options.revision
                                                                    : options.generation);
  record.source_digest = Sha256::Of(metric + "#" + std::to_string(value.value()));
  record.producer = options.producer;
  return record;
}

}  // namespace

PolicyDocument MakePolicy(const PolicyOptions& options) {
  PolicyDocument policy;
  policy.id = PolicyId::FromValue(options.id);
  policy.generation = PolicyGeneration::FromValue(options.generation);
  policy.revision_key = options.revision_key;
  policy.max_recovery_step = options.max_recovery_step;
  policy.default_max_age_ms = Duration::FromValue(options.default_max_age_ms);
  policy.service_classes = {"batch", "inference", "life-safety", "storage"};

  ObligationClass life_safety;
  life_safety.id = ObligationClassId::FromValue(1);
  life_safety.key = "life-safety";
  life_safety.protection = ProtectionLevel::Inviolable;
  life_safety.service_classes = {"life-safety"};
  policy.obligations.push_back(life_safety);

  ObligationClass durability;
  durability.id = ObligationClassId::FromValue(2);
  durability.key = "data-durability";
  durability.protection = ProtectionLevel::Protected;
  durability.service_classes = {"storage"};
  policy.obligations.push_back(durability);

  const Scalar clear = Scalar::Code(0);
  const Scalar major = Scalar::Code(2);
  const Scalar critical = Scalar::Code(3);
  const Scalar full_redundancy = Scalar(Unit::MilliRatio, 1000);
  const Scalar degraded_redundancy = Scalar(Unit::MilliRatio, 500);

  ModeDefinition nominal = Mode(1, "facility.nominal", OperatingClass::Nominal, LatchMode::None, 0,
                                options.recovery_hold_ms);
  nominal.entry.push_back(Criterion(EvidenceClass::Incident, "*", "severity", Aggregation::Maximum,
                                    Comparator::AtMost, clear));
  nominal.entry.push_back(Criterion(EvidenceClass::Redundancy, "*", "ratio",
                                    Aggregation::Minimum, Comparator::AtLeast, full_redundancy));
  nominal.exit.push_back(Criterion(EvidenceClass::Incident, "*", "severity", Aggregation::Maximum,
                                   Comparator::AtMost, clear));
  nominal.exit.push_back(Criterion(EvidenceClass::Redundancy, "*", "ratio", Aggregation::Minimum,
                                   Comparator::AtLeast, full_redundancy));
  policy.modes.push_back(nominal);

  ModeDefinition watch = Mode(3, "facility.watch", OperatingClass::Watch, LatchMode::None,
                              options.min_dwell_ms, options.recovery_hold_ms);
  watch.entry.push_back(Criterion(EvidenceClass::Incident, "*", "severity", Aggregation::Maximum,
                                  Comparator::AtLeast, Scalar::Code(1)));
  watch.exit.push_back(Criterion(EvidenceClass::Incident, "*", "severity", Aggregation::Maximum,
                                 Comparator::AtMost, clear));
  watch.restrictions.push_back(
      Rule(9, "batch", RestrictionKind::Throttle, Scalar(Unit::MilliPercent, 90000)));
  policy.modes.push_back(watch);

  ModeDefinition conserve = Mode(5, "facility.conserve", OperatingClass::Conserve, LatchMode::None,
                                 options.min_dwell_ms, options.recovery_hold_ms);
  conserve.entry.push_back(Criterion(EvidenceClass::Incident, "*", "severity",
                                     Aggregation::Maximum, Comparator::AtLeast, major));
  conserve.exit.push_back(Criterion(EvidenceClass::Incident, "*", "severity", Aggregation::Maximum,
                                    Comparator::AtMost, clear));
  conserve.restrictions.push_back(
      Rule(10, "batch", RestrictionKind::Throttle, Scalar(Unit::MilliPercent, 50000)));
  policy.modes.push_back(conserve);

  ModeDefinition restricted = Mode(6, "facility.restricted", OperatingClass::Restricted,
                                   LatchMode::None, options.min_dwell_ms, options.recovery_hold_ms);
  restricted.entry.push_back(Criterion(EvidenceClass::Incident, "*", "severity",
                                       Aggregation::Maximum, Comparator::AtLeast, critical));
  restricted.entry.push_back(Criterion(EvidenceClass::Redundancy, "*", "ratio",
                                       Aggregation::Minimum, Comparator::AtMost,
                                       degraded_redundancy));
  restricted.exit.push_back(Criterion(EvidenceClass::Incident, "*", "severity",
                                      Aggregation::Maximum, Comparator::AtMost, major));
  restricted.exit.push_back(Criterion(EvidenceClass::Redundancy, "*", "ratio",
                                      Aggregation::Minimum, Comparator::AtLeast,
                                      full_redundancy));
  restricted.restrictions.push_back(Rule(11, "batch", RestrictionKind::Deny, Scalar()));
  restricted.restrictions.push_back(
      Rule(12, "inference", RestrictionKind::Throttle, Scalar(Unit::MilliPercent, 25000)));
  restricted.protected_obligations.push_back(ObligationClassId::FromValue(2));
  policy.modes.push_back(restricted);

  ModeDefinition emergency =
      Mode(9, "facility.emergency", OperatingClass::Emergency,
           options.latch_emergency ? LatchMode::UntilExplicitClear : LatchMode::None,
           options.min_dwell_ms, options.emergency_hold_ms);
  // Thermal margin is measured above the safe threshold: a margin at or below
  // five degrees kelvin-equivalent units is an emergency, and the mode is only
  // left once the margin has recovered well above it.
  emergency.entry.push_back(Criterion(EvidenceClass::Cooling, "zone-a", "margin",
                                      Aggregation::Maximum, Comparator::AtMost,
                                      Scalar(Unit::MilliCelsius, 5000)));
  emergency.exit.push_back(Criterion(EvidenceClass::Cooling, "zone-a", "margin",
                                     Aggregation::Maximum, Comparator::AtLeast,
                                     Scalar(Unit::MilliCelsius, 15000)));
  emergency.restrictions.push_back(Rule(13, "batch", RestrictionKind::Deny, Scalar()));
  emergency.restrictions.push_back(Rule(14, "inference", RestrictionKind::Deny, Scalar()));
  emergency.restrictions.push_back(
      Rule(15, "storage", RestrictionKind::Throttle, Scalar(Unit::MilliPercent, 10000)));
  emergency.protected_obligations.push_back(ObligationClassId::FromValue(1));
  emergency.protected_obligations.push_back(ObligationClassId::FromValue(2));
  policy.modes.push_back(emergency);

  policy.indeterminate_restrictions.push_back(
      Rule(20, "batch", RestrictionKind::Throttle, Scalar(Unit::MilliPercent, 25000)));

  EvidenceRequirement incident;
  incident.cls = EvidenceClass::Incident;
  incident.subject = "*";
  incident.metric = "severity";
  incident.scope = RequirementScope::Both;
  incident.max_age_ms = Duration::FromValue(options.requirement_max_age_ms);
  policy.requirements.push_back(incident);

  EvidenceRequirement redundancy;
  redundancy.cls = EvidenceClass::Redundancy;
  redundancy.subject = "*";
  redundancy.metric = "ratio";
  redundancy.scope = RequirementScope::Both;
  redundancy.max_age_ms = Duration::FromValue(options.requirement_max_age_ms);
  policy.requirements.push_back(redundancy);

  EvidenceRequirement cooling;
  cooling.cls = EvidenceClass::Cooling;
  cooling.subject = "zone-a";
  cooling.metric = "margin";
  cooling.scope = RequirementScope::Both;
  cooling.max_age_ms = Duration::FromValue(options.requirement_max_age_ms);
  policy.requirements.push_back(cooling);
  return policy;
}

EvidenceSnapshot MakeEvidence(const EvidenceOptions& options) {
  std::vector<EvidenceRecord> records;
  records.push_back(MakeRecord(EvidenceClass::Incident, "facility", "severity",
                               Scalar::Code(options.severity), options));
  records.push_back(MakeRecord(EvidenceClass::Redundancy, "power", "ratio",
                               Scalar(Unit::MilliRatio, options.redundancy), options));
  if (options.include_cooling) {
    records.push_back(MakeRecord(EvidenceClass::Cooling, "zone-a", "margin",
                                 Scalar(Unit::MilliCelsius, options.cooling_margin), options));
  }
  if (options.cooling_conflict) {
    EvidenceOptions other = options;
    other.producer = "facility-observer-b";
    other.cooling_margin = options.cooling_margin + 1000;
    records.push_back(MakeRecord(EvidenceClass::Cooling, "zone-a", "margin",
                                 Scalar(Unit::MilliCelsius, other.cooling_margin), other));
  }
  auto snapshot = EvidenceSnapshot::Build(std::move(records),
                                          EvidenceRevision::FromValue(options.revision));
  if (!snapshot) {
    // Fixtures are always valid; a failure here is a defect in the fixture.
    EvidenceSnapshot empty;
    return empty;
  }
  return snapshot.value();
}

CommittedView MakeCommittedView(const PolicyDocument& policy, std::uint64_t epoch,
                                std::uint64_t incarnation) {
  CommittedView committed;
  const ModeDefinition* nominal = FindFirstModeOfClass(policy, OperatingClass::Nominal);
  committed.mode = nominal != nullptr ? nominal->id : ModeId::FromValue(1);
  committed.posture = Posture::Nominal;
  committed.epoch = ControlEpoch::FromValue(epoch);
  committed.incarnation = Incarnation::FromValue(incarnation);
  committed.state_sequence = StateSequence::FromValue(1);
  committed.since_ms = Instant();
  committed.since_tick = Tick();
  return committed;
}

CoordinatorOptions MakeCoordinatorOptions(const std::string& directory,
                                          const PolicyDocument& policy,
                                          std::uint64_t incarnation) {
  CoordinatorOptions options;
  options.directory = directory;
  options.policy = policy;
  options.incarnation = Incarnation::FromValue(incarnation);
  options.max_authorization_ttl_ms = Duration::FromValue(600000);
  return options;
}

Status WriteTextFile(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return Status::Error(ErrorCode::IoError, "cannot create " + path);
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!stream) {
    return Status::Error(ErrorCode::IoError, "short write to " + path);
  }
  stream.flush();
  return stream ? Status::Ok()
                : Status::Error(ErrorCode::IoError, "cannot flush " + path);
}

}  // namespace dom::test
