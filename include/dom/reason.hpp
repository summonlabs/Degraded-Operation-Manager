// Degraded Operation Manager - machine readable explanations.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dom/ids.hpp"

namespace dom {

enum class EvidenceClass : std::uint8_t;

/// Stable reason vocabulary. Codes are part of the machine readable contract:
/// they are encoded into durable state and must never be renumbered.
enum class ReasonCode : std::uint16_t {
  // Verdict level.
  Escalated = 1,
  Held = 2,
  Deescalated = 3,
  BecameIndeterminate = 4,
  Refused = 5,

  // Evidence state.
  EvidenceFresh = 10,
  EvidenceStale = 11,
  EvidenceMissing = 12,
  EvidenceConflicted = 13,
  EvidenceFutureDated = 14,
  EvidenceUnusable = 15,

  // Criteria.
  EntrySatisfied = 20,
  EntryUnsatisfied = 21,
  ExitSatisfied = 22,
  ExitUnsatisfied = 23,
  PredicateUnresolvable = 24,

  // Transition gating.
  MinDwellActive = 30,
  RecoveryHoldActive = 31,
  RecoveryHoldSatisfied = 32,
  LatchActive = 33,
  LatchCleared = 34,
  LadderStepBlocked = 35,
  RecoveryStepLimit = 36,

  // Authority and separation of duties.
  AuthorizationIssued = 40,
  AuthorizationRefused = 41,
  AuthorizationExpired = 42,
  AcknowledgementRecorded = 43,
  EffectVerified = 44,
  LatchClearRefused = 45,

  // Lifecycle, fencing and replay.
  EpochAdopted = 50,
  StaleEpochRefused = 51,
  DecisionFenced = 52,
  IdempotentReplay = 53,
  StateRecovered = 54,
  UnpublishedStateDiscarded = 55,
  InputRefused = 56,
  DuplicateRecorded = 57,
};

const char* ReasonCodeName(ReasonCode code) noexcept;

/// One explanation entry. Entries are canonicalised (sorted and de-duplicated)
/// so a trace digest never depends on evaluation order.
struct Reason {
  ReasonCode code = ReasonCode::Held;
  ModeId mode;
  EvidenceClass cls{};
  bool has_class = false;
  std::string subject;
  std::string metric;
  bool has_observed = false;
  std::int64_t observed = 0;
  std::string detail;
};

class ReasonTrace {
 public:
  void Add(Reason reason);
  void Add(ReasonCode code, std::string detail = {});

  bool empty() const noexcept { return reasons_.empty(); }
  std::size_t size() const noexcept { return reasons_.size(); }
  const std::vector<Reason>& reasons() const noexcept { return reasons_; }

  /// Canonical ordering: sort by (code, mode, class, subject, metric, observed,
  /// detail), then remove exact duplicates.
  void Canonicalize();

  /// Stable digest of the canonical trace.
  Digest digest() const;

  /// Deterministic text rendering, one reason per line.
  std::string ToText() const;

  static std::string RenderReason(const Reason& reason);

 private:
  std::vector<Reason> reasons_;
};

}  // namespace dom
