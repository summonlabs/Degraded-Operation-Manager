// Degraded Operation Manager - randomized property tests with a reference model.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The reference model is written independently of the engine: it evaluates the
// same fixture policy directly from the ground truth values the generator
// produced, and it tracks continuous-clearance time itself. The engine must
// never contradict it, and every invariant is checked after every step.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

namespace {

using namespace dom;

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}
  std::uint64_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  std::size_t Below(std::size_t bound) {
    return bound == 0 ? 0 : static_cast<std::size_t>(Next() % bound);
  }

 private:
  std::uint64_t state_;
};

/// One generated facility condition.
struct Truth {
  std::int64_t severity = 0;
  std::int64_t redundancy = 1000;
  std::int64_t cooling = 25000;
  bool has_cooling = true;
  bool conflicted = false;
  bool fresh = true;
};

/// Independent evaluation of the fixture policy directly from ground truth.
struct Reference {
  static bool EmergencySatisfied(const Truth& truth) {
    return truth.has_cooling && !truth.conflicted && truth.fresh && truth.cooling <= 5000;
  }
  static bool RestrictedSatisfied(const Truth& truth) {
    return !truth.conflicted && truth.fresh && truth.severity >= 3 && truth.redundancy <= 500;
  }
  static bool ConserveSatisfied(const Truth& truth) {
    return !truth.conflicted && truth.fresh && truth.severity >= 2;
  }
  static bool WatchSatisfied(const Truth& truth) {
    return !truth.conflicted && truth.fresh && truth.severity >= 1;
  }
  static bool NominalSatisfied(const Truth& truth) {
    return !truth.conflicted && truth.fresh && truth.severity <= 0 && truth.redundancy >= 1000;
  }
  static int MostRestrictiveRank(const Truth& truth) {
    if (EmergencySatisfied(truth)) {
      return 5;
    }
    if (RestrictedSatisfied(truth)) {
      return 3;
    }
    if (ConserveSatisfied(truth)) {
      return 2;
    }
    if (WatchSatisfied(truth)) {
      return 1;
    }
    return -1;
  }
  static int RetainedRank(Posture posture, int retained_mode_rank) {
    return posture == Posture::Indeterminate ? retained_mode_rank : static_cast<int>(posture);
  }
};

int RankOfMode(const PolicyDocument& policy, ModeId id) {
  auto mode = FindMode(policy, id);
  return mode ? SeverityRank(mode.value()->cls) : -1;
}

int RankOfPosture(const PolicyDocument& policy, const CommittedView& committed) {
  if (committed.posture == Posture::Indeterminate) {
    return RankOfMode(policy, committed.mode);
  }
  return static_cast<int>(committed.posture);
}

