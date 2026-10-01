// Degraded Operation Manager - deterministic mode evaluation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

#include "dom/decision.hpp"
#include "dom/evidence.hpp"
#include "dom/policy.hpp"

namespace dom {

/// Explicit evaluation context. Wall clock, tick and control epoch are inputs;
/// the engine never reads a clock or a global.
struct EvaluationContext {
  ControlEpoch epoch;
  Incarnation incarnation;
  Instant now_ms;
  Tick tick;
};

struct EvaluationInputs {
  const PolicyDocument* policy = nullptr;
  const EvidenceSnapshot* evidence = nullptr;
  CommittedView committed;
  EvaluationContext context;
  /// The caller asserts that ValidatePolicy already passed for exactly this
  /// policy document. When false the engine validates before evaluating.
  bool policy_validated = false;
  /// The caller has already verified that an explicit latch clear is
  /// authorized and presents the matching cause digest. The engine then reports
  /// whether the recovery preconditions hold; it never de-escalates in the same
  /// step as a latch clear.
  bool latch_clear_requested = false;
};

struct EvaluationOutcome {
  ModeDecision decision;
  CommittedView next;
  RestrictionSet restrictions;
  /// True when next differs from the committed view (a publishable change).
  bool changed = false;
  /// True when the evidence and dwell preconditions required to leave the
  /// current mode hold. A latch clear requires this.
  bool recovery_preconditions_ok = false;
  /// True when the latch is clear in next.
  bool latch_cleared = false;
  /// True when next is the indeterminate posture.
  bool indeterminate = false;
};

/// Pure transition function: (policy, evidence, committed state, context) maps
/// to (decision, next committed state, restriction set). No I/O, no clock, no
/// shared mutable state, so the same inputs always produce the same digests.
///
/// Refusals (stale epoch, unknown mode, policy/evidence incompatible) are
/// returned as errors and never mutate state.
Result<EvaluationOutcome> EvaluateMode(const EvaluationInputs& inputs);

/// Restriction set in force for a mode, with protected obligations applied.
RestrictionSet RestrictionsForMode(const PolicyDocument& policy,
                                   const ModeDefinition& mode);

/// Union of two restriction sets: never weaker than either input.
RestrictionSet UnionRestrictions(const PolicyDocument& policy, const RestrictionSet& a,
                                 const RestrictionSet& b);

}  // namespace dom
