// Degraded Operation Manager - scale and benchmark suite.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Two separately measured workloads, both counting completed useful operations:
//   * evaluation throughput of the pure engine (no I/O);
//   * durable publication throughput of the authority, where every generation is
//     staged, flushed, read back, verified, atomically renamed and journalled on
//     the real local file system.
// Submission latency is never measured, and no before/after pair is invented.
//
// Provenance: REAL local file system durability and real single process
// execution; SYNTHETIC facility, mode catalog and measurements.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

namespace {

using namespace dom;

PolicyDocument MakeLargePolicy(std::size_t mode_count) {
  PolicyDocument policy;
  policy.id = PolicyId::FromValue(9001);
  policy.generation = PolicyGeneration::FromValue(3);
  policy.revision_key = "synthetic-large-facility";
  policy.max_recovery_step = 6;
  policy.default_max_age_ms = Duration::FromValue(600000);
  policy.service_classes = {"batch", "inference", "life-safety", "storage"};

  ObligationClass life_safety;
  life_safety.id = ObligationClassId::FromValue(1);
  life_safety.key = "life-safety";
  life_safety.protection = ProtectionLevel::Inviolable;
  life_safety.service_classes = {"life-safety"};
  policy.obligations.push_back(life_safety);

  ModeDefinition nominal;
  nominal.id = ModeId::FromValue(1);
  nominal.key = "facility.nominal";
  nominal.cls = OperatingClass::Nominal;
  nominal.revision = RevisionId::FromValue(1);
  nominal.recovery_hold_ms = Duration::FromValue(30000);
  Predicate quiet;
  quiet.cls = EvidenceClass::Capacity;
  quiet.subject = "*";
  quiet.metric = "*";
  quiet.aggregation = Aggregation::Maximum;
  quiet.comparator = Comparator::AtMost;
  quiet.threshold = Scalar::Count(0);
  quiet.require_fresh = true;
  nominal.entry.push_back(quiet);
  nominal.exit.push_back(quiet);
  policy.modes.push_back(nominal);

  ModeDefinition emergency;
  emergency.id = ModeId::FromValue(2);
  emergency.key = "facility.emergency";
  emergency.cls = OperatingClass::Emergency;
  emergency.revision = RevisionId::FromValue(1);
  emergency.min_dwell_ms = Duration::FromValue(1000);
  emergency.recovery_hold_ms = Duration::FromValue(30000);
  Predicate hot;
  hot.cls = EvidenceClass::Cooling;
  hot.subject = "*";
  hot.metric = "*";
  hot.aggregation = Aggregation::Maximum;
  hot.comparator = Comparator::AtLeast;
  hot.threshold = Scalar::Count(1);
  hot.require_fresh = true;
  emergency.entry.push_back(hot);
  emergency.exit.push_back(quiet);
  policy.modes.push_back(emergency);

  for (std::size_t i = 0; i + 2 < mode_count; ++i) {
    ModeDefinition mode;
    mode.id = ModeId::FromValue(static_cast<std::uint64_t>(100 + i));
    mode.key = "facility.level-" + std::to_string(i);
    mode.cls = static_cast<OperatingClass>(1 + (i % 4));
    mode.revision = RevisionId::FromValue(1);
    mode.min_dwell_ms = Duration::FromValue(1000);
    mode.recovery_hold_ms = Duration::FromValue(30000);
    Predicate trigger;
    trigger.cls = EvidenceClass::Capacity;
    trigger.subject = "*";
    trigger.metric = "*";
    trigger.aggregation = Aggregation::Maximum;
    trigger.comparator = Comparator::AtLeast;
    trigger.threshold = Scalar::Count(1);
    trigger.require_fresh = true;
    mode.entry.push_back(trigger);
    mode.exit.push_back(quiet);
    policy.modes.push_back(mode);
  }

  EvidenceRequirement capacity;
  capacity.cls = EvidenceClass::Capacity;
  capacity.subject = "*";
  capacity.metric = "*";
  capacity.scope = RequirementScope::Both;
  capacity.max_age_ms = Duration::FromValue(600000);
  policy.requirements.push_back(capacity);

  EvidenceRequirement cooling;
  cooling.cls = EvidenceClass::Cooling;
  cooling.subject = "*";
  cooling.metric = "*";
  cooling.scope = RequirementScope::Both;
  cooling.max_age_ms = Duration::FromValue(600000);
  policy.requirements.push_back(cooling);
  return policy;
}

EvidenceSnapshot MakeLargeEvidence(std::size_t metric_count, std::int64_t value,
                                   std::uint64_t revision, std::uint64_t observed_at) {
  std::vector<EvidenceRecord> records;
  records.reserve(metric_count + 1);
  for (std::size_t i = 0; i < metric_count; ++i) {
    EvidenceRecord record;
    record.cls = EvidenceClass::Capacity;
    record.subject = "zone-" + std::to_string(i % 32);
    record.metric = "metric-" + std::to_string(i);
    record.value = Scalar::Count(value);
    record.observed_at_ms = Instant::FromValue(observed_at);
    record.generation = Generation::FromValue(revision);
    record.source_digest = Sha256::Of(record.metric);
    record.producer = "synthetic-observer";
    records.push_back(std::move(record));
  }
  EvidenceRecord cooling;
  cooling.cls = EvidenceClass::Cooling;
  cooling.subject = "zone-0";
  cooling.metric = "margin";
  cooling.value = Scalar::Count(value);
  cooling.observed_at_ms = Instant::FromValue(observed_at);
  cooling.generation = Generation::FromValue(revision);
  cooling.source_digest = Sha256::Of(std::string_view("cooling"));
  cooling.producer = "synthetic-observer";
  records.push_back(std::move(cooling));
  auto snapshot = EvidenceSnapshot::Build(std::move(records),
                                          EvidenceRevision::FromValue(revision));
  if (!snapshot) {
    return EvidenceSnapshot();
  }
  return snapshot.value();
}

DOM_TEST(engine_evaluation_throughput) {
  constexpr std::size_t kModes = 32;
  constexpr std::size_t kMetrics = 512;
  // The unoptimised configurations are slower by construction; the reported
  // rate is per configuration and the scale is printed with it.
#if defined(NDEBUG)
  constexpr int kEvaluations = 2000;
#else
  constexpr int kEvaluations = 200;
#endif
  const PolicyDocument policy = MakeLargePolicy(kModes);
  DOM_CHECK_OK(ValidatePolicy(policy));

  CommittedView committed = dom::test::MakeCommittedView(policy, 1, 1);
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t decided = 0;
  for (int i = 0; i < kEvaluations; ++i) {
    const std::uint64_t now = 10000 + static_cast<std::uint64_t>(i) * 100;
    auto evidence = MakeLargeEvidence(kMetrics, i % 2 == 0 ? 1 : 0,
                                      static_cast<std::uint64_t>(i + 1), now);
    EvaluationInputs inputs;
    inputs.policy = &policy;
    inputs.evidence = &evidence;
    inputs.committed = committed;
    inputs.context.epoch = committed.epoch;
    inputs.context.incarnation = committed.incarnation;
    inputs.context.now_ms = Instant::FromValue(now);
    inputs.context.tick = Tick::FromValue(static_cast<std::uint64_t>(i + 1));
    inputs.policy_validated = true;
    auto outcome = EvaluateMode(inputs);
    DOM_CHECK_OK(outcome);
    if (!outcome.value().decision.input_digest.IsZero()) {
      ++decided;
    }
    committed = outcome.value().next;
  }
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - start).count();
  DOM_NOTE("benchmark engine modes=" + std::to_string(kModes) +
           " records=" + std::to_string(kMetrics + 1) +
           " evaluations=" + std::to_string(kEvaluations) + " decided=" + std::to_string(decided) +
           " elapsed-ms=" + std::to_string(static_cast<std::uint64_t>(seconds * 1000.0)) +
           " evaluations-per-second=" +
           std::to_string(static_cast<std::uint64_t>(kEvaluations / seconds)));
  DOM_CHECK_EQ(decided, static_cast<std::uint64_t>(kEvaluations));
  DOM_NOTE("benchmark engine-provenance REAL single-process-cpu SYNTHETIC "
           "facility-model,mode-catalog,measurements");
}

