// Degraded Operation Manager - operator command line.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every mutating invocation is a new process, so it explicitly adopts a new
// control epoch before it mutates anything. Nothing is inherited from the
// previous process by mere possession of the store directory.

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "tool_support.hpp"

namespace {

using dom::tools::Arguments;

void PrintUsage() {
  std::cout <<
      "Degraded Operation Manager " << dom::VersionString() << "\n"
      "usage: dom_cli <command> [options]\n"
      "\n"
      "commands:\n"
      "  version\n"
      "  validate-policy --file POLICY\n"
      "  status          --store DIR\n"
      "  history         --store DIR [--limit N]\n"
      "  evaluate        --store DIR --policy POLICY --evidence EVIDENCE [--now-ms N]\n"
      "                  [--tick N] [--key HEX] [--incarnation N] [--authority NAME]\n"
      "  authorize       --store DIR --policy POLICY --now-ms N --ttl-ms N [--key HEX]\n"
      "                  [--incarnation N] [--authority NAME]\n"
      "  acknowledge     --store DIR --policy POLICY --authorization N --owner NAME\n"
      "                  [--status accepted|partially-accepted|rejected] [--now-ms N]\n"
      "                  [--key HEX] [--incarnation N] [--authority NAME]\n"
      "  verify          --store DIR --policy POLICY --authorization N --owner NAME\n"
      "                  [--outcome effective|partially-effective|ineffective|unknown]\n"
      "                  [--effect-digest HEX] [--source-digest HEX] [--now-ms N]\n"
      "                  [--key HEX] [--incarnation N] [--authority NAME]\n"
      "  clear-latch     --store DIR --policy POLICY --evidence EVIDENCE --cause-digest HEX\n"
      "                  --authority NAME [--now-ms N] [--tick N] [--key HEX]\n"
      "                  [--incarnation N]\n"
      "  adopt-epoch     --store DIR --policy POLICY --incarnation N [--authority NAME]\n";
}

dom::Result<std::string> Require(const Arguments& arguments, const std::string& name) {
  const auto value = arguments.Get(name);
  if (!value.has_value()) {
    return dom::Result<std::string>::Err(dom::ErrorCode::InvalidArgument,
                                         "option --" + name + " is required");
  }
  return dom::Result<std::string>::Ok(value.value());
}

dom::Result<dom::Incarnation> RequireIncarnation(const Arguments& arguments) {
  const auto text = Require(arguments, "incarnation");
  if (!text) {
    return dom::Result<dom::Incarnation>::Err(text.status());
  }
  auto value = dom::tools::ParseUnsigned(text.value(), "incarnation");
  if (!value) {
    return dom::Result<dom::Incarnation>::Err(value.status());
  }
  return dom::Result<dom::Incarnation>::Ok(dom::Incarnation::FromValue(value.value()));
}

dom::Result<dom::Instant> InstantOrNow(const Arguments& arguments) {
  const auto text = arguments.Get("now-ms");
  if (!text.has_value()) {
    return dom::Result<dom::Instant>::Ok(dom::Instant::FromValue(dom::tools::NowMillis()));
  }
  auto value = dom::tools::ParseUnsigned(text.value(), "now-ms");
  if (!value) {
    return dom::Result<dom::Instant>::Err(value.status());
  }
  return dom::Result<dom::Instant>::Ok(dom::Instant::FromValue(value.value()));
}

dom::Result<dom::Tick> TickOrZero(const Arguments& arguments) {
  const auto text = arguments.Get("tick");
  if (!text.has_value()) {
    return dom::Result<dom::Tick>::Ok(dom::Tick());
  }
  auto value = dom::tools::ParseUnsigned(text.value(), "tick");
  if (!value) {
    return dom::Result<dom::Tick>::Err(value.status());
  }
  return dom::Result<dom::Tick>::Ok(dom::Tick::FromValue(value.value()));
}

/// Opens a coordinator and, when this process does not already own authority,
/// adopts a new control epoch explicitly.
dom::Result<std::unique_ptr<dom::Coordinator>> OpenAuthority(const std::string& directory,
                                                            const std::string& policy_path,
                                                            const std::string& authority,
                                                            dom::Incarnation incarnation) {
  auto policy = dom::tools::LoadPolicy(policy_path);
  if (!policy) {
    return dom::Result<std::unique_ptr<dom::Coordinator>>::Err(policy.status());
  }
  dom::CoordinatorOptions options;
  options.directory = directory;
  options.policy = policy.value();
  options.incarnation = incarnation;
  options.max_authorization_ttl_ms = dom::Duration::FromValue(24ull * 60ull * 60ull * 1000ull);
  auto coordinator = dom::Coordinator::Open(options);
  if (!coordinator) {
    return coordinator;
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    return dom::Result<std::unique_ptr<dom::Coordinator>>::Err(status.status());
  }
  if (!status.value().epoch_adopted) {
    dom::AdoptEpochRequest adopt;
    adopt.expected_epoch = status.value().epoch;
    adopt.new_incarnation = incarnation;
    adopt.authority_reference = authority;
    auto adopted = coordinator.value()->AdoptEpoch(adopt);
    if (!adopted) {
      return dom::Result<std::unique_ptr<dom::Coordinator>>::Err(adopted.status());
    }
    std::cout << "adopted epoch=" << adopted.value().epoch.ToString()
              << " incarnation=" << adopted.value().incarnation.ToString() << "\n";
  }
  return coordinator;
}

int CommandStatus(const Arguments& arguments) {
  auto directory = Require(arguments, "store");
  if (!directory) {
    return dom::tools::Fail(directory.status());
  }
  dom::StoreOptions options;
  options.directory = directory.value();
  options.create_if_missing = false;
  auto store = dom::StateStore::Open(options);
  if (!store) {
    return dom::tools::Fail(store.status());
  }
  const dom::PersistedState& state = store.value()->state();
  std::cout << "state-sequence " << state.sequence.ToString() << "\n";
  std::cout << "decision-sequence " << state.committed.decision_sequence.ToString() << "\n";
  std::cout << "epoch " << state.committed.epoch.ToString() << "\n";
  std::cout << "incarnation " << state.committed.incarnation.ToString() << "\n";
  std::cout << "mode " << state.committed.mode.ToString() << "\n";
  std::cout << "posture " << dom::PostureName(state.committed.posture) << "\n";
  std::cout << "since-ms " << state.committed.since_ms.ToString() << "\n";
  std::cout << "latched " << (state.committed.latch.latched ? "true" : "false") << "\n";
  std::cout << "latch-cause " << state.committed.latch.cause_digest.ToHex() << "\n";
  std::cout << "history-entries " << state.history.size() << "\n";
  std::cout << "authorizations " << state.authorizations.size() << "\n";
  std::cout << "acknowledgements " << state.acknowledgements.size() << "\n";
  std::cout << "verifications " << state.verifications.size() << "\n";
  std::cout << "recovery " << dom::RecoveryOutcomeName(store.value()->recovery().outcome) << "\n";
  std::cout << "recovery-detail " << store.value()->recovery().detail << "\n";
  return 0;
}

int CommandHistory(const Arguments& arguments) {
  auto directory = Require(arguments, "store");
  if (!directory) {
    return dom::tools::Fail(directory.status());
  }
  std::size_t limit = 20;
  if (const auto text = arguments.Get("limit")) {
    auto value = dom::tools::ParseUnsigned(text.value(), "limit");
    if (!value) {
      return dom::tools::Fail(value.status());
    }
    limit = static_cast<std::size_t>(value.value());
  }
  dom::StoreOptions options;
  options.directory = directory.value();
  options.create_if_missing = false;
  auto store = dom::StateStore::Open(options);
  if (!store) {
    return dom::tools::Fail(store.status());
  }
  const std::vector<dom::HistoryEntry>& history = store.value()->state().history;
  const std::size_t take = (std::min)(limit, history.size());
  std::cout << "history-entries " << history.size() << "\n";
  for (std::size_t i = history.size() - take; i < history.size(); ++i) {
    const dom::HistoryEntry& entry = history[i];
    std::cout << "history state=" << entry.state_sequence.ToString()
              << " decision=" << entry.decision_sequence.ToString()
              << " mode=" << entry.mode.ToString()
              << " posture=" << dom::PostureName(entry.posture)
              << " verdict=" << dom::VerdictName(entry.verdict)
              << " previous-mode=" << entry.previous_mode.ToString()
              << " previous-posture=" << dom::PostureName(entry.previous_posture)
              << " at=" << entry.decided_at_ms.ToString()
              << " decision-digest=" << entry.decision_digest.ToHex() << "\n";
  }
  return 0;
}

int CommandValidatePolicy(const Arguments& arguments) {
  auto file = Require(arguments, "file");
  if (!file) {
    return dom::tools::Fail(file.status());
  }
  auto policy = dom::tools::LoadPolicy(file.value());
  if (!policy) {
    return dom::tools::Fail(policy.status());
  }
  std::cout << "policy-valid id=" << policy.value().id.ToString()
            << " generation=" << policy.value().generation.ToString()
            << " revision=" << policy.value().revision_key
            << " modes=" << policy.value().modes.size()
            << " digest=" << dom::PolicyDigest(policy.value()).ToHex() << "\n";
  return 0;
}

int CommandEvaluate(const Arguments& arguments) {
  auto directory = Require(arguments, "store");
  auto policy_path = Require(arguments, "policy");
  auto evidence_path = Require(arguments, "evidence");
  auto incarnation = RequireIncarnation(arguments);
  if (!directory || !policy_path || !evidence_path || !incarnation) {
    return dom::tools::Fail("evaluate requires --store, --policy, --evidence and --incarnation");
  }
  auto now = InstantOrNow(arguments);
  auto tick = TickOrZero(arguments);
  if (!now || !tick) {
    return dom::tools::Fail("now-ms and tick must be decimal integers");
  }
  auto evidence = dom::tools::LoadEvidence(evidence_path.value());
  if (!evidence) {
    return dom::tools::Fail(evidence.status());
  }
  const std::string authority = arguments.GetOr("authority", "dom_cli");
  auto coordinator = OpenAuthority(directory.value(), policy_path.value(), authority,
                                   incarnation.value());
  if (!coordinator) {
    return dom::tools::Fail(coordinator.status());
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  dom::EvaluationRequest request;
  request.envelope.idempotency_key =
      dom::tools::LoadDigestOr(arguments.Get("key"),
                               dom::tools::DerivedKey("evaluate",
                                                      {evidence_path.value(),
                                                       now.value().ToString(),
                                                       tick.value().ToString(),
                                                       status.value().state_sequence.ToString()}))
          .value();
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = now.value();
  request.context.tick = tick.value();
  request.evidence = evidence.value();
  auto outcome = coordinator.value()->Evaluate(request);
  if (!outcome) {
    return dom::tools::Fail(outcome.status());
  }
  std::cout << "ok evaluate state-sequence=" << outcome.value().state_sequence.ToString()
            << " changed=" << (outcome.value().state_changed ? "true" : "false")
            << " replayed=" << (outcome.value().replayed ? "true" : "false") << "\n";
  std::cout << outcome.value().decision.ToText();
  std::cout << outcome.value().restrictions.ToText();
  return 0;
}

int CommandAuthorize(const Arguments& arguments) {
  auto directory = Require(arguments, "store");
  auto policy_path = Require(arguments, "policy");
  auto ttl = Require(arguments, "ttl-ms");
  auto incarnation = RequireIncarnation(arguments);
  if (!directory || !policy_path || !ttl || !incarnation) {
    return dom::tools::Fail("authorize requires --store, --policy, --ttl-ms and --incarnation");
  }
  auto now = InstantOrNow(arguments);
  auto ttl_value = dom::tools::ParseUnsigned(ttl.value(), "ttl-ms");
  if (!now || !ttl_value) {
    return dom::tools::Fail("now-ms and ttl-ms must be decimal integers");
  }
  const std::string authority = arguments.GetOr("authority", "dom_cli");
  auto coordinator = OpenAuthority(directory.value(), policy_path.value(), authority,
                                   incarnation.value());
  if (!coordinator) {
    return dom::tools::Fail(coordinator.status());
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  dom::AuthorizationRequest request;
  request.envelope.idempotency_key =
      dom::tools::LoadDigestOr(arguments.Get("key"),
                               dom::tools::DerivedKey("authorize",
                                                      {now.value().ToString(), ttl.value(),
                                                       status.value().state_sequence.ToString()}))
          .value();
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.now_ms = now.value();
  request.ttl_ms = dom::Duration::FromValue(ttl_value.value());
  auto outcome = coordinator.value()->Authorize(request);
  if (!outcome) {
    return dom::tools::Fail(outcome.status());
  }
  std::cout << "ok authorize state-sequence=" << outcome.value().state_sequence.ToString()
            << " replayed=" << (outcome.value().replayed ? "true" : "false") << "\n";
  std::cout << outcome.value().authorization.ToText();
  return 0;
}

int CommandAcknowledge(const Arguments& arguments) {
  auto directory = Require(arguments, "store");
  auto policy_path = Require(arguments, "policy");
  auto authorization = Require(arguments, "authorization");
  auto owner = Require(arguments, "owner");
  auto incarnation = RequireIncarnation(arguments);
  if (!directory || !policy_path || !authorization || !owner || !incarnation) {
    return dom::tools::Fail(
        "acknowledge requires --store, --policy, --authorization, --owner and --incarnation");
  }
  auto identifier = dom::tools::ParseUnsigned(authorization.value(), "authorization");
  auto now = InstantOrNow(arguments);
  if (!identifier || !now) {
    return dom::tools::Fail("authorization and now-ms must be decimal integers");
  }
  auto acceptance = dom::AcceptanceStatusFromName(arguments.GetOr("status", "accepted"));
  if (!acceptance) {
    return dom::tools::Fail(acceptance.status());
  }
  const std::string authority = arguments.GetOr("authority", "dom_cli");
  auto coordinator = OpenAuthority(directory.value(), policy_path.value(), authority,
                                   incarnation.value());
  if (!coordinator) {
    return dom::tools::Fail(coordinator.status());
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  dom::AcknowledgementRequest request;
  request.envelope.idempotency_key =
      dom::tools::LoadDigestOr(arguments.Get("key"),
                               dom::tools::DerivedKey("acknowledge",
                                                      {authorization.value(), owner.value(),
                                                       std::string(dom::AcceptanceStatusName(
                                                           acceptance.value())),
                                                       status.value().state_sequence.ToString()}))
          .value();
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.authorization = dom::AuthorizationId::FromValue(identifier.value());
  request.adjacent_owner = owner.value();
  request.status = acceptance.value();
  request.recorded_at_ms = now.value();
  auto outcome = coordinator.value()->RecordAcknowledgement(request);
  if (!outcome) {
    return dom::tools::Fail(outcome.status());
  }
  std::cout << "ok acknowledge state-sequence=" << outcome.value().state_sequence.ToString()
            << " replayed=" << (outcome.value().replayed ? "true" : "false") << "\n";
  std::cout << outcome.value().trace.ToText();
  return 0;
}

int CommandVerify(const Arguments& arguments) {
  auto directory = Require(arguments, "store");
  auto policy_path = Require(arguments, "policy");
  auto authorization = Require(arguments, "authorization");
  auto owner = Require(arguments, "owner");
  auto incarnation = RequireIncarnation(arguments);
  if (!directory || !policy_path || !authorization || !owner || !incarnation) {
    return dom::tools::Fail(
        "verify requires --store, --policy, --authorization, --owner and --incarnation");
  }
  auto identifier = dom::tools::ParseUnsigned(authorization.value(), "authorization");
  auto now = InstantOrNow(arguments);
  if (!identifier || !now) {
    return dom::tools::Fail("authorization and now-ms must be decimal integers");
  }
  auto outcome_value = dom::VerificationOutcomeFromName(arguments.GetOr("outcome", "unknown"));
  if (!outcome_value) {
    return dom::tools::Fail(outcome_value.status());
  }
  auto effect = dom::tools::LoadDigestOr(arguments.Get("effect-digest"), dom::Digest::Zero());
  auto source = dom::tools::LoadDigestOr(arguments.Get("source-digest"), dom::Digest::Zero());
  if (!effect || !source) {
    return dom::tools::Fail("effect-digest and source-digest must be 64 hex characters");
  }
  const std::string authority = arguments.GetOr("authority", "dom_cli");
  auto coordinator = OpenAuthority(directory.value(), policy_path.value(), authority,
                                   incarnation.value());
  if (!coordinator) {
    return dom::tools::Fail(coordinator.status());
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  dom::VerificationRequest request;
  request.envelope.idempotency_key =
      dom::tools::LoadDigestOr(arguments.Get("key"),
                               dom::tools::DerivedKey("verify",
                                                      {authorization.value(), owner.value(),
                                                       std::string(dom::VerificationOutcomeName(
                                                           outcome_value.value())),
                                                       effect.value().ToHex(),
                                                       status.value().state_sequence.ToString()}))
          .value();
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.authorization = dom::AuthorizationId::FromValue(identifier.value());
  request.adjacent_owner = owner.value();
  request.outcome = outcome_value.value();
  request.observed_at_ms = now.value();
  request.effect_digest = effect.value();
  request.source_digest = source.value();
  auto outcome = coordinator.value()->RecordEffectVerification(request);
  if (!outcome) {
    return dom::tools::Fail(outcome.status());
  }
  std::cout << "ok verify state-sequence=" << outcome.value().state_sequence.ToString()
            << " replayed=" << (outcome.value().replayed ? "true" : "false")
            << " stale-binding=" << (outcome.value().stale_binding ? "true" : "false") << "\n";
  std::cout << outcome.value().trace.ToText();
  return 0;
}

int CommandClearLatch(const Arguments& arguments) {
  auto directory = Require(arguments, "store");
  auto policy_path = Require(arguments, "policy");
  auto evidence_path = Require(arguments, "evidence");
  auto cause = Require(arguments, "cause-digest");
  auto authority = Require(arguments, "authority");
  auto incarnation = RequireIncarnation(arguments);
  if (!directory || !policy_path || !evidence_path || !cause || !authority || !incarnation) {
    return dom::tools::Fail(
        "clear-latch requires --store, --policy, --evidence, --cause-digest, --authority and "
        "--incarnation");
  }
  auto cause_digest = dom::Digest::FromHex(cause.value());
  if (!cause_digest) {
    return dom::tools::Fail("cause-digest must be 64 hex characters");
  }
  auto now = InstantOrNow(arguments);
  auto tick = TickOrZero(arguments);
  if (!now || !tick) {
    return dom::tools::Fail("now-ms and tick must be decimal integers");
  }
  auto evidence = dom::tools::LoadEvidence(evidence_path.value());
  if (!evidence) {
    return dom::tools::Fail(evidence.status());
  }
  auto coordinator = OpenAuthority(directory.value(), policy_path.value(), authority.value(),
                                   incarnation.value());
  if (!coordinator) {
    return dom::tools::Fail(coordinator.status());
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  dom::LatchClearRequest request;
  request.envelope.idempotency_key =
      dom::tools::LoadDigestOr(arguments.Get("key"),
                               dom::tools::DerivedKey("clear-latch",
                                                      {cause.value(), authority.value(),
                                                       now.value().ToString(),
                                                       status.value().state_sequence.ToString()}))
          .value();
  request.envelope.expected_state_sequence = status.value().state_sequence;
  request.envelope.expected_epoch = status.value().epoch;
  request.authority_reference = authority.value();
  request.latch_cause_digest = cause_digest.value();
  request.context.epoch = status.value().epoch;
  request.context.incarnation = status.value().incarnation;
  request.context.now_ms = now.value();
  request.context.tick = tick.value();
  request.evidence = evidence.value();
  auto outcome = coordinator.value()->ClearLatch(request);
  if (!outcome) {
    return dom::tools::Fail(outcome.status());
  }
  std::cout << "ok clear-latch state-sequence=" << outcome.value().state_sequence.ToString()
            << " cleared=" << (outcome.value().cleared ? "true" : "false") << "\n";
  std::cout << outcome.value().trace.ToText();
  return 0;
}

int CommandAdoptEpoch(const Arguments& arguments) {
  auto directory = Require(arguments, "store");
  auto policy_path = Require(arguments, "policy");
  auto incarnation = RequireIncarnation(arguments);
  if (!directory || !policy_path || !incarnation) {
    return dom::tools::Fail("adopt-epoch requires --store, --policy and --incarnation");
  }
  auto policy = dom::tools::LoadPolicy(policy_path.value());
  if (!policy) {
    return dom::tools::Fail(policy.status());
  }
  dom::CoordinatorOptions options;
  options.directory = directory.value();
  options.policy = policy.value();
  options.incarnation = incarnation.value();
  options.max_authorization_ttl_ms = dom::Duration::FromValue(24ull * 60ull * 60ull * 1000ull);
  auto coordinator = dom::Coordinator::Open(options);
  if (!coordinator) {
    return dom::tools::Fail(coordinator.status());
  }
  auto status = coordinator.value()->GetStatus();
  if (!status) {
    return dom::tools::Fail(status.status());
  }
  if (status.value().epoch_adopted) {
    std::cout << "epoch-already-owned epoch=" << status.value().epoch.ToString() << "\n";
    return 0;
  }
  dom::AdoptEpochRequest request;
  request.expected_epoch = status.value().epoch;
  request.new_incarnation = incarnation.value();
  request.authority_reference = arguments.GetOr("authority", "dom_cli");
  auto outcome = coordinator.value()->AdoptEpoch(request);
  if (!outcome) {
    return dom::tools::Fail(outcome.status());
  }
  std::cout << "ok adopt-epoch epoch=" << outcome.value().epoch.ToString()
            << " incarnation=" << outcome.value().incarnation.ToString()
            << " state-sequence=" << outcome.value().state_sequence.ToString() << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto arguments = dom::tools::ParseArguments(argc, argv);
  if (!arguments) {
    return dom::tools::Fail(arguments.status());
  }
  if (arguments.value().positional.empty()) {
    PrintUsage();
    return 1;
  }
  const std::string command = arguments.value().positional.front();
  if (command == "version") {
    std::cout << dom::kProductName << " " << dom::VersionString() << "\n";
    return 0;
  }
  if (command == "validate-policy") {
    return CommandValidatePolicy(arguments.value());
  }
  if (command == "status") {
    return CommandStatus(arguments.value());
  }
  if (command == "history") {
    return CommandHistory(arguments.value());
  }
  if (command == "evaluate") {
    return CommandEvaluate(arguments.value());
  }
  if (command == "authorize") {
    return CommandAuthorize(arguments.value());
  }
  if (command == "acknowledge") {
    return CommandAcknowledge(arguments.value());
  }
  if (command == "verify") {
    return CommandVerify(arguments.value());
  }
  if (command == "clear-latch") {
    return CommandClearLatch(arguments.value());
  }
  if (command == "adopt-epoch") {
    return CommandAdoptEpoch(arguments.value());
  }
  PrintUsage();
  return dom::tools::Fail("unknown command '" + command + "'");
}
