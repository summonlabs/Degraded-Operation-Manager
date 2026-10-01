// Degraded Operation Manager - end to end walkthrough.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This example is self contained: it builds a synthetic facility policy and
// synthetic evidence, opens a real durable store, and walks through escalation,
// an indeterminate posture caused by stale evidence, staged recovery governed
// by dwell and recovery hold, and an explicit latch clear. Nothing here touches
// real facility hardware: the facility, its incidents and its measurements are
// modelled inputs (SYNTHETIC). The store, the locking and the publication are
// real (REAL).

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "dom/dom.hpp"

namespace {

dom::Predicate AtMost(dom::EvidenceClass cls, const std::string& subject,
                      const std::string& metric, dom::Scalar threshold, bool require_fresh) {
  dom::Predicate predicate;
  predicate.cls = cls;
  predicate.subject = subject;
  predicate.metric = metric;
  predicate.aggregation = dom::Aggregation::Maximum;
  predicate.comparator = dom::Comparator::AtMost;
  predicate.threshold = threshold;
  predicate.require_fresh = require_fresh;
  return predicate;
}

dom::Predicate AtLeast(dom::EvidenceClass cls, const std::string& subject,
                       const std::string& metric, dom::Scalar threshold, bool require_fresh) {
  dom::Predicate predicate = AtMost(cls, subject, metric, threshold, require_fresh);
  predicate.comparator = dom::Comparator::AtLeast;
  return predicate;
}

dom::Predicate EveryRecordAtMost(dom::EvidenceClass cls, const std::string& subject,
                                 const std::string& metric, dom::Scalar threshold) {
  dom::Predicate predicate = AtMost(cls, subject, metric, threshold, true);
  predicate.aggregation = dom::Aggregation::All;
  return predicate;
}

dom::RestrictionRule Restrict(std::uint64_t id, const std::string& service,
                              dom::RestrictionKind kind, dom::Scalar allowance) {
  dom::RestrictionRule rule;
  rule.id = dom::RestrictionId::FromValue(id);
  rule.service_class = service;
  rule.kind = kind;
  rule.allowance = allowance;
  return rule;
}

dom::ModeDefinition MakeMode(std::uint64_t id, const std::string& key, dom::OperatingClass cls,
                             dom::LatchMode latch, std::uint64_t dwell_ms,
                             std::uint64_t hold_ms) {
  dom::ModeDefinition mode;
  mode.id = dom::ModeId::FromValue(id);
  mode.key = key;
  mode.cls = cls;
  mode.revision = dom::RevisionId::FromValue(1);
  mode.latch = latch;
  mode.min_dwell_ms = dom::Duration::FromValue(dwell_ms);
  mode.recovery_hold_ms = dom::Duration::FromValue(hold_ms);
  return mode;
}

dom::PolicyDocument BuildPolicy() {
  dom::PolicyDocument policy;
  policy.id = dom::PolicyId::FromValue(51);
  policy.generation = dom::PolicyGeneration::FromValue(7);
  policy.revision_key = "facility-degraded-modes-2026-02";
  policy.max_recovery_step = 6;
  policy.default_max_age_ms = dom::Duration::FromValue(30000);
  policy.service_classes = {"batch", "inference", "storage", "life-safety"};

  dom::ObligationClass life_safety;
  life_safety.id = dom::ObligationClassId::FromValue(1);
  life_safety.key = "life-safety";
  life_safety.protection = dom::ProtectionLevel::Inviolable;
  life_safety.service_classes = {"life-safety"};
  policy.obligations.push_back(life_safety);

  dom::ObligationClass durability;
  durability.id = dom::ObligationClassId::FromValue(2);
  durability.key = "data-durability";
  durability.protection = dom::ProtectionLevel::Protected;
  durability.service_classes = {"storage"};
  policy.obligations.push_back(durability);

  // Severity codes: 0 clear, 1 advisory, 2 major, 3 critical.
  const dom::Scalar severity_clear = dom::Scalar::Code(0);
  const dom::Scalar severity_major = dom::Scalar::Code(2);
  const dom::Scalar severity_critical = dom::Scalar::Code(3);
  const dom::Scalar ratio_full = dom::Scalar(dom::Unit::MilliRatio, 1000);
  const dom::Scalar ratio_half = dom::Scalar(dom::Unit::MilliRatio, 500);

  dom::ModeDefinition nominal =
      MakeMode(1, "facility.nominal", dom::OperatingClass::Nominal, dom::LatchMode::None, 0, 60000);
  nominal.entry.push_back(AtMost(dom::EvidenceClass::Incident, "*", "severity", severity_clear, true));
  nominal.entry.push_back(
      AtLeast(dom::EvidenceClass::Redundancy, "*", "ratio", ratio_full, true));
  nominal.exit.push_back(AtMost(dom::EvidenceClass::Incident, "*", "severity", severity_clear, true));
  nominal.exit.push_back(
      AtLeast(dom::EvidenceClass::Redundancy, "*", "ratio", ratio_full, true));
  policy.modes.push_back(nominal);

  dom::ModeDefinition conserve = MakeMode(5, "facility.conserve", dom::OperatingClass::Conserve,
                                          dom::LatchMode::None, 1000, 30000);
  conserve.entry.push_back(
      AtLeast(dom::EvidenceClass::Incident, "*", "severity", severity_major, true));
  conserve.exit.push_back(AtMost(dom::EvidenceClass::Incident, "*", "severity", severity_clear, true));
  conserve.restrictions.push_back(
      Restrict(10, "batch", dom::RestrictionKind::Throttle, dom::Scalar(dom::Unit::MilliPercent, 50000)));
  policy.modes.push_back(conserve);

  dom::ModeDefinition restricted = MakeMode(6, "facility.restricted", dom::OperatingClass::Restricted,
                                            dom::LatchMode::None, 1000, 30000);
  restricted.entry.push_back(
      AtLeast(dom::EvidenceClass::Incident, "*", "severity", severity_critical, true));
  restricted.exit.push_back(
      AtMost(dom::EvidenceClass::Incident, "*", "severity", severity_major, true));
  restricted.restrictions.push_back(Restrict(11, "batch", dom::RestrictionKind::Deny, dom::Scalar()));
  restricted.restrictions.push_back(Restrict(
      12, "inference", dom::RestrictionKind::Throttle, dom::Scalar(dom::Unit::MilliPercent, 25000)));
  restricted.protected_obligations.push_back(dom::ObligationClassId::FromValue(2));
  policy.modes.push_back(restricted);

  dom::ModeDefinition emergency = MakeMode(9, "facility.emergency", dom::OperatingClass::Emergency,
                                           dom::LatchMode::UntilExplicitClear, 1000, 60000);
  // Thermal margin is the distance above the safe threshold: a margin at or
  // below 5000 is an emergency, and the mode is left only after recovery well
  // above it.
  emergency.entry.push_back(AtMost(dom::EvidenceClass::Cooling, "zone-a", "margin",
                                   dom::Scalar(dom::Unit::MilliCelsius, 5000), true));
  emergency.exit.push_back(AtLeast(dom::EvidenceClass::Cooling, "zone-a", "margin",
                                   dom::Scalar(dom::Unit::MilliCelsius, 15000), true));
  emergency.restrictions.push_back(Restrict(13, "batch", dom::RestrictionKind::Deny, dom::Scalar()));
  emergency.restrictions.push_back(Restrict(14, "inference", dom::RestrictionKind::Deny, dom::Scalar()));
  emergency.restrictions.push_back(Restrict(
      15, "storage", dom::RestrictionKind::Throttle, dom::Scalar(dom::Unit::MilliPercent, 10000)));
  emergency.protected_obligations.push_back(dom::ObligationClassId::FromValue(1));
  emergency.protected_obligations.push_back(dom::ObligationClassId::FromValue(2));
  policy.modes.push_back(emergency);

  policy.indeterminate_restrictions.push_back(Restrict(
      20, "batch", dom::RestrictionKind::Throttle, dom::Scalar(dom::Unit::MilliPercent, 25000)));

  auto requirement = [](dom::EvidenceClass cls, const std::string& subject,
                        const std::string& metric, dom::RequirementScope scope,
                        std::uint64_t max_age_ms) {
    dom::EvidenceRequirement item;
    item.cls = cls;
    item.subject = subject;
    item.metric = metric;
    item.scope = scope;
    item.max_age_ms = dom::Duration::FromValue(max_age_ms);
    return item;
  };
  policy.requirements.push_back(requirement(dom::EvidenceClass::Incident, "*", "severity",
                                            dom::RequirementScope::Both, 60000));
  policy.requirements.push_back(requirement(dom::EvidenceClass::Redundancy, "*", "ratio",
                                            dom::RequirementScope::Both, 60000));
  policy.requirements.push_back(requirement(dom::EvidenceClass::Cooling, "zone-a", "margin",
                                            dom::RequirementScope::Both, 60000));
  return policy;
}

dom::EvidenceRecord Record(dom::EvidenceClass cls, const std::string& subject,
                           const std::string& metric, dom::Scalar value, std::uint64_t observed_at,
                           std::uint64_t generation) {
  dom::EvidenceRecord record;
  record.cls = cls;
  record.subject = subject;
  record.metric = metric;
  record.value = value;
  record.observed_at_ms = dom::Instant::FromValue(observed_at);
  record.valid_until_ms = dom::Instant();
  record.generation = dom::Generation::FromValue(generation);
  record.source_digest = dom::Sha256::Of(metric + ":" + std::to_string(value.value()));
  record.producer = "facility-observer";
  return record;
}

dom::Result<dom::EvidenceSnapshot> Evidence(std::uint64_t revision, std::uint64_t observed_at,
                                            std::int64_t severity, std::int64_t redundancy,
                                            std::int64_t cooling_margin) {
  std::vector<dom::EvidenceRecord> records;
  records.push_back(Record(dom::EvidenceClass::Incident, "facility", "severity",
                           dom::Scalar::Code(severity), observed_at, revision));
  records.push_back(Record(dom::EvidenceClass::Redundancy, "power", "ratio",
                           dom::Scalar(dom::Unit::MilliRatio, redundancy), observed_at, revision));
  records.push_back(Record(dom::EvidenceClass::Cooling, "zone-a", "margin",
                           dom::Scalar(dom::Unit::MilliCelsius, cooling_margin), observed_at,
                           revision));
  return dom::EvidenceSnapshot::Build(std::move(records),
                                      dom::EvidenceRevision::FromValue(revision));
}

void Report(const char* step, const dom::DecisionOutcome& outcome) {
  std::cout << "step " << step << " state-sequence=" << outcome.state_sequence.ToString()
            << " changed=" << (outcome.state_changed ? "true" : "false")
            << " posture=" << dom::PostureName(outcome.decision.posture)
            << " verdict=" << dom::VerdictName(outcome.decision.verdict)
            << " mode=" << outcome.decision.mode.ToString() << "\n";
  std::cout << "  restrictions:";
  for (const std::string& service : outcome.restrictions.restricted_services) {
    std::cout << " " << service;
  }
  std::cout << "\n";
  for (const dom::Reason& reason : outcome.decision.trace.reasons()) {
    std::cout << "  " << dom::ReasonTrace::RenderReason(reason) << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string directory = "dom-example-store";
  std::string authority = "walkthrough";
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--store" && i + 1 < argc) {
      directory = argv[++i];
    } else if (argument == "--authority" && i + 1 < argc) {
      authority = argv[++i];
    }
  }