DOM_TEST(durable_publication_throughput) {
  constexpr std::size_t kModes = 32;
  constexpr std::size_t kMetrics = 512;
  constexpr int kCommits = 80;
  const PolicyDocument policy = MakeLargePolicy(kModes);
  DOM_CHECK_OK(ValidatePolicy(policy));
  auto directory = dom::test::TempDir::Create("scale-durable");
  DOM_CHECK_OK(directory);
  auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 9901);
  auto coordinator = Coordinator::Open(options);
  DOM_CHECK_OK(coordinator);

  std::uint64_t now = 100'000;
  std::uint64_t tick = 1;
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t published = 0;
  for (int i = 0; i < kCommits; ++i) {
    auto status = coordinator.value()->GetStatus();
    DOM_CHECK_OK(status);
    auto evidence = MakeLargeEvidence(kMetrics, i % 2 == 0 ? 1 : 0,
                                      static_cast<std::uint64_t>(i + 1), now);
    EvaluationRequest request;
    request.envelope.idempotency_key = Sha256::Of("durable-" + std::to_string(i));
    request.envelope.expected_state_sequence = status.value().state_sequence;
    request.envelope.expected_epoch = status.value().epoch;
    request.context.epoch = status.value().epoch;
    request.context.incarnation = status.value().incarnation;
    request.context.now_ms = Instant::FromValue(now);
    request.context.tick = Tick::FromValue(tick);
    request.evidence = evidence;
    auto outcome = coordinator.value()->Evaluate(request);
    DOM_CHECK_OK(outcome);
    if (outcome.value().state_changed) {
      ++published;
    }
    now += 1000;
    ++tick;
  }
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - start).count();
  const std::uint64_t bytes = coordinator.value()->PublishedBytes();
  DOM_NOTE("benchmark durable modes=" + std::to_string(kModes) +
           " records=" + std::to_string(kMetrics + 1) +
           " evaluations=" + std::to_string(kCommits) +
           " committed-generations=" + std::to_string(coordinator.value()->PublishedGenerations()) +
           " published-bytes=" + std::to_string(bytes) +
           " bytes-per-generation=" +
           std::to_string(published == 0 ? 0 : bytes / coordinator.value()->PublishedGenerations()) +
           " elapsed-ms=" + std::to_string(static_cast<std::uint64_t>(seconds * 1000.0)) +
           " commits-per-second=" +
           std::to_string(static_cast<std::uint64_t>(published / seconds)));
  DOM_CHECK(published > 0);
  DOM_CHECK(coordinator.value()->PublishedGenerations() >= published);

  auto final_status = coordinator.value()->GetStatus();
  DOM_CHECK_OK(final_status);
  DOM_CHECK_EQ(final_status.value().state_sequence.value(),
               final_status.value().decision_sequence.value() + 1);
  DOM_CHECK(final_status.value().history_entries <= 4096u);
  DOM_NOTE("benchmark durable-provenance REAL local-filesystem-flush,rename,journal "
           "SYNTHETIC facility-model,mode-catalog,measurements");
}

}  // namespace
