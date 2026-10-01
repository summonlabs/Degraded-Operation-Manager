// Degraded Operation Manager - concurrency and race tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// These cases race real threads through one authority. The invariant they check
// is that concurrent evidence changes still commit one deterministic sequence of
// mode states, with no duplicate or skipped generation.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

namespace {

using namespace dom;

struct Envelope {
  StateSequence state_sequence;
  ControlEpoch epoch;
  Incarnation incarnation;
  Instant since_ms;
};

Result<Envelope> CurrentEnvelope(Coordinator& coordinator) {
  auto status = coordinator.GetStatus();
  if (!status) {
    return Result<Envelope>::Err(status.status());
  }
  Envelope envelope;
  envelope.state_sequence = status.value().state_sequence;
  envelope.epoch = status.value().epoch;
  envelope.incarnation = status.value().incarnation;
  envelope.since_ms = status.value().since_ms;
  return Result<Envelope>::Ok(envelope);
}

EvaluationRequest MakeRequest(const Envelope& envelope, const EvidenceSnapshot& evidence,
                              const std::string& key, std::uint64_t now_ms, std::uint64_t tick) {
  EvaluationRequest request;
  request.envelope.idempotency_key = Sha256::Of(key);
  request.envelope.expected_state_sequence = envelope.state_sequence;
  request.envelope.expected_epoch = envelope.epoch;
  request.context.epoch = envelope.epoch;
  request.context.incarnation = envelope.incarnation;
  request.context.now_ms = Instant::FromValue(now_ms);
  request.context.tick = Tick::FromValue(tick);
  request.evidence = evidence;
  return request;
}

DOM_TEST(concurrent_evaluations_commit_one_deterministic_sequence) {
  auto directory = dom::test::TempDir::Create("race-evaluate");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 8101);
  auto coordinator = Coordinator::Open(options);
  DOM_CHECK_OK(coordinator);