  const dom::PolicyDocument policy = BuildPolicy();
  const dom::Status valid = dom::ValidatePolicy(policy);
  if (!valid.ok()) {
    std::cerr << "policy is not valid: " << valid.ToString() << "\n";
    return 1;
  }
  std::cout << "policy digest=" << dom::PolicyDigest(policy).ToHex() << "\n";

  dom::CoordinatorOptions options;
  options.directory = directory;
  options.policy = policy;
  options.incarnation = dom::Incarnation::FromValue(101);
  options.max_authorization_ttl_ms = dom::Duration::FromValue(600000);
  auto coordinator = dom::Coordinator::Open(options);
  if (!coordinator) {
    std::cerr << "cannot open the authority: " << coordinator.status().ToString() << "\n";
    return 1;
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    std::cerr << status.status().ToString() << "\n";
    return 1;
  }
  std::cout << "opened epoch=" << status.value().epoch.ToString()
            << " incarnation=" << status.value().incarnation.ToString()
            << " recovery=" << dom::RecoveryOutcomeName(coordinator.value()->recovery().outcome)
            << "\n";

  struct Step {
    const char* name;
    std::uint64_t now_ms;
    std::int64_t severity;
    std::int64_t redundancy;
    std::int64_t cooling_margin;
  };
  const std::uint64_t base = 1'700'000'000'000ull;
  const Step steps[] = {
      {"nominal", base, 0, 1000, 25000},
      {"major-incident", base + 1000, 2, 1000, 25000},
      {"critical-incident", base + 2000, 3, 750, 25000},
      {"cooling-emergency", base + 3000, 3, 750, 5000},
      {"stale-evidence", base + 300000, 0, 1000, 25000},
      {"fresh-recovery-start", base + 301000, 0, 1000, 25000},
      {"recovery-hold", base + 331000, 0, 1000, 25000},
      {"recovery-second-step", base + 362000, 0, 1000, 25000},
  };

