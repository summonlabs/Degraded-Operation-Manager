// Degraded Operation Manager - decision rendering and generation vectors.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/decision.hpp"

#include <string>

namespace dom {

const char* VerdictName(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::Hold:
      return "hold";
    case Verdict::Escalate:
      return "escalate";
    case Verdict::Deescalate:
      return "deescalate";
    case Verdict::BecomeIndeterminate:
      return "become-indeterminate";
  }
  return "unknown";
}

Result<Verdict> VerdictFromName(std::string_view name) {
  for (std::uint8_t code = 0; code <= static_cast<std::uint8_t>(Verdict::BecomeIndeterminate);
       ++code) {
    const auto verdict = static_cast<Verdict>(code);
    if (name == VerdictName(verdict)) {
      return Result<Verdict>::Ok(verdict);
    }
  }
  return Result<Verdict>::Err(ErrorCode::InvalidArgument, "unknown verdict name");
}

Generation GenerationVector::Of(EvidenceClass cls) const {
  const auto index = static_cast<std::size_t>(cls);
  if (index == 0 || index > kEvidenceClassCount) {
    return Generation();
  }
  return classes[index - 1];
}

void GenerationVector::Set(EvidenceClass cls, Generation generation) {
  const auto index = static_cast<std::size_t>(cls);
  if (index == 0 || index > kEvidenceClassCount) {
    return;
  }
  classes[index - 1] = generation;
}

std::string ModeDecision::ToText() const {
  std::string text;
  text += "decision sequence=";
  text += sequence.ToString();
  text += " mode=";
  text += mode.ToString();
  text += " posture=";
  text += PostureName(posture);
  text += " verdict=";
  text += VerdictName(verdict);
  text += " at=";
  text += decided_at_ms.ToString();
  text += " tick=";
  text += tick.ToString();
  text += "\n";
  text += "generations epoch=";
  text += generations.epoch.ToString();
  text += " incarnation=";
  text += generations.incarnation.ToString();
  text += " policy=";
  text += generations.policy.ToString();
  text += " evidence=";
  text += generations.evidence.ToString();
  for (std::size_t i = 0; i < kEvidenceClassCount; ++i) {
    text += " class";
    text += std::to_string(i + 1);
    text += "=";
    text += generations.classes[i].ToString();
  }
  text += "\n";
  text += "policy-digest " + policy_digest.ToHex() + "\n";
  text += "evidence-digest " + evidence_digest.ToHex() + "\n";
  text += "input-digest " + input_digest.ToHex() + "\n";
  text += "previous-decision-digest " + previous_decision_digest.ToHex() + "\n";
  text += "restrictions-digest " + restrictions_digest.ToHex() + "\n";
  text += trace.ToText();
  return text;
}

std::string RestrictionSet::ToText() const {
  std::string text;
  for (const RestrictionRule& rule : rules) {
    text += "restriction id=";
    text += rule.id.ToString();
    text += " service=";
    text += rule.service_class;
    text += " kind=";
    text += RestrictionKindName(rule.kind);
    text += " allowance=";
    text += rule.allowance.ToString();
    text.push_back('\n');
  }
  text += "restricted";
  for (const std::string& service : restricted_services) {
    text += " ";
    text += service;
  }
  text.push_back('\n');
  text += "permitted";
  for (const std::string& service : permitted_services) {
    text += " ";
    text += service;
  }
  text.push_back('\n');
  return text;
}

}  // namespace dom
