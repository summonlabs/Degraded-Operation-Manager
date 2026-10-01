// Degraded Operation Manager - real multiprocess and crash tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every case here uses independent operating system processes: a holder that is
// locked out, a holder that is killed abruptly, a publisher that is killed at a
// real durable publication boundary, and a fresh process that reopens the store.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/process.hpp"
#include "testkit/testkit.hpp"

namespace {

using namespace dom;

struct ChildResult {
  std::uint32_t exit_code = 0;
  std::vector<std::string> lines;

  bool HasLineStartingWith(const std::string& prefix) const {
    for (const std::string& line : lines) {
      if (line.rfind(prefix, 0) == 0) {
        return true;
      }
    }
    return false;
  }

  std::string LineStartingWith(const std::string& prefix) const {
    for (const std::string& line : lines) {
      if (line.rfind(prefix, 0) == 0) {
        return line;
      }
    }
    return std::string();
  }
};

Result<std::string> ChildExecutable() {
  auto directory = dom::test::ExecutableDirectory();
  if (!directory) {
    return Result<std::string>::Err(directory.status());
  }
  return Result<std::string>::Ok(dom::test::Join(directory.value(), "dom_test_child.exe"));
}

/// Runs a child process to completion and collects its output.
Result<ChildResult> RunChild(const std::string& executable,
                             const std::vector<std::string>& arguments) {
  auto child = dom::test::ChildProcess::Spawn(executable, arguments);
  if (!child) {
    return Result<ChildResult>::Err(child.status());
  }
  ChildResult result;
  for (;;) {
    auto line = child.value().ReadLine();
    if (!line) {
      break;
    }
    result.lines.push_back(line.value());
  }
  auto code = child.value().WaitForExit();
  if (!code) {
    return Result<ChildResult>::Err(code.status());
  }
  result.exit_code = code.value();
  return Result<ChildResult>::Ok(std::move(result));
}

std::uint64_t ExtractNumber(const std::string& line, const std::string& key) {
  const std::string needle = key + "=";
  const std::size_t position = line.find(needle);
  if (position == std::string::npos) {
    return 0;
  }
  std::uint64_t value = 0;
  std::size_t index = position + needle.size();
  while (index < line.size() && line[index] >= '0' && line[index] <= '9') {
    value = value * 10ull + static_cast<std::uint64_t>(line[index] - '0');
    ++index;
  }
  return value;
}

struct Fixture {
  dom::test::TempDir directory;
  std::string policy_path;
  std::string escalation_evidence;
  std::string healthy_evidence;

