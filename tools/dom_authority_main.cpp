// Degraded Operation Manager - authority holder process.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The holder opens the store, explicitly adopts a control epoch and then serves
// newline delimited commands on standard input until it is told to exit or the
// input ends. It exists so that a facility authority can be operated as one
// long lived process, and so that tests can prove real single-writer exclusion
// and real recovery after abrupt death.
//
// Commands:
//   status
//   evaluate --evidence FILE --now-ms N [--tick N]
//   authorize --ttl-ms N --now-ms N
//   exit

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "tool_support.hpp"

namespace {

struct Session {
  std::unique_ptr<dom::Coordinator> coordinator;
  std::string policy_path;
};

dom::Result<std::string> Require(const dom::tools::Arguments& arguments, const std::string& name) {
  const auto value = arguments.Get(name);
  if (!value.has_value()) {
    return dom::Result<std::string>::Err(dom::ErrorCode::InvalidArgument,
                                         "option --" + name + " is required");
  }
  return dom::Result<std::string>::Ok(value.value());
}

int ServeStatus(Session& session) {
  auto status = session.coordinator->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  std::cout << "state-sequence " << status.value().state_sequence.ToString()
            << " decision-sequence " << status.value().decision_sequence.ToString()
            << " epoch " << status.value().epoch.ToString()
            << " incarnation " << status.value().incarnation.ToString()
            << " mode " << status.value().mode.ToString()
            << " posture " << dom::PostureName(status.value().posture)
            << " latched " << (status.value().latched ? "true" : "false") << "\n";
  return 0;
}

int ServeEvaluate(Session& session, const dom::tools::Arguments& arguments) {
  auto evidence_path = Require(arguments, "evidence");
  if (!evidence_path) {
    return dom::tools::Fail(evidence_path.status());
  }
  auto evidence = dom::tools::LoadEvidence(evidence_path.value());
  if (!evidence) {
    return dom::tools::Fail(evidence.status());
  }
  auto status = session.coordinator->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  dom::Instant now = dom::Instant::FromValue(dom::tools::NowMillis());
  if (const auto text = arguments.Get("now-ms")) {
    auto value = dom::tools::ParseUnsigned(text.value(), "now-ms");
    if (!value) {
      return dom::tools::Fail(value.status());
    }
    now = dom::Instant::FromValue(value.value());
  }
  dom::Tick tick;
  if (const auto text = arguments.Get("tick")) {
    auto value = dom::tools::ParseUnsigned(text.value(), "tick");
    if (!value) {
      return dom::tools::Fail(value.status());
    }
    tick = dom::Tick::FromValue(value.value());
  }
  dom::EvaluationRequest request;
  request.envelope.idempotency_key =
      dom::tools::DerivedKey("authority-evaluate",
                             {evidence_path.value(), now.ToString(), tick.ToString(),
                              status.value().state_sequence.ToString()});
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = now;
  request.context.tick = tick;
  request.evidence = evidence.value();
  auto outcome = session.coordinator->Evaluate(request);
  if (!outcome) {
    return dom::tools::Fail(outcome.status());
  }
  std::cout << "evaluated state-sequence=" << outcome.value().state_sequence.ToString()
            << " changed=" << (outcome.value().state_changed ? "true" : "false")
            << " posture=" << dom::PostureName(outcome.value().decision.posture)
            << " verdict=" << dom::VerdictName(outcome.value().decision.verdict)
            << " mode=" << outcome.value().decision.mode.ToString() << "\n";
  return 0;
}

int ServeAuthorize(Session& session, const dom::tools::Arguments& arguments) {
  auto ttl = Require(arguments, "ttl-ms");
  if (!ttl) {
    return dom::tools::Fail(ttl.status());
  }
  auto ttl_value = dom::tools::ParseUnsigned(ttl.value(), "ttl-ms");
  if (!ttl_value) {
    return dom::tools::Fail(ttl_value.status());
  }
  auto status = session.coordinator->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  dom::Instant now = dom::Instant::FromValue(dom::tools::NowMillis());
  if (const auto text = arguments.Get("now-ms")) {
    auto value = dom::tools::ParseUnsigned(text.value(), "now-ms");
    if (!value) {
      return dom::tools::Fail(value.status());
    }
    now = dom::Instant::FromValue(value.value());
  }
  dom::AuthorizationRequest request;
  request.envelope.idempotency_key =
      dom::tools::DerivedKey("authority-authorize",
                             {now.ToString(), ttl.value(),
                              status.value().state_sequence.ToString()});
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.now_ms = now;
  request.ttl_ms = dom::Duration::FromValue(ttl_value.value());
  auto outcome = session.coordinator->Authorize(request);
  if (!outcome) {
    return dom::tools::Fail(outcome.status());
  }
  std::cout << "authorized id=" << outcome.value().authorization.id.ToString()
            << " bound-decision=" << outcome.value().authorization.bound_decision.ToString()
            << " expires=" << outcome.value().authorization.expires_at_ms.ToString()
            << " state-sequence=" << outcome.value().state_sequence.ToString() << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto arguments = dom::tools::ParseArguments(argc, argv);
  if (!arguments) {
    return dom::tools::Fail(arguments.status());
  }
  auto directory = Require(arguments.value(), "store");
  auto policy_path = Require(arguments.value(), "policy");
  auto incarnation_text = Require(arguments.value(), "incarnation");
  if (!directory || !policy_path || !incarnation_text) {
    return dom::tools::Fail(
        "dom_authority requires --store DIR --policy FILE --incarnation N [--authority NAME]");
  }
  auto incarnation_value = dom::tools::ParseUnsigned(incarnation_text.value(), "incarnation");
  if (!incarnation_value) {
    return dom::tools::Fail(incarnation_value.status());
  }
  auto policy = dom::tools::LoadPolicy(policy_path.value());
  if (!policy) {
    return dom::tools::Fail(policy.status());
  }
  dom::CoordinatorOptions options;
  options.directory = directory.value();
  options.policy = policy.value();
  options.incarnation = dom::Incarnation::FromValue(incarnation_value.value());
  options.max_authorization_ttl_ms = dom::Duration::FromValue(24ull * 60ull * 60ull * 1000ull);
  auto coordinator = dom::Coordinator::Open(options);
  if (!coordinator) {
    return dom::tools::Fail(coordinator.status());
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  if (!status.value().epoch_adopted) {
    dom::AdoptEpochRequest adopt;
    adopt.expected_epoch = status.value().epoch;
    adopt.new_incarnation = dom::Incarnation::FromValue(incarnation_value.value());
    adopt.authority_reference = arguments.value().GetOr("authority", "dom_authority");
    auto adopted = coordinator.value()->AdoptEpoch(adopt);
    if (!adopted) {
      return dom::tools::Fail(adopted.status());
    }
    status = coordinator.value()->GetStatus();
    if (!status) {
      return dom::tools::Fail(status.status());
    }
  }

  Session session;
  session.coordinator = std::move(coordinator.value());
  session.policy_path = policy_path.value();

  std::cout << "ready epoch=" << status.value().epoch.ToString()
            << " incarnation=" << status.value().incarnation.ToString()
            << " state-sequence=" << status.value().state_sequence.ToString() << "\n"
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
    if (tokens[0] == "exit") {
      std::cout << "bye\n" << std::flush;
      return 0;
    }
    std::vector<char*> raw;
    raw.reserve(tokens.size());
    for (std::string& item : tokens) {
      raw.push_back(item.data());
    }
    auto parsed = dom::tools::ParseArguments(static_cast<int>(raw.size()), raw.data());
    if (!parsed) {
      return dom::tools::Fail(parsed.status());
    }
    int result = 0;
    if (tokens[0] == "status") {
      result = ServeStatus(session);
    } else if (tokens[0] == "evaluate") {
      result = ServeEvaluate(session, parsed.value());
    } else if (tokens[0] == "authorize") {
      result = ServeAuthorize(session, parsed.value());
    } else {
      std::cout << "err unknown-command " << tokens[0] << "\n" << std::flush;
      continue;
    }
    if (result != 0) {
      std::cout << "err command-failed " << tokens[0] << "\n";
    }
    std::cout << std::flush;
  }
  return 0;
}