  dom::CoordinatorStatus current = status.value();
  std::uint64_t tick = 1;
  for (const Step& step : steps) {
    auto evidence = Evidence(static_cast<std::uint64_t>(tick), step.now_ms, step.severity,
                             step.redundancy, step.cooling_margin);
    if (!evidence) {
      std::cerr << "evidence rejected: " << evidence.status().ToString() << "\n";
      return 1;
    }
    dom::EvaluationRequest request;
    request.envelope.idempotency_key =
        dom::Sha256::Of(std::string("walkthrough-") + step.name);
    request.envelope.expected_state_sequence = current.state_sequence;
    request.envelope.expected_epoch = current.epoch;
    request.context.epoch = current.epoch;
    request.context.incarnation = current.incarnation;
    request.context.now_ms = dom::Instant::FromValue(step.now_ms);
    request.context.tick = dom::Tick::FromValue(tick);
    request.evidence = evidence.value();
    auto outcome = coordinator.value()->Evaluate(request);
    if (!outcome) {
      std::cerr << "evaluation refused: " << outcome.status().ToString() << "\n";
      return 1;
    }
    Report(step.name, outcome.value());
    auto refreshed = coordinator.value()->GetStatus();
    if (!refreshed) {
      std::cerr << refreshed.status().ToString() << "\n";
      return 1;
    }
    current = refreshed.value();
    ++tick;
  }

