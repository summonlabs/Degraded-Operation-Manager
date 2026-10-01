// Degraded Operation Manager - helper process for multiprocess and crash tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This binary is not a product tool and is not installed. It is the independent
// operating system process that the multiprocess suite locks out, kills at real
// publication boundaries, and reopens from. Every command prints one line and
// flushes it, so the parent can synchronise without guessing.

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/process.hpp"

namespace {

using namespace dom;

struct Options {
  std::string command;
  std::string store;
  std::string policy;
  std::string evidence;
  std::uint64_t incarnation = 0;
  std::uint64_t now_ms = 0;
  std::uint64_t tick = 1;
  std::uint64_t stage = 0;
  std::uint64_t authorization = 0;
  std::uint64_t ttl_ms = 60000;
  std::string key;
  std::string owner = "test-child";
  bool has_key = false;
};

Result<Options> ParseOptions(int argc, char** argv) {
  Options options;
  if (argc < 2) {
    return Result<Options>::Err(ErrorCode::InvalidArgument, "a command is required");
  }
  options.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string name = argv[i];
    if (i + 1 >= argc) {
      return Result<Options>::Err(ErrorCode::InvalidArgument, "option " + name + " needs a value");
    }
    const std::string value = argv[++i];
    auto number = [&value](std::uint64_t& target) -> Status {
      std::uint64_t parsed = 0;
      for (char c : value) {
        if (c < '0' || c > '9') {
          return Status::Error(ErrorCode::InvalidArgument, "expected a decimal integer");
        }
        parsed = parsed * 10ull + static_cast<std::uint64_t>(c - '0');
      }
      target = parsed;
      return Status::Ok();
    };
    Status status = Status::Ok();
    if (name == "--store") {
      options.store = value;
    } else if (name == "--policy") {
      options.policy = value;
    } else if (name == "--evidence") {
      options.evidence = value;
    } else if (name == "--key") {
      options.key = value;
      options.has_key = true;
    } else if (name == "--owner") {
      options.owner = value;
    } else if (name == "--incarnation") {
      status = number(options.incarnation);
    } else if (name == "--now-ms") {
      status = number(options.now_ms);
    } else if (name == "--tick") {
      status = number(options.tick);
    } else if (name == "--stage") {
      status = number(options.stage);
    } else if (name == "--authorization") {
      status = number(options.authorization);
    } else if (name == "--ttl-ms") {
      status = number(options.ttl_ms);
    } else {
      return Result<Options>::Err(ErrorCode::InvalidArgument, "unknown option " + name);
    }
    if (!status.ok()) {
      return Result<Options>::Err(status);
    }
  }
  return Result<Options>::Ok(std::move(options));
}

Result<std::string> ReadWholeFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Result<std::string>::Err(ErrorCode::NotFound, "cannot open " + path);
  }
  std::string text;
  std::vector<char> buffer(8192);
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = stream.gcount();
    if (got <= 0) {
      break;
    }
    text.append(buffer.data(), static_cast<std::size_t>(got));
  }
  return Result<std::string>::Ok(std::move(text));
}

Result<PolicyDocument> LoadPolicyFile(const std::string& path) {
  auto text = ReadWholeFile(path);
  if (!text) {
    return Result<PolicyDocument>::Err(text.status());
  }
  return DecodePolicyText(text.value());
}

Result<EvidenceSnapshot> LoadEvidenceFile(const std::string& path) {
  auto text = ReadWholeFile(path);
  if (!text) {
    return Result<EvidenceSnapshot>::Err(text.status());
  }
  return DecodeEvidenceText(text.value());
}

