// Degraded Operation Manager - shared synthetic facility fixtures.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The facility, its incidents and its measurements are synthetic inputs. The
// store, the locking, the publication and the processes are real.

#pragma once

#include <cstdint>
#include <string>

#include "dom/dom.hpp"
#include "testkit/process.hpp"

namespace dom::test {

struct PolicyOptions {
  std::uint64_t id = 51;
  std::uint64_t generation = 1;
  std::string revision_key = "facility-degraded-modes-2026-02";
  std::uint64_t default_max_age_ms = 30000;
  std::uint64_t requirement_max_age_ms = 60000;
  std::uint64_t min_dwell_ms = 1000;
  std::uint64_t recovery_hold_ms = 30000;
  std::uint64_t emergency_hold_ms = 60000;
  bool latch_emergency = true;
  /// Default allows a full ladder descent; narrower limits are exercised
  /// explicitly by the engine suite.
  std::uint8_t max_recovery_step = 6;
};

struct EvidenceOptions {
  std::uint64_t revision = 1;
  std::uint64_t observed_at_ms = 1000;
  std::int64_t severity = 0;
  std::int64_t redundancy = 1000;
  bool include_cooling = true;
  std::int64_t cooling_margin = 25000;
  bool cooling_conflict = false;
  std::string producer = "facility-observer";
  std::uint64_t generation = 0;
  std::uint64_t valid_until_ms = 0;
};

/// Canonical facility policy used across the suites: nominal, conserve,
/// restricted and latched emergency, with one inviolable and one protected
/// obligation.
PolicyDocument MakePolicy(const PolicyOptions& options = PolicyOptions());

/// Synthetic measurements for the standard policy.
EvidenceSnapshot MakeEvidence(const EvidenceOptions& options);

/// Committed view of a freshly initialised store: nominal mode at epoch 1.
CommittedView MakeCommittedView(const PolicyDocument& policy, std::uint64_t epoch,
                                std::uint64_t incarnation);

/// Coordinator options wired to a temporary store directory.
CoordinatorOptions MakeCoordinatorOptions(const std::string& directory,
                                          const PolicyDocument& policy,
                                          std::uint64_t incarnation = 1000);

/// Writes text to a file, creating it.
Status WriteTextFile(const std::string& path, const std::string& text);

}  // namespace dom::test