  // Explicit latch clear: the emergency mode is latched until an authority
  // presents the decision that latched it together with fresh evidence.
  if (current.latched) {
    auto evidence = Evidence(static_cast<std::uint64_t>(tick), base + 400000, 0, 1000, 25000);
    dom::LatchClearRequest clear;
    clear.envelope.idempotency_key = dom::Sha256::Of(std::string_view("walkthrough-clear"));
    clear.envelope.expected_state_sequence = current.state_sequence;
    clear.envelope.expected_epoch = current.epoch;
    clear.authority_reference = authority;
    const dom::PersistedState& state = coordinator.value()->state();
    clear.latch_cause_digest = state.committed.latch.cause_digest;
    clear.context.epoch = current.epoch;
    clear.context.incarnation = current.incarnation;
    clear.context.now_ms = dom::Instant::FromValue(base + 400000);
    clear.context.tick = dom::Tick::FromValue(tick);
    clear.evidence = evidence.value();
    auto cleared = coordinator.value()->ClearLatch(clear);
    if (!cleared) {
      std::cerr << "latch clear refused: " << cleared.status().ToString() << "\n";
      return 1;
    }
    std::cout << "step latch-clear cleared=" << (cleared.value().cleared ? "true" : "false")
              << " state-sequence=" << cleared.value().state_sequence.ToString() << "\n";
    auto refreshed = coordinator.value()->GetStatus();
    if (refreshed) {
      current = refreshed.value();
    }
  }

  std::cout << "final state-sequence=" << current.state_sequence.ToString()
            << " posture=" << dom::PostureName(current.posture)
            << " mode=" << current.mode.ToString()
            << " latched=" << (current.latched ? "true" : "false") << "\n";
  return 0;
}