Result<std::unique_ptr<Coordinator>> OpenCoordinator(const Options& options,
                                                    PolicyDocument policy,
                                                    bool adopt) {
  CoordinatorOptions coordinator_options;
  coordinator_options.directory = options.store;
  coordinator_options.policy = std::move(policy);
  coordinator_options.incarnation = Incarnation::FromValue(options.incarnation);
  coordinator_options.max_authorization_ttl_ms = Duration::FromValue(600000);
  auto coordinator = Coordinator::Open(coordinator_options);
  if (!coordinator) {
    return coordinator;
  }
  if (!adopt) {
    return coordinator;
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    return Result<std::unique_ptr<Coordinator>>::Err(status.status());
  }
  if (!status.value().epoch_adopted) {
    AdoptEpochRequest request;
    request.expected_epoch = status.value().epoch;
    request.new_incarnation = Incarnation::FromValue(options.incarnation);
    request.authority_reference = "test-child";
    auto adopted = coordinator.value()->AdoptEpoch(request);
    if (!adopted) {
      return Result<std::unique_ptr<Coordinator>>::Err(adopted.status());
    }
  }
  return coordinator;
}

Digest KeyFor(const Options& options, const std::string& fallback) {
  if (options.has_key) {
    auto digest = Digest::FromHex(options.key);
    if (digest) {
      return digest.value();
    }
  }
  return Sha256::Of(fallback);
}

/// Blocks forever, so the parent can terminate this process at exactly this
/// durable publication boundary.
void ParkForever() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
}

int CommandTryOpen(const Options& options) {
  auto policy = LoadPolicyFile(options.policy);
  if (!policy) {
    std::cout << "policy-error " << policy.status().ToString() << "\n" << std::flush;
    return 2;
  }
  auto coordinator = OpenCoordinator(options, policy.value(), false);
  if (!coordinator) {
    std::cout << "locked code=" << ErrorCodeName(coordinator.status().code()) << "\n" << std::flush;
    return 3;
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    std::cout << "status-error " << status.status().ToString() << "\n" << std::flush;
    return 2;
  }
  std::cout << "opened state-sequence=" << status.value().state_sequence.ToString()
            << " epoch=" << status.value().epoch.ToString()
            << " adopted=" << (status.value().epoch_adopted ? "true" : "false") << "\n"
            << std::flush;
  return 0;
}

int CommandReopenStatus(const Options& options) {
  StoreOptions store_options;
  store_options.directory = options.store;
  store_options.create_if_missing = false;
  auto store = StateStore::Open(store_options);
  if (!store) {
    std::cout << "open-error code=" << ErrorCodeName(store.status().code())
              << " message=" << store.status().message() << "\n" << std::flush;
    return 4;
  }
  std::cout << "state sequence=" << store.value()->state().sequence.ToString()
            << " digest=" << store.value()->state().digest().ToHex()
            << " recovery=" << RecoveryOutcomeName(store.value()->recovery().outcome)
            << " epoch=" << store.value()->state().committed.epoch.ToString()
            << " posture=" << PostureName(store.value()->state().committed.posture)
            << " detail=\"" << store.value()->recovery().detail << "\"\n"
            << std::flush;
  return 0;
}

int CommandCommit(const Options& options, bool adopt) {
  auto policy = LoadPolicyFile(options.policy);
  if (!policy) {
    std::cout << "policy-error " << policy.status().ToString() << "\n" << std::flush;
    return 2;
  }
  auto evidence = LoadEvidenceFile(options.evidence);
  if (!evidence) {
    std::cout << "evidence-error " << evidence.status().ToString() << "\n" << std::flush;
    return 2;
  }
  auto coordinator = OpenCoordinator(options, policy.value(), adopt);
  if (!coordinator) {
    std::cout << "open-error code=" << ErrorCodeName(coordinator.status().code()) << "\n"
              << std::flush;
    return 2;
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    std::cout << "status-error\n" << std::flush;
    return 2;
  }
  EvaluationRequest request;
  request.envelope.idempotency_key = KeyFor(options, "child-commit");
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = Instant::FromValue(options.now_ms);
  request.context.tick = Tick::FromValue(options.tick);
  request.evidence = evidence.value();
  auto outcome = coordinator.value()->Evaluate(request);
  if (!outcome) {
    std::cout << "evaluate-error code=" << ErrorCodeName(outcome.status().code()) << "\n"
              << std::flush;
    return 5;
  }
  std::cout << "committed state-sequence=" << outcome.value().state_sequence.ToString()
            << " changed=" << (outcome.value().state_changed ? "true" : "false")
            << " posture=" << PostureName(outcome.value().decision.posture) << "\n"
            << std::flush;
  return 0;
}

