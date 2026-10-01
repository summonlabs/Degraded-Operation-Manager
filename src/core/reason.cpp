// Degraded Operation Manager - machine readable explanations.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/reason.hpp"

#include <algorithm>
#include <string>

namespace dom {
namespace {

std::string Quote(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  out.push_back('"');
  return out;
}

bool ReasonLess(const Reason& a, const Reason& b) {
  if (a.code != b.code) {
    return static_cast<std::uint16_t>(a.code) < static_cast<std::uint16_t>(b.code);
  }
  if (a.mode != b.mode) {
    return a.mode < b.mode;
  }
  if (a.has_class != b.has_class) {
    return static_cast<int>(a.has_class) < static_cast<int>(b.has_class);
  }
  if (a.has_class && a.cls != b.cls) {
    return static_cast<int>(a.cls) < static_cast<int>(b.cls);
  }
  if (a.subject != b.subject) {
    return a.subject < b.subject;
  }
  if (a.metric != b.metric) {
    return a.metric < b.metric;
  }
  if (a.has_observed != b.has_observed) {
    return static_cast<int>(a.has_observed) < static_cast<int>(b.has_observed);
  }
  if (a.observed != b.observed) {
    return a.observed < b.observed;
  }
  return a.detail < b.detail;
}

bool ReasonEqual(const Reason& a, const Reason& b) { return !ReasonLess(a, b) && !ReasonLess(b, a); }

}  // namespace

const char* ReasonCodeName(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::Escalated:
      return "escalated";
    case ReasonCode::Held:
      return "held";
    case ReasonCode::Deescalated:
      return "deescalated";
    case ReasonCode::BecameIndeterminate:
      return "became-indeterminate";
    case ReasonCode::Refused:
      return "refused";
    case ReasonCode::EvidenceFresh:
      return "evidence-fresh";
    case ReasonCode::EvidenceStale:
      return "evidence-stale";
    case ReasonCode::EvidenceMissing:
      return "evidence-missing";
    case ReasonCode::EvidenceConflicted:
      return "evidence-conflicted";
    case ReasonCode::EvidenceFutureDated:
      return "evidence-future-dated";
    case ReasonCode::EvidenceUnusable:
      return "evidence-unusable";
    case ReasonCode::EntrySatisfied:
      return "entry-satisfied";
    case ReasonCode::EntryUnsatisfied:
      return "entry-unsatisfied";
    case ReasonCode::ExitSatisfied:
      return "exit-satisfied";
    case ReasonCode::ExitUnsatisfied:
      return "exit-unsatisfied";
    case ReasonCode::PredicateUnresolvable:
      return "predicate-unresolvable";
    case ReasonCode::MinDwellActive:
      return "min-dwell-active";
    case ReasonCode::RecoveryHoldActive:
      return "recovery-hold-active";
    case ReasonCode::RecoveryHoldSatisfied:
      return "recovery-hold-satisfied";
    case ReasonCode::LatchActive:
      return "latch-active";
    case ReasonCode::LatchCleared:
      return "latch-cleared";
    case ReasonCode::LadderStepBlocked:
      return "ladder-step-blocked";
    case ReasonCode::RecoveryStepLimit:
      return "recovery-step-limit";
    case ReasonCode::AuthorizationIssued:
      return "authorization-issued";
    case ReasonCode::AuthorizationRefused:
      return "authorization-refused";
    case ReasonCode::AuthorizationExpired:
      return "authorization-expired";
    case ReasonCode::AcknowledgementRecorded:
      return "acknowledgement-recorded";
    case ReasonCode::EffectVerified:
      return "effect-verified";
    case ReasonCode::LatchClearRefused:
      return "latch-clear-refused";
    case ReasonCode::EpochAdopted:
      return "epoch-adopted";
    case ReasonCode::StaleEpochRefused:
      return "stale-epoch-refused";
    case ReasonCode::DecisionFenced:
      return "decision-fenced";
    case ReasonCode::IdempotentReplay:
      return "idempotent-replay";
    case ReasonCode::StateRecovered:
      return "state-recovered";
    case ReasonCode::UnpublishedStateDiscarded:
      return "unpublished-state-discarded";
    case ReasonCode::InputRefused:
      return "input-refused";
    case ReasonCode::DuplicateRecorded:
      return "duplicate-recorded";
  }
  return "unknown";
}

void ReasonTrace::Add(Reason reason) { reasons_.push_back(std::move(reason)); }

void ReasonTrace::Add(ReasonCode code, std::string detail) {
  Reason reason;
  reason.code = code;
  reason.detail = std::move(detail);
  reasons_.push_back(std::move(reason));
}

void ReasonTrace::Canonicalize() {
  std::sort(reasons_.begin(), reasons_.end(), ReasonLess);
  reasons_.erase(std::unique(reasons_.begin(), reasons_.end(), ReasonEqual), reasons_.end());
}

std::string ReasonTrace::RenderReason(const Reason& reason) {
  std::string line = "reason ";
  line += ReasonCodeName(reason.code);
  if (!reason.mode.IsZero()) {
    line += " mode=";
    line += reason.mode.ToString();
  }
  if (reason.has_class) {
    line += " class=";
    line += std::to_string(static_cast<unsigned>(reason.cls));
  }
  if (!reason.subject.empty()) {
    line += " subject=";
    line += Quote(reason.subject);
  }
  if (!reason.metric.empty()) {
    line += " metric=";
    line += Quote(reason.metric);
  }
  if (reason.has_observed) {
    line += " observed=";
    line += std::to_string(reason.observed);
  }
  if (!reason.detail.empty()) {
    line += " detail=";
    line += Quote(reason.detail);
  }
  return line;
}

std::string ReasonTrace::ToText() const {
  std::string text;
  for (const Reason& reason : reasons_) {
    text += RenderReason(reason);
    text.push_back('\n');
  }
  return text;
}

}  // namespace dom