  constexpr int kThreads = 4;
  constexpr int kAttempts = 25;
  // A shared monotonic clock: evaluation instants never move backwards, which
  // is what a real caller's clock guarantees.
  std::atomic<std::uint64_t> clock{1};
  // Retry budget per attempt; it is generous and never a timeout.
  std::atomic<int> committed{0};
  std::atomic<int> refused{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&coordinator, &committed, &refused, &clock, thread]() {
      for (int attempt = 0; attempt < kAttempts; ++attempt) {
        for (int retry = 0; retry < 64; ++retry) {
          auto envelope = CurrentEnvelope(*coordinator.value());
          if (!envelope) {
            ++refused;
            break;
          }
          const std::uint64_t stamp = clock.fetch_add(1) + 1;
          // The caller's clock never reports an instant before the mode it is
          // reasoning about started; a real clock is monotonic, and the engine
          // refuses a backwards instant rather than guessing.
          const std::uint64_t now =
              (std::max)(1000 + stamp * 500, envelope.value().since_ms.value() + 1);
          dom::test::EvidenceOptions evidence_options;
          evidence_options.revision = stamp;
          evidence_options.observed_at_ms = now;
          evidence_options.severity = (thread + attempt) % 2 == 0 ? 2 : 0;
          evidence_options.redundancy = 1000;
          auto evidence = dom::test::MakeEvidence(evidence_options);
          auto request = MakeRequest(envelope.value(), evidence,
                                     "race-" + std::to_string(thread) + "-" +
                                         std::to_string(attempt) + "-" + std::to_string(retry),
                                     now, static_cast<std::uint64_t>(attempt + 1));
          auto outcome = coordinator.value()->Evaluate(request);
          if (outcome) {
            if (outcome.value().state_changed) {
              ++committed;
            }
            break;
          }
          // Under contention only a stale precondition may be retried; any
          // other failure is a defect and is reported as such.
          if (outcome.status().code() == ErrorCode::PreconditionFailed ||
              outcome.status().code() == ErrorCode::StaleAuthority) {
            continue;
          }
          DOM_CHECK_EQ(outcome.status().code(), ErrorCode::PreconditionFailed);
          ++refused;
          break;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  auto status = coordinator.value()->GetStatus();
  DOM_CHECK_OK(status);
  DOM_CHECK_EQ(status.value().state_sequence.value(), status.value().decision_sequence.value() + 1);
  DOM_CHECK_EQ(committed.load() + 1,
               static_cast<int>(status.value().state_sequence.value()));
  DOM_NOTE("race committed=" + std::to_string(committed.load()) +
           " refused=" + std::to_string(refused.load()) +
           " final-sequence=" + status.value().state_sequence.ToString());

  auto history = coordinator.value()->History(1000000);
  DOM_CHECK_OK(history);
  DOM_CHECK_EQ(history.value().size(), status.value().decision_sequence.value());
  for (std::size_t i = 1; i < history.value().size(); ++i) {
    DOM_CHECK(history.value()[i - 1].state_sequence < history.value()[i].state_sequence);
    DOM_CHECK(history.value()[i - 1].decision_sequence < history.value()[i].decision_sequence);
  }
  // The store survived the race and still verifies on disk.
  DOM_CHECK_EQ(coordinator.value()->state().digest(),
               coordinator.value()->state().digest());
}

DOM_TEST(the_same_idempotency_key_commits_at_most_once) {
  auto directory = dom::test::TempDir::Create("race-idempotent");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 8201);
  auto coordinator = Coordinator::Open(options);
  DOM_CHECK_OK(coordinator);

  dom::test::EvidenceOptions evidence_options;
  evidence_options.revision = 5;
  evidence_options.observed_at_ms = 10'000;
  evidence_options.severity = 2;
  auto evidence = dom::test::MakeEvidence(evidence_options);
  auto envelope = CurrentEnvelope(*coordinator.value());
  DOM_CHECK_OK(envelope);
  // Every thread sends the identical request, so the request digest matches.
  const EvaluationRequest request =
      MakeRequest(envelope.value(), evidence, "shared-key", 10'000, 7);

  constexpr int kThreads = 6;
  std::atomic<int> changes{0};
  std::atomic<int> replays{0};
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&coordinator, &request, &changes, &replays, &failures]() {
      auto outcome = coordinator.value()->Evaluate(request);
      if (!outcome) {
        ++failures;
        return;
      }
      if (outcome.value().replayed) {
        ++replays;
      }
      if (outcome.value().state_changed) {
        ++changes;
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  DOM_CHECK_EQ(changes.load(), 1);
  DOM_CHECK_EQ(replays.load(), kThreads - 1);
  DOM_CHECK_EQ(failures.load(), 0);
  auto status = coordinator.value()->GetStatus();
  DOM_CHECK_OK(status);
  DOM_CHECK_EQ(status.value().state_sequence.value(), 2ull);
}

DOM_TEST(readers_observe_monotonic_generations_while_writers_commit) {
  auto directory = dom::test::TempDir::Create("race-readers");
  DOM_CHECK_OK(directory);
  const PolicyDocument policy = dom::test::MakePolicy();
  auto options = dom::test::MakeCoordinatorOptions(directory.value().path(), policy, 8301);
  auto coordinator = Coordinator::Open(options);
  DOM_CHECK_OK(coordinator);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> observed_max{0};
  std::atomic<bool> monotonic{true};
  std::thread reader([&coordinator, &stop, &observed_max, &monotonic]() {
    std::uint64_t previous = 0;
    while (!stop.load()) {
      auto status = coordinator.value()->GetStatus();
      if (!status) {
        continue;
      }
      const std::uint64_t current = status.value().state_sequence.value();
      if (current < previous) {
        monotonic.store(false);
      }
      previous = current;
      const std::uint64_t seen = observed_max.load();
      if (current > seen) {
        observed_max.store(current);
      }
    }
  });

  for (int step = 0; step < 40; ++step) {
    auto envelope = CurrentEnvelope(*coordinator.value());
    DOM_CHECK_OK(envelope);
    dom::test::EvidenceOptions evidence_options;
    evidence_options.revision = static_cast<std::uint64_t>(step + 1);
    evidence_options.observed_at_ms = 5000 + static_cast<std::uint64_t>(step) * 1000;
    evidence_options.severity = step % 2 == 0 ? 2 : 0;
    auto evidence = dom::test::MakeEvidence(evidence_options);
    auto request = MakeRequest(envelope.value(), evidence, "reader-writer-" + std::to_string(step),
                               5000 + static_cast<std::uint64_t>(step) * 1000,
                               static_cast<std::uint64_t>(step + 1));
    auto outcome = coordinator.value()->Evaluate(request);
    DOM_CHECK_OK(outcome);
  }
  stop.store(true);
  reader.join();
  DOM_CHECK(monotonic.load());
  auto status = coordinator.value()->GetStatus();
  DOM_CHECK_OK(status);
  DOM_CHECK_EQ(observed_max.load(), status.value().state_sequence.value());
}

}  // namespace