int CommandAuthorize(const Options& options) {
  auto policy = LoadPolicyFile(options.policy);
  if (!policy) {
    std::cout << "policy-error\n" << std::flush;
    return 2;
  }
  auto coordinator = OpenCoordinator(options, policy.value(), true);
  if (!coordinator) {
    std::cout << "open-error code=" << ErrorCodeName(coordinator.status().code()) << "\n"
              << std::flush;
    return 2;
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    std::cout << "status-error\n" << std::flush;
    return 2;
  }
  AuthorizationRequest request;
  request.envelope.idempotency_key = KeyFor(options, "child-authorize");
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.now_ms = Instant::FromValue(options.now_ms);
  request.ttl_ms = Duration::FromValue(options.ttl_ms);
  auto outcome = coordinator.value()->Authorize(request);
  if (!outcome) {
    std::cout << "authorize-error code=" << ErrorCodeName(outcome.status().code()) << "\n"
              << std::flush;
    return 5;
  }
  std::cout << "authorized id=" << outcome.value().authorization.id.ToString()
            << " bound-decision=" << outcome.value().authorization.bound_decision.ToString()
            << " epoch=" << outcome.value().authorization.epoch.ToString() << "\n"
            << std::flush;
  return 0;
}

int CommandVerify(const Options& options) {
  auto policy = LoadPolicyFile(options.policy);
  if (!policy) {
    std::cout << "policy-error\n" << std::flush;
    return 2;
  }
  auto coordinator = OpenCoordinator(options, policy.value(), true);
  if (!coordinator) {
    std::cout << "open-error code=" << ErrorCodeName(coordinator.status().code()) << "\n"
              << std::flush;
    return 2;
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    std::cout << "status-error\n" << std::flush;
    return 2;
  }
  VerificationRequest request;
  request.envelope.idempotency_key = KeyFor(options, "child-verify");
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.authorization = AuthorizationId::FromValue(options.authorization);
  request.adjacent_owner = options.owner;
  request.outcome = VerificationOutcome::Effective;
  request.observed_at_ms = Instant::FromValue(options.now_ms);
  request.effect_digest = Sha256::Of(std::string_view("effect"));
  request.source_digest = Sha256::Of(std::string_view("source"));
  auto outcome = coordinator.value()->RecordEffectVerification(request);
  if (!outcome) {
    std::cout << "verify-error code=" << ErrorCodeName(outcome.status().code()) << "\n" << std::flush;
    return 5;
  }
  std::cout << "verified stale=" << (outcome.value().stale_binding ? "true" : "false")
            << " state-sequence=" << outcome.value().state_sequence.ToString() << "\n"
            << std::flush;
  return 0;
}

int CommandCrash(const Options& options) {
  auto policy = LoadPolicyFile(options.policy);
  if (!policy) {
    std::cout << "policy-error\n" << std::flush;
    return 2;
  }
  auto evidence = LoadEvidenceFile(options.evidence);
  if (!evidence) {
    std::cout << "evidence-error\n" << std::flush;
    return 2;
  }
  CoordinatorOptions coordinator_options;
  coordinator_options.directory = options.store;
  coordinator_options.policy = policy.value();
  coordinator_options.incarnation = Incarnation::FromValue(options.incarnation);
  coordinator_options.max_authorization_ttl_ms = Duration::FromValue(600000);
  const std::uint64_t park_stage = options.stage;
  coordinator_options.commit_observer = [park_stage](CommitStage stage) {
    if (static_cast<std::uint64_t>(stage) != park_stage) {
      return;
    }
    std::cout << "at-stage " << static_cast<unsigned>(stage) << "\n" << std::flush;
    ParkForever();
  };
  auto coordinator = Coordinator::Open(coordinator_options);
  if (!coordinator) {
    std::cout << "open-error code=" << ErrorCodeName(coordinator.status().code()) << "\n"
              << std::flush;
    return 2;
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    std::cout << "status-error\n" << std::flush;
    return 2;
  }
  if (!status.value().epoch_adopted) {
    AdoptEpochRequest adopt;
    adopt.expected_epoch = status.value().epoch;
    adopt.new_incarnation = Incarnation::FromValue(options.incarnation);
    adopt.authority_reference = "crash-child";
    auto adopted = coordinator.value()->AdoptEpoch(adopt);
    if (!adopted) {
      std::cout << "adopt-error\n" << std::flush;
      return 2;
    }
    status = coordinator.value()->GetStatus();
    if (!status) {
      std::cout << "status-error\n" << std::flush;
      return 2;
    }
  }
  EvaluationRequest request;
  request.envelope.idempotency_key = KeyFor(options, "crash-commit");
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = Instant::FromValue(options.now_ms);
  request.context.tick = Tick::FromValue(options.tick);
  request.evidence = evidence.value();
  std::cout << "committing state-sequence=" << status.value().state_sequence.ToString() << "\n"
            << std::flush;
  auto outcome = coordinator.value()->Evaluate(request);
  if (!outcome) {
    std::cout << "evaluate-error code=" << ErrorCodeName(outcome.status().code()) << "\n"
              << std::flush;
    return 5;
  }
  std::cout << "completed state-sequence=" << outcome.value().state_sequence.ToString() << "\n"
            << std::flush;
  return 0;
}

