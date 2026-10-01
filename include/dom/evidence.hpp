// Degraded Operation Manager - evidence intake and freshness.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dom/ids.hpp"
#include "dom/scalar.hpp"

namespace dom {

/// Evidence classes the facility posture is assembled from. Each class is
/// published by an adjacent owner; this runtime never observes the facility
/// itself and never infers a measurement from a missing one.
enum class EvidenceClass : std::uint8_t {
  Incident = 1,
  FailureDomain = 2,
  Capacity = 3,
  Redundancy = 4,
  Power = 5,
  Cooling = 6,
  Maintenance = 7,
  Obligation = 8,
};

inline constexpr std::size_t kEvidenceClassCount = 8;

std::span<const EvidenceClass> AllEvidenceClasses();
const char* EvidenceClassName(EvidenceClass cls) noexcept;
Result<EvidenceClass> EvidenceClassFromName(std::string_view name);

/// One published fact: a typed value for (class, subject, metric) with the
/// generation and provenance that produced it.
struct EvidenceRecord {
  EvidenceClass cls = EvidenceClass::Incident;
  std::string subject;
  std::string metric;
  Scalar value;
  Instant observed_at_ms;
  /// Absolute expiry. Zero means "no explicit expiry"; the policy maximum age
  /// still applies.
  Instant valid_until_ms;
  Generation generation;
  /// Digest of the producer's own snapshot, carried through unchanged.
  Digest source_digest;
  std::string producer;

  bool HasExplicitExpiry() const { return !valid_until_ms.IsZero(); }
  friend bool operator==(const EvidenceRecord& a, const EvidenceRecord& b);
};

struct EvidenceKey {
  EvidenceClass cls = EvidenceClass::Incident;
  std::string subject;
  std::string metric;

  std::string ToString() const;
  friend bool operator==(const EvidenceKey& a, const EvidenceKey& b);
  friend bool operator<(const EvidenceKey& a, const EvidenceKey& b);
};

/// Freshness classification of one (class, subject, metric) key at a given
/// instant. Only Fresh evidence may drive a less restrictive decision.
enum class Freshness : std::uint8_t {
  Fresh = 0,
  Stale = 1,
  Expired = 2,
  FutureDated = 3,
  Missing = 4,
  Conflicted = 5,
};

const char* FreshnessName(Freshness freshness) noexcept;
bool IsUsable(Freshness freshness) noexcept;

/// Evaluated state of one key: why it is or is not usable, and which record
/// (highest generation, then newest observation, then producer) is the one the
/// posture is taken from.
struct KeyPosture {
  EvidenceKey key;
  Freshness freshness = Freshness::Missing;
  std::size_t record_count = 0;
  bool has_authoritative = false;
  Generation generation;
  Instant observed_at_ms;
  Scalar value;
  std::string producer;
  std::string detail;
};

/// Immutable, canonically ordered evidence snapshot. Construction validates,
/// sorts, and classifies structure only; freshness is a function of time and
/// policy and is therefore computed by EvidenceView.
class EvidenceSnapshot {
 public:
  EvidenceSnapshot() = default;

  static Result<EvidenceSnapshot> Build(std::vector<EvidenceRecord> records,
                                        EvidenceRevision revision);

  const std::vector<EvidenceRecord>& records() const noexcept { return records_; }
  const std::vector<EvidenceKey>& keys() const noexcept { return keys_; }
  /// Keys whose records disagree; such a key is never usable.
  const std::vector<EvidenceKey>& conflicted_keys() const noexcept { return conflicted_keys_; }

  EvidenceRevision revision() const noexcept { return revision_; }
  const Digest& digest() const noexcept { return digest_; }

  Generation ClassGeneration(EvidenceClass cls) const;
  std::size_t ClassRecordCount(EvidenceClass cls) const;

 private:
  std::vector<EvidenceRecord> records_;
  std::vector<EvidenceKey> keys_;
  std::vector<EvidenceKey> conflicted_keys_;
  EvidenceRevision revision_;
  Digest digest_;
};

/// Policy supplied freshness limit for one selector. The most specific rule
/// that matches a key wins; ties are resolved by the smallest maximum age, so
/// resolution never depends on declaration order.
struct AgeRule {
  EvidenceClass cls = EvidenceClass::Incident;
  std::string subject = "*";
  std::string metric = "*";
  Duration max_age_ms;
};

/// Result of applying a selector (possibly with wildcards) to the posture of a
/// whole evidence class.
struct SelectionPosture {
  Freshness freshness = Freshness::Missing;
  std::size_t keys = 0;
  std::size_t fresh_keys = 0;
  std::string detail;
};

/// Freshness and selection view over a snapshot at one instant. The view holds
/// no reference to mutable state and is safe to copy.
class EvidenceView {
 public:
  EvidenceView() = default;
  EvidenceView(const EvidenceSnapshot& snapshot, Instant now_ms, Duration default_max_age_ms,
               const std::vector<AgeRule>& ages);

  Instant now_ms() const noexcept { return now_ms_; }
  const std::vector<KeyPosture>& postures() const noexcept { return postures_; }

  const KeyPosture* Find(EvidenceClass cls, std::string_view subject,
                         std::string_view metric) const;
  Freshness FreshnessOf(EvidenceClass cls, std::string_view subject,
                        std::string_view metric) const;
  /// Worst freshness over the keys of one class. Informational: gating uses
  /// declared requirements, never a class-wide aggregate.
  Freshness ClassFreshness(EvidenceClass cls) const;
  std::size_t FreshKeyCount(EvidenceClass cls) const;

  /// Records of a class whose subject and metric match an exact key or "*".
  /// Only keys that are usable under require_fresh are returned.
  std::vector<const KeyPosture*> Select(EvidenceClass cls, std::string_view subject,
                                        std::string_view metric,
                                        bool require_fresh) const;

  /// Aggregate posture of every key matching a selector. A selector with no
  /// matching key is Missing, which can never satisfy a freshness requirement.
  SelectionPosture PostureFor(EvidenceClass cls, std::string_view subject,
                              std::string_view metric) const;

  Duration MaxAgeOf(EvidenceClass cls, std::string_view subject,
                    std::string_view metric) const;

 private:
  struct ClassRange {
    std::size_t begin = 0;
    std::size_t end = 0;
  };

  const EvidenceSnapshot* snapshot_ = nullptr;
  Instant now_ms_;
  Duration default_max_age_ms_;
  std::vector<AgeRule> ages_;
  std::vector<KeyPosture> postures_;
  /// Postures are in canonical order, so each evidence class owns one
  /// contiguous range and selection never scans unrelated classes.
  std::array<ClassRange, kEvidenceClassCount + 1> ranges_{};
};

}  // namespace dom
