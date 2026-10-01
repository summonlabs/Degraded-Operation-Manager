// Degraded Operation Manager - committed mode decisions.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dom/ids.hpp"
#include "dom/policy.hpp"
#include "dom/reason.hpp"

namespace dom {

/// Outcome classification of one evaluation. Refusals are reported as errors or
/// as Hold with explicit refusal reasons; they never silently change state.
enum class Verdict : std::uint8_t {
  Hold = 0,
  Escalate = 1,
  Deescalate = 2,
  BecomeIndeterminate = 3,
};

const char* VerdictName(Verdict verdict) noexcept;
Result<Verdict> VerdictFromName(std::string_view name);

/// The exact input generations one decision was bound to.
struct GenerationVector {
  ControlEpoch epoch;
  Incarnation incarnation;
  PolicyGeneration policy;
  EvidenceRevision evidence;
  std::array<Generation, kEvidenceClassCount> classes{};

  Generation Of(EvidenceClass cls) const;
  void Set(EvidenceClass cls, Generation generation);
  Digest digest() const;

  friend bool operator==(const GenerationVector& a, const GenerationVector& b) = default;
};

/// Sticky-mode latch carried in authoritative state.
struct LatchState {
  bool latched = false;
  ModeId mode;
  Instant latched_at_ms;
  Digest cause_digest;

  friend bool operator==(const LatchState& a, const LatchState& b) = default;
};

/// The authoritative mode state. Everything the evaluation engine needs about
/// the committed past is here; the engine is a pure function of it.
struct CommittedView {
  StateSequence state_sequence;
  DecisionSequence decision_sequence;
  /// Control epoch and incarnation that own this state. A successor process
  /// must adopt a new epoch before it may mutate anything.
  ControlEpoch epoch;
  Incarnation incarnation;
  ModeId mode;
  Posture posture = Posture::Nominal;
  Instant since_ms;
  Tick since_tick;
  bool has_recovery_hold = false;
  Instant recovery_hold_start_ms;
  LatchState latch;
  Digest last_decision_digest;

  friend bool operator==(const CommittedView& a, const CommittedView& b) = default;
};

/// Bounded restriction outcome: which services are restricted, which remain
/// permitted. Permit and restriction sets are canonical and disjoint.
struct RestrictionSet {
  std::vector<RestrictionRule> rules;
  std::vector<std::string> restricted_services;
  std::vector<std::string> permitted_services;

  Digest digest() const;
  bool empty() const noexcept { return rules.empty() && restricted_services.empty(); }
  std::string ToText() const;
};

/// One committed evaluation result.
struct ModeDecision {
  /// Zero when the evaluation did not change authoritative state.
  DecisionSequence sequence;
  ModeId mode;
  Posture posture = Posture::Nominal;
  Verdict verdict = Verdict::Hold;
  Instant decided_at_ms;
  Tick tick;
  GenerationVector generations;
  Digest policy_digest;
  Digest evidence_digest;
  Digest input_digest;
  Digest previous_decision_digest;
  Digest restrictions_digest;
  ReasonTrace trace;

  bool IsCommitted() const noexcept { return !sequence.IsZero(); }
  Digest digest() const;
  /// Deterministic machine readable rendering.
  std::string ToText() const;
};

/// Durable transition record, oldest first, bounded by store capacity.
struct HistoryEntry {
  StateSequence state_sequence;
  DecisionSequence decision_sequence;
  ModeId mode;
  Posture posture = Posture::Nominal;
  Verdict verdict = Verdict::Hold;
  ModeId previous_mode;
  Posture previous_posture = Posture::Nominal;
  Instant decided_at_ms;
  Digest decision_digest;
  Digest trace_digest;

  friend bool operator==(const HistoryEntry& a, const HistoryEntry& b) = default;
};

}  // namespace dom