int CommandHold(const Options& options) {
  auto policy = LoadPolicyFile(options.policy);
  if (!policy) {
    std::cout << "policy-error\n" << std::flush;
    return 2;
  }
  auto coordinator = OpenCoordinator(options, policy.value(), true);
  if (!coordinator) {
    std::cout << "open-error code=" << ErrorCodeName(coordinator.status().code()) << "\n"
              << std::flush;
    return 2;
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    std::cout << "status-error\n" << std::flush;
    return 2;
  }
  std::cout << "ready state-sequence=" << status.value().state_sequence.ToString()
            << " epoch=" << status.value().epoch.ToString() << "\n"
            << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream stream(line);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) {
      tokens.push_back(token);
    }
    if (tokens.empty()) {
      continue;
    }
    if (tokens[0] == "status") {
      auto current = coordinator.value()->GetStatus();
      if (!current) {
        std::cout << "status-error\n" << std::flush;
        continue;
      }
      std::cout << "status state-sequence=" << current.value().state_sequence.ToString()
                << " posture=" << PostureName(current.value().posture)
                << " epoch=" << current.value().epoch.ToString() << "\n"
                << std::flush;
      continue;
    }
    if (tokens[0] == "authorize") {
      Options request_options = options;
      request_options.now_ms = tokens.size() > 1 ? std::stoull(tokens[1]) : options.now_ms;
      request_options.ttl_ms = tokens.size() > 2 ? std::stoull(tokens[2]) : options.ttl_ms;
      request_options.has_key = false;
      const int result = CommandAuthorize(request_options);
      if (result != 0) {
        std::cout << "authorize-failed\n" << std::flush;
      }
      continue;
    }
    if (tokens[0] == "exit") {
      std::cout << "bye\n" << std::flush;
      return 0;
    }
    std::cout << "unknown-command\n" << std::flush;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto options = ParseOptions(argc, argv);
  if (!options) {
    std::cout << "usage-error " << options.status().ToString() << "\n" << std::flush;
    return 1;
  }
  const Options& parsed = options.value();
  if (parsed.command == "try-open") {
    return CommandTryOpen(parsed);
  }
  if (parsed.command == "reopen-status") {
    return CommandReopenStatus(parsed);
  }
  if (parsed.command == "commit") {
    return CommandCommit(parsed, true);
  }
  if (parsed.command == "evaluate-no-adopt") {
    return CommandCommit(parsed, false);
  }
  if (parsed.command == "authorize") {
    return CommandAuthorize(parsed);
  }
  if (parsed.command == "verify") {
    return CommandVerify(parsed);
  }
  if (parsed.command == "crash") {
    return CommandCrash(parsed);
  }
  if (parsed.command == "hold") {
    return CommandHold(parsed);
  }
  std::cout << "unknown-command " << parsed.command << "\n" << std::flush;
  return 1;
}