  static Result<Fixture> Create(const std::string& label, const PolicyDocument& policy,
                                const EvidenceSnapshot& escalation,
                                const EvidenceSnapshot& healthy) {
    auto directory = dom::test::TempDir::Create(label);
    if (!directory) {
      return Result<Fixture>::Err(directory.status());
    }
    Fixture fixture;
    fixture.directory = std::move(directory.value());
    fixture.policy_path = dom::test::Join(fixture.directory.path(), "policy.txt");
    fixture.escalation_evidence = dom::test::Join(fixture.directory.path(), "escalate.txt");
    fixture.healthy_evidence = dom::test::Join(fixture.directory.path(), "healthy.txt");
    Status status = dom::test::WriteTextFile(fixture.policy_path, EncodePolicyText(policy));
    if (!status.ok()) {
      return Result<Fixture>::Err(status);
    }
    status = dom::test::WriteTextFile(fixture.escalation_evidence,
                                      EncodeEvidenceText(escalation));
    if (!status.ok()) {
      return Result<Fixture>::Err(status);
    }
    status = dom::test::WriteTextFile(fixture.healthy_evidence, EncodeEvidenceText(healthy));
    if (!status.ok()) {
      return Result<Fixture>::Err(status);
    }
    return Result<Fixture>::Ok(std::move(fixture));
  }
};

DOM_TEST(a_second_process_is_refused_while_the_holder_lives) {
  auto executable = ChildExecutable();
  DOM_CHECK_OK(executable);
  const PolicyDocument policy = dom::test::MakePolicy();
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 1000;
  auto fixture = Fixture::Create("mp-refusal", policy,
                                 dom::test::MakeEvidence(options),
                                 dom::test::MakeEvidence(options));
  DOM_CHECK_OK(fixture);

  auto holder = dom::test::ChildProcess::Spawn(
      executable.value(),
      {"hold", "--store", fixture.value().directory.path(), "--policy",
       fixture.value().policy_path, "--incarnation", "7001"});
  DOM_CHECK_OK(holder);
  auto ready = holder.value().ReadLine();
  DOM_CHECK_OK(ready);
  DOM_CHECK(ready.value().rfind("ready", 0) == 0);

  auto contender = RunChild(executable.value(),
                            {"try-open", "--store", fixture.value().directory.path(), "--policy",
                             fixture.value().policy_path, "--incarnation", "7002"});
  DOM_CHECK_OK(contender);
  DOM_CHECK_EQ(contender.value().exit_code, 3u);
  DOM_CHECK(contender.value().HasLineStartingWith("locked"));
  DOM_CHECK_EQ(contender.value().LineStartingWith("locked"), std::string("locked code=locked"));

  // The holder is still alive and still owns the store.
  DOM_CHECK_OK(holder.value().WriteLine("status"));
  auto status = holder.value().ReadLine();
  DOM_CHECK_OK(status);
  DOM_CHECK(status.value().rfind("status", 0) == 0);
  DOM_CHECK_OK(holder.value().WriteLine("exit"));
  auto bye = holder.value().ReadLine();
  DOM_CHECK_OK(bye);
  DOM_CHECK_EQ(bye.value(), std::string("bye"));
  auto code = holder.value().WaitForExit();
  DOM_CHECK_OK(code);
  DOM_CHECK_EQ(code.value(), 0u);
}

DOM_TEST(abrupt_holder_death_releases_the_lock_and_requires_a_new_epoch) {
  auto executable = ChildExecutable();
  DOM_CHECK_OK(executable);
  const PolicyDocument policy = dom::test::MakePolicy();
  dom::test::EvidenceOptions options;
  options.observed_at_ms = 1000;
  auto fixture = Fixture::Create("mp-death", policy, dom::test::MakeEvidence(options),
                                 dom::test::MakeEvidence(options));
  DOM_CHECK_OK(fixture);

  auto holder = dom::test::ChildProcess::Spawn(
      executable.value(),
      {"hold", "--store", fixture.value().directory.path(), "--policy",
       fixture.value().policy_path, "--incarnation", "7101"});
  DOM_CHECK_OK(holder);
  auto ready = holder.value().ReadLine();
  DOM_CHECK_OK(ready);
  const std::uint64_t held_epoch = ExtractNumber(ready.value(), "epoch");
  DOM_CHECK(held_epoch >= 1);

  // Abrupt, silent death while it owns the store.
  holder.value().Terminate();

  auto successor = RunChild(executable.value(),
                            {"try-open", "--store", fixture.value().directory.path(), "--policy",
                             fixture.value().policy_path, "--incarnation", "7102"});
  DOM_CHECK_OK(successor);
  DOM_CHECK_EQ(successor.value().exit_code, 0u);
  DOM_CHECK(successor.value().HasLineStartingWith("opened"));
  DOM_CHECK(successor.value().LineStartingWith("opened").find("adopted=false") !=
            std::string::npos);

  // The successor does not inherit mutation authority: it must adopt explicitly.
  auto refused = RunChild(executable.value(),
                          {"evaluate-no-adopt", "--store", fixture.value().directory.path(),
                           "--policy", fixture.value().policy_path, "--evidence",
                           fixture.value().healthy_evidence, "--now-ms", "5000", "--tick", "2",
                           "--incarnation", "7102"});
  DOM_CHECK_OK(refused);
  DOM_CHECK_EQ(refused.value().exit_code, 5u);
  DOM_CHECK_EQ(refused.value().LineStartingWith("evaluate-error"),
               std::string("evaluate-error code=stale-authority"));

  auto adopted = RunChild(executable.value(),
                          {"commit", "--store", fixture.value().directory.path(), "--policy",
                           fixture.value().policy_path, "--evidence",
                           fixture.value().escalation_evidence, "--now-ms", "6000", "--tick", "3",
                           "--incarnation", "7102"});
  DOM_CHECK_OK(adopted);
  DOM_CHECK_EQ(adopted.value().exit_code, 0u);
  DOM_CHECK(adopted.value().HasLineStartingWith("committed"));

  auto status = RunChild(executable.value(),
                         {"reopen-status", "--store", fixture.value().directory.path()});
  DOM_CHECK_OK(status);
  DOM_CHECK_EQ(status.value().exit_code, 0u);
  const std::uint64_t new_epoch = ExtractNumber(status.value().LineStartingWith("state"), "epoch");
  DOM_CHECK(new_epoch > held_epoch);
}

DOM_TEST(crash_at_every_publication_boundary_recovers_to_a_whole_generation) {
  auto executable = ChildExecutable();
  DOM_CHECK_OK(executable);
  const PolicyDocument policy = dom::test::MakePolicy();
  dom::test::EvidenceOptions healthy_options;
  healthy_options.observed_at_ms = 1000;
  dom::test::EvidenceOptions escalation_options;
  escalation_options.observed_at_ms = 2000;
  escalation_options.severity = 2;
  auto fixture = Fixture::Create("mp-crash", policy, dom::test::MakeEvidence(escalation_options),
                                 dom::test::MakeEvidence(healthy_options));
  DOM_CHECK_OK(fixture);

  for (std::uint64_t stage = 0; stage <= 4; ++stage) {
    const std::string directory =
        dom::test::Join(fixture.value().directory.path(), "store-" + std::to_string(stage));
    auto baseline = RunChild(executable.value(),
                             {"commit", "--store", directory, "--policy",
                              fixture.value().policy_path, "--evidence",
                              fixture.value().healthy_evidence, "--now-ms", "1000", "--tick", "1",
                              "--incarnation", "7201"});
    DOM_CHECK_OK(baseline);
    DOM_CHECK_EQ(baseline.value().exit_code, 0u);
    const std::uint64_t baseline_sequence =
        ExtractNumber(baseline.value().LineStartingWith("committed"), "state-sequence");
    DOM_CHECK(baseline_sequence >= 1);

    auto publisher = dom::test::ChildProcess::Spawn(
        executable.value(),
        {"crash", "--store", directory, "--policy", fixture.value().policy_path, "--evidence",
         fixture.value().escalation_evidence, "--now-ms", "2000", "--tick", "2", "--stage",
         std::to_string(stage), "--incarnation", "7202"});
    DOM_CHECK_OK(publisher);
    std::string observed;
    for (;;) {
      auto line = publisher.value().ReadLine();
      DOM_CHECK_OK(line);
      if (line.value().rfind("at-stage", 0) == 0) {
        observed = line.value();
        break;
      }
      if (line.value().rfind("evaluate-error", 0) == 0) {
        DOM_CHECK(false);
        break;
      }
    }
    DOM_CHECK_EQ(observed, std::string("at-stage ") + std::to_string(stage));
    publisher.value().Terminate();

    auto recovered = RunChild(executable.value(), {"reopen-status", "--store", directory});
    DOM_CHECK_OK(recovered);
    DOM_CHECK_EQ(recovered.value().exit_code, 0u);
    const std::string line = recovered.value().LineStartingWith("state");
    const std::uint64_t sequence = ExtractNumber(line, "sequence");
    if (stage <= 2) {
      // The publish never happened: the previous whole generation is authoritative.
      DOM_CHECK_EQ(sequence, baseline_sequence);
    } else {
      // The atomic rename happened: the new generation is authoritative.
      DOM_CHECK_EQ(sequence, baseline_sequence + 1);
    }
    if (stage == 2) {
      DOM_CHECK(line.find("recovery=discarded-unpublished") != std::string::npos);
    }
    if (stage == 3) {
      DOM_CHECK(line.find("recovery=repaired-commit-marker") != std::string::npos);
    }
    if (stage == 4) {
      DOM_CHECK(line.find("recovery=loaded") != std::string::npos);
    }

    // The recovered store accepts a new commit from a fresh process.
    auto after = RunChild(executable.value(),
                          {"commit", "--store", directory, "--policy",
                           fixture.value().policy_path, "--evidence",
                           fixture.value().healthy_evidence, "--now-ms", "9000", "--tick", "9",
                           "--incarnation", "7203"});
    DOM_CHECK_OK(after);
    DOM_CHECK_EQ(after.value().exit_code, 0u);
  }
}

DOM_TEST(authority_from_a_previous_epoch_is_fenced_after_recovery) {
  auto executable = ChildExecutable();
  DOM_CHECK_OK(executable);
  const PolicyDocument policy = dom::test::MakePolicy();
  dom::test::EvidenceOptions healthy_options;
  healthy_options.observed_at_ms = 1000;
  dom::test::EvidenceOptions escalation_options;
  escalation_options.observed_at_ms = 2000;
  escalation_options.severity = 3;
  escalation_options.redundancy = 400;
  auto fixture = Fixture::Create("mp-fencing", policy,
                                 dom::test::MakeEvidence(escalation_options),
                                 dom::test::MakeEvidence(healthy_options));
  DOM_CHECK_OK(fixture);
  const std::string directory = dom::test::Join(fixture.value().directory.path(), "store");

  auto baseline = RunChild(executable.value(),
                           {"commit", "--store", directory, "--policy",
                            fixture.value().policy_path, "--evidence",
                            fixture.value().escalation_evidence, "--now-ms", "1000", "--tick", "1",
                            "--incarnation", "7301"});
  DOM_CHECK_OK(baseline);
  DOM_CHECK_EQ(baseline.value().exit_code, 0u);

  auto authorization = RunChild(executable.value(),
                                {"authorize", "--store", directory, "--policy",
                                 fixture.value().policy_path, "--now-ms", "1500", "--ttl-ms",
                                 "600000", "--incarnation", "7301"});
  DOM_CHECK_OK(authorization);
  DOM_CHECK_EQ(authorization.value().exit_code, 0u);
  const std::uint64_t identifier =
      ExtractNumber(authorization.value().LineStartingWith("authorized"), "id");
  DOM_CHECK(identifier >= 1);

  // Kill a publisher at the commit point, then recover with a new epoch.
  auto publisher = dom::test::ChildProcess::Spawn(
      executable.value(),
      {"crash", "--store", directory, "--policy", fixture.value().policy_path, "--evidence",
       fixture.value().healthy_evidence, "--now-ms", "3000", "--tick", "3", "--stage", "4",
       "--incarnation", "7302"});
  DOM_CHECK_OK(publisher);
  auto parked = publisher.value().ReadLine();
  DOM_CHECK_OK(parked);
  DOM_CHECK_EQ(parked.value(), std::string("at-stage 4"));
  publisher.value().Terminate();

  auto verification = RunChild(executable.value(),
                               {"verify", "--store", directory, "--policy",
                                fixture.value().policy_path, "--authorization",
                                std::to_string(identifier), "--owner", "adjacent-owner", "--now-ms",
                                "4000", "--incarnation", "7303"});
  DOM_CHECK_OK(verification);
  DOM_CHECK_EQ(verification.value().exit_code, 0u);
  DOM_CHECK_EQ(verification.value().LineStartingWith("verified").find("stale=true") !=
                   std::string::npos,
               true);
}

}  // namespace
