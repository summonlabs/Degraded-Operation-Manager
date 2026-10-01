// Degraded Operation Manager - independent downstream consumer.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This program is deliberately written as an outside consumer would write it:
// it includes only the installed public headers, links only the exported
// targets, builds a small facility policy, opens a durable authority in a store
// directory supplied on the command line, evaluates two synthetic postures and
// prints a machine readable summary. It exits non-zero if the authority did not
// behave as documented.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "dom/dom.hpp"

namespace {

dom::Predicate AtMost(dom::EvidenceClass cls, const std::string& metric,
                      std::int64_t threshold) {
  dom::Predicate predicate;
  predicate.cls = cls;
  predicate.subject = "*";
  predicate.metric = metric;
  predicate.aggregation = dom::Aggregation::Maximum;
  predicate.comparator = dom::Comparator::AtMost;
  predicate.threshold = dom::Scalar::Code(threshold);
  predicate.require_fresh = true;
  return predicate;
}

dom::Predicate AtLeast(dom::EvidenceClass cls, const std::string& metric,
                       std::int64_t threshold) {
  dom::Predicate predicate = AtMost(cls, metric, threshold);
  predicate.comparator = dom::Comparator::AtLeast;
  return predicate;
}

dom::PolicyDocument BuildPolicy() {
  dom::PolicyDocument policy;
  policy.id = dom::PolicyId::FromValue(7);
  policy.generation = dom::PolicyGeneration::FromValue(1);
  policy.revision_key = "consumer-example";
  policy.max_recovery_step = 6;
  policy.default_max_age_ms = dom::Duration::FromValue(60000);
  policy.service_classes = {"batch", "life-safety"};

  dom::ObligationClass safety;
  safety.id = dom::ObligationClassId::FromValue(1);
  safety.key = "life-safety";
  safety.protection = dom::ProtectionLevel::Inviolable;
  safety.service_classes = {"life-safety"};
  policy.obligations.push_back(safety);

  dom::ModeDefinition nominal;
  nominal.id = dom::ModeId::FromValue(1);
  nominal.key = "facility.nominal";
  nominal.cls = dom::OperatingClass::Nominal;
  nominal.revision = dom::RevisionId::FromValue(1);
  nominal.recovery_hold_ms = dom::Duration::FromValue(1000);
  nominal.entry.push_back(AtMost(dom::EvidenceClass::Incident, "severity", 0));
  nominal.exit.push_back(AtMost(dom::EvidenceClass::Incident, "severity", 0));
  policy.modes.push_back(nominal);

  dom::ModeDefinition restricted;
  restricted.id = dom::ModeId::FromValue(2);
  restricted.key = "facility.restricted";
  restricted.cls = dom::OperatingClass::Restricted;
  restricted.revision = dom::RevisionId::FromValue(1);
  restricted.min_dwell_ms = dom::Duration::FromValue(1000);
  restricted.recovery_hold_ms = dom::Duration::FromValue(1000);
  restricted.entry.push_back(AtLeast(dom::EvidenceClass::Incident, "severity", 2));
  restricted.exit.push_back(AtMost(dom::EvidenceClass::Incident, "severity", 0));
  dom::RestrictionRule rule;
  rule.id = dom::RestrictionId::FromValue(5);
  rule.service_class = "batch";
  rule.kind = dom::RestrictionKind::Deny;
  restricted.restrictions.push_back(rule);
  policy.modes.push_back(restricted);

  dom::ModeDefinition emergency;
  emergency.id = dom::ModeId::FromValue(3);
  emergency.key = "facility.emergency";
  emergency.cls = dom::OperatingClass::Emergency;
  emergency.revision = dom::RevisionId::FromValue(1);
  emergency.min_dwell_ms = dom::Duration::FromValue(1000);
  emergency.recovery_hold_ms = dom::Duration::FromValue(1000);
  emergency.entry.push_back(AtLeast(dom::EvidenceClass::Incident, "severity", 3));
  emergency.exit.push_back(AtMost(dom::EvidenceClass::Incident, "severity", 0));
  dom::RestrictionRule emergency_rule;
  emergency_rule.id = dom::RestrictionId::FromValue(6);
  emergency_rule.service_class = "batch";
  emergency_rule.kind = dom::RestrictionKind::Deny;
  emergency.restrictions.push_back(emergency_rule);
  emergency.protected_obligations.push_back(dom::ObligationClassId::FromValue(1));
  policy.modes.push_back(emergency);

  dom::EvidenceRequirement requirement;
  requirement.cls = dom::EvidenceClass::Incident;
  requirement.subject = "*";
  requirement.metric = "severity";
  requirement.scope = dom::RequirementScope::Both;
  requirement.max_age_ms = dom::Duration::FromValue(60000);
  policy.requirements.push_back(requirement);
  return policy;
}

dom::EvidenceSnapshot Evidence(std::uint64_t revision, std::uint64_t observed_at,
                               std::int64_t severity) {
  std::vector<dom::EvidenceRecord> records;
  dom::EvidenceRecord record;
  record.cls = dom::EvidenceClass::Incident;
  record.subject = "facility";
  record.metric = "severity";
  record.value = dom::Scalar::Code(severity);
  record.observed_at_ms = dom::Instant::FromValue(observed_at);
  record.generation = dom::Generation::FromValue(revision);
  record.source_digest = dom::Sha256::Of("severity-" + std::to_string(severity));
  record.producer = "consumer-observer";
  records.push_back(std::move(record));
  auto snapshot = dom::EvidenceSnapshot::Build(std::move(records),
                                               dom::EvidenceRevision::FromValue(revision));
  if (!snapshot) {
    return dom::EvidenceSnapshot();
  }
  return snapshot.value();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string store = argc > 1 ? argv[1] : "dom-consumer-store";
  const dom::PolicyDocument policy = BuildPolicy();
  const dom::Status valid = dom::ValidatePolicy(policy);
  if (!valid.ok()) {
    std::cerr << "policy invalid: " << valid.ToString() << "\n";
    return 1;
  }

  dom::CoordinatorOptions options;
  options.directory = store;
  options.policy = policy;
  options.incarnation = dom::Incarnation::FromValue(20260101);
  options.max_authorization_ttl_ms = dom::Duration::FromValue(600000);
  auto coordinator = dom::Coordinator::Open(options);
  if (!coordinator) {
    std::cerr << "cannot open the authority: " << coordinator.status().ToString() << "\n";
    return 1;
  }

  std::cout << "product=" << dom::kProductName << " version=" << dom::VersionString() << "\n";
  std::cout << "policy-digest=" << dom::PolicyDigest(policy).ToHex() << "\n";

  const std::int64_t severities[] = {3, 0};
  int exit_code = 0;
  for (int step = 0; step < 2; ++step) {
    auto status = coordinator.value()->GetStatus();
    if (!status) {
      std::cerr << status.status().ToString() << "\n";
      return 1;
    }
    const std::uint64_t now = 1000 + static_cast<std::uint64_t>(step) * 5000;
    dom::EvaluationRequest request;
    request.envelope.idempotency_key = dom::Sha256::Of("consumer-" + std::to_string(step));
    request.envelope.expected_state_sequence = status.value().state_sequence;
    request.envelope.expected_epoch = status.value().epoch;
    request.context.epoch = status.value().epoch;
    request.context.incarnation = status.value().incarnation;
    request.context.now_ms = dom::Instant::FromValue(now);
    request.context.tick = dom::Tick::FromValue(static_cast<std::uint64_t>(step + 1));
    request.evidence = Evidence(static_cast<std::uint64_t>(step + 1), now, severities[step]);
    auto outcome = coordinator.value()->Evaluate(request);
    if (!outcome) {
      std::cerr << "evaluation refused: " << outcome.status().ToString() << "\n";
      return 1;
    }
    std::cout << "step=" << step << " state-sequence=" << outcome.value().state_sequence.ToString()
              << " posture=" << dom::PostureName(outcome.value().decision.posture)
              << " verdict=" << dom::VerdictName(outcome.value().decision.verdict)
              << " restricted-services=";
    for (std::size_t i = 0; i < outcome.value().restrictions.restricted_services.size(); ++i) {
      std::cout << (i == 0 ? "" : ",") << outcome.value().restrictions.restricted_services[i];
    }
    std::cout << "\n";
    if (step == 0 && outcome.value().decision.posture != dom::Posture::Emergency) {
      std::cerr << "expected an emergency posture for a critical incident\n";
      exit_code = 2;
    }
  }

  auto final_status = coordinator.value()->GetStatus();
  if (!final_status) {
    std::cerr << final_status.status().ToString() << "\n";
    return 1;
  }
  std::cout << "final state-sequence=" << final_status.value().state_sequence.ToString()
            << " decision-sequence=" << final_status.value().decision_sequence.ToString()
            << " recovery=" << dom::RecoveryOutcomeName(coordinator.value()->recovery().outcome)
            << "\n";
  return exit_code;
}