EvaluationInputs Inputs(const PolicyDocument& policy, const CommittedView& committed,
                        const EvidenceSnapshot& evidence, std::uint64_t now_ms, std::uint64_t tick) {
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

DOM_TEST(randomized_state_machine_matches_the_reference_model) {
  const std::uint64_t seed = 0x51D0A7E5ull;
  DOM_NOTE("property seed=" + std::to_string(seed) + " steps=400");
  Rng rng(seed);
  const PolicyDocument policy = dom::test::MakePolicy();
  const std::int64_t severities[] = {0, 0, 1, 2, 3, 3};
  const std::int64_t redundancies[] = {1000, 1000, 750, 500, 400};
  const std::int64_t coolings[] = {25000, 20000, 10000, 5000, 4000};

  CommittedView committed = dom::test::MakeCommittedView(policy, 1, 1000);
  std::uint64_t now = 1000;
  std::uint64_t tick = 1;
  bool model_healthy = true;
  std::uint64_t model_healthy_since = 1000;

  for (int step = 0; step < 400; ++step) {
    Truth truth;
    truth.severity = severities[rng.Below(std::size(severities))];
    truth.redundancy = redundancies[rng.Below(std::size(redundancies))];
    truth.cooling = coolings[rng.Below(std::size(coolings))];
    truth.has_cooling = rng.Below(8) != 0;
    truth.conflicted = rng.Below(16) == 0;
    truth.fresh = rng.Below(6) != 0;

    now += 500 + rng.Below(40000);
    const std::uint64_t observed = truth.fresh ? now : (now > 200000 ? now - 200000 : 0);

    dom::test::EvidenceOptions options;
    options.revision = static_cast<std::uint64_t>(step + 1);
    options.observed_at_ms = observed;
    options.severity = truth.severity;
    options.redundancy = truth.redundancy;
    options.cooling_margin = truth.cooling;
    options.include_cooling = truth.has_cooling;
    options.cooling_conflict = truth.conflicted;
    auto evidence = dom::test::MakeEvidence(options);
    // A missing cooling record makes the cooling requirement missing rather
    // than conflicted; keep the reference view consistent.
    if (!truth.has_cooling) {
      truth.conflicted = false;
    }

    const int before_rank = RankOfPosture(policy, committed);
    auto outcome = EvaluateMode(Inputs(policy, committed, evidence, now, tick));
    DOM_CHECK_OK(outcome);

    const EvaluationOutcome& evaluation = outcome.value();
    const CommittedView& next = evaluation.next;
    const int after_rank = RankOfPosture(policy, next);
    const bool healthy = Reference::NominalSatisfied(truth);
    const int retained_before = before_rank;
    const int expected_escalation = Reference::MostRestrictiveRank(truth);

    // I1: no relaxation unless the facility is genuinely healthy on fresh evidence.
    if (after_rank < retained_before) {
      DOM_CHECK(healthy);
      DOM_CHECK(truth.fresh);
      DOM_CHECK(!truth.conflicted);
      const auto mode = FindMode(policy, committed.mode);
      DOM_CHECK(mode.has_value());
      const std::uint64_t dwell = now - committed.since_ms.value();
      DOM_CHECK(dwell >= mode.value()->min_dwell_ms.value());
      DOM_CHECK(now - model_healthy_since >= mode.value()->recovery_hold_ms.value());
    }

    // I2: an unhealthy or unusable facility never relaxes the posture.
    if (!healthy) {
      DOM_CHECK(after_rank >= retained_before);
    }

    // I3: escalation always lands on the reference answer, never higher.
    if (evaluation.decision.verdict == Verdict::Escalate) {
      DOM_CHECK(expected_escalation > retained_before);
      DOM_CHECK_EQ(after_rank, expected_escalation);
    }

    // I4: an indeterminate posture keeps at least the restrictions of the mode
    // it retains.
    if (next.posture == Posture::Indeterminate) {
      const auto retained = FindMode(policy, next.mode);
      DOM_CHECK(retained.has_value());
      const RestrictionSet base = RestrictionsForMode(policy, *retained.value());
      for (const std::string& service : base.restricted_services) {
        DOM_CHECK(std::find(evaluation.restrictions.restricted_services.begin(),
                            evaluation.restrictions.restricted_services.end(),
                            service) != evaluation.restrictions.restricted_services.end());
      }
    }

    // I5: every decision is bound to the evidence and epoch it was made from.
    DOM_CHECK_EQ(evaluation.decision.generations.evidence.value(), evidence.revision().value());
    DOM_CHECK_EQ(evaluation.decision.generations.epoch.value(), committed.epoch.value());
    DOM_CHECK_EQ(evaluation.decision.generations.incarnation.value(), committed.incarnation.value());
    DOM_CHECK(!evaluation.decision.trace.empty());
    DOM_CHECK(!evaluation.decision.input_digest.IsZero());

    // I6: determinism - the same transition produces the same digests.
    auto replay = EvaluateMode(Inputs(policy, committed, evidence, now, tick));
    DOM_CHECK_OK(replay);
    DOM_CHECK_EQ(replay.value().decision.digest(), evaluation.decision.digest());

    committed = next;
    if (healthy && truth.fresh && !truth.conflicted) {
      if (!model_healthy) {
        model_healthy_since = now;
      }
      model_healthy = true;
    } else {
      model_healthy = false;
      model_healthy_since = now;
    }
    ++tick;
  }
  DOM_CHECK(!committed.mode.IsZero());
}

DOM_TEST(the_same_seed_reproduces_the_same_decisions) {
  const std::uint64_t seed = 424242;
  const PolicyDocument policy = dom::test::MakePolicy();
  std::vector<Digest> first_run;
  std::vector<Digest> second_run;
  for (int pass = 0; pass < 2; ++pass) {
    Rng rng(seed);
    CommittedView committed = dom::test::MakeCommittedView(policy, 1, 1000);
    std::uint64_t now = 1000;
    for (int step = 0; step < 120; ++step) {
      dom::test::EvidenceOptions options;
      options.revision = static_cast<std::uint64_t>(step + 1);
      now += 250 + rng.Below(30000);
      options.observed_at_ms = rng.Below(10) == 0 ? 0 : now;
      options.severity = static_cast<std::int64_t>(rng.Below(4));
      options.redundancy = static_cast<std::int64_t>(rng.Below(4)) * 250 + 250;
      options.cooling_margin = static_cast<std::int64_t>(rng.Below(5)) * 5000;
      options.include_cooling = rng.Below(8) != 0;
      auto evidence = dom::test::MakeEvidence(options);
      auto outcome = EvaluateMode(Inputs(policy, committed, evidence, now,
                                         static_cast<std::uint64_t>(step + 1)));
      DOM_CHECK_OK(outcome);
      if (pass == 0) {
        first_run.push_back(outcome.value().decision.digest());
      } else {
        second_run.push_back(outcome.value().decision.digest());
      }
      committed = outcome.value().next;
    }
  }
  DOM_CHECK_EQ(first_run.size(), second_run.size());
  for (std::size_t i = 0; i < first_run.size(); ++i) {
    DOM_CHECK_EQ(first_run[i], second_run[i]);
  }
}

DOM_TEST(durable_authority_survives_repeated_restarts) {
  auto directory = dom::test::TempDir::Create("property-restart");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  Rng rng(0xABCDEF);
  std::uint64_t now = 1000;
  std::vector<Digest> decision_digests;
  StateSequence last_sequence;

  for (int cycle = 0; cycle < 6; ++cycle) {
    const std::uint64_t stamp = now;
    dom::test::EvidenceOptions options;
    options.revision = static_cast<std::uint64_t>(cycle + 1);
    options.observed_at_ms = stamp;
    options.severity = cycle % 2 == 0 ? 3 : 0;
    options.redundancy = cycle % 2 == 0 ? 400 : 1000;
    auto evidence = dom::test::MakeEvidence(options);
    now += 100000;

    auto options_coordinator =
        dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 5000 + cycle);
    auto coordinator = Coordinator::Open(options_coordinator);
    DOM_CHECK_OK(coordinator);
    auto status = coordinator.value()->GetStatus();
    DOM_CHECK_OK(status);
    if (!status.value().epoch_adopted) {
      AdoptEpochRequest adopt;
      adopt.expected_epoch = status.value().epoch;
      adopt.new_incarnation = Incarnation::FromValue(9000 + cycle);
      adopt.authority_reference = "property-test";
      DOM_CHECK_OK(coordinator.value()->AdoptEpoch(adopt));
      status = coordinator.value()->GetStatus();
      DOM_CHECK_OK(status);
    }
    DOM_CHECK(last_sequence <= status.value().state_sequence);
    last_sequence = status.value().state_sequence;

    EvaluationRequest request;
    request.envelope.idempotency_key = Sha256::Of("cycle-" + std::to_string(cycle));
    request.envelope.expected_state_sequence = status.value().state_sequence;
    request.envelope.expected_epoch = status.value().epoch;
    request.context.epoch = status.value().epoch;
    request.context.incarnation = status.value().incarnation;
    request.context.now_ms = Instant::FromValue(stamp);
    request.context.tick = Tick::FromValue(static_cast<std::uint64_t>(cycle + 1));
    request.evidence = evidence;
    auto outcome = coordinator.value()->Evaluate(request);
    DOM_CHECK_OK(outcome);
    decision_digests.push_back(outcome.value().decision.digest());
    last_sequence = outcome.value().state_sequence;
    // The coordinator is destroyed here: the next cycle is a new process image.
  }

  auto options_coordinator = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 7777);
  auto reopened = Coordinator::Open(options_coordinator);
  DOM_CHECK_OK(reopened);
  auto status = reopened.value()->GetStatus();
  DOM_CHECK_OK(status);
  DOM_CHECK_EQ(status.value().state_sequence.value(), last_sequence.value());
  DOM_CHECK_EQ(status.value().decision_sequence.value(),
               static_cast<std::uint64_t>(decision_digests.size()));
  // Mutating without adopting the epoch is refused even though the store opened.
  EvaluationRequest request;
  request.envelope.idempotency_key = Sha256::Of(std::string_view("after-restart"));
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = Instant::FromValue(now + 1000);
  request.context.tick = Tick::FromValue(99);
  request.evidence = dom::test::MakeEvidence(dom::test::EvidenceOptions{});
  DOM_CHECK_ERR(reopened.value()->Evaluate(request), ErrorCode::StaleAuthority);
}

}  // namespace
