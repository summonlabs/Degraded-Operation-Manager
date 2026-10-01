// Degraded Operation Manager - canonical degraded mode policy.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dom/evidence.hpp"
#include "dom/ids.hpp"
#include "dom/scalar.hpp"

namespace dom {

/// Ordered operating classes. Severity rank increases with the enumerator, so a
/// larger class is strictly more restrictive. Indeterminate is deliberately not
/// a member: it is a posture, not an operating class.
enum class OperatingClass : std::uint8_t {
  Nominal = 0,
  Watch = 1,
  Conserve = 2,
  Restricted = 3,
  Critical = 4,
  Emergency = 5,
};

inline constexpr std::size_t kOperatingClassCount = 6;

const char* OperatingClassName(OperatingClass cls) noexcept;
Result<OperatingClass> OperatingClassFromName(std::string_view name);
std::span<const OperatingClass> AllOperatingClasses();
/// Ordered severity rank; larger is more restrictive.
int SeverityRank(OperatingClass cls) noexcept;

/// Effective operating posture, which is an operating class or Indeterminate.
/// Indeterminate means the available evidence cannot justify any operating
/// class; it never relaxes the restrictions already in force.
enum class Posture : std::uint8_t {
  Nominal = 0,
  Watch = 1,
  Conserve = 2,
  Restricted = 3,
  Critical = 4,
  Emergency = 5,
  Indeterminate = 6,
};

const char* PostureName(Posture posture) noexcept;
Result<Posture> PostureFromName(std::string_view name);
Posture PostureOf(OperatingClass cls) noexcept;
bool IsIndeterminate(Posture posture) noexcept;
/// Rank used for transition ordering. Indeterminate carries the rank of the
/// mode it retains, which the caller supplies.
int EffectiveRank(Posture posture, OperatingClass retained) noexcept;

/// Where a declared freshness requirement is enforced.
enum class RequirementScope : std::uint8_t {
  Escalation = 1,
  Recovery = 2,
  Both = 3,
};

const char* RequirementScopeName(RequirementScope scope) noexcept;
Result<RequirementScope> RequirementScopeFromName(std::string_view name);
bool CoversEscalation(RequirementScope scope) noexcept;
bool CoversRecovery(RequirementScope scope) noexcept;

/// How long a mode sticks once entered.
enum class LatchMode : std::uint8_t {
  /// The mode is left as soon as its exit criteria and dwell allow.
  None = 0,
  /// The mode is only left by an explicit, authority-carrying clear request.
  UntilExplicitClear = 1,
  /// The mode may be left automatically once recovery preconditions hold.
  UntilRecoveryPermitted = 2,
};

const char* LatchModeName(LatchMode mode) noexcept;
Result<LatchMode> LatchModeFromName(std::string_view name);

/// Protection level of an obligation class.
enum class ProtectionLevel : std::uint8_t {
  /// No restriction may target this obligation's service classes.
  Inviolable = 0,
  /// Restrictions may target it only with an explicit protected minimum.
  Protected = 1,
  BestEffort = 2,
};

const char* ProtectionLevelName(ProtectionLevel level) noexcept;
Result<ProtectionLevel> ProtectionLevelFromName(std::string_view name);

enum class RestrictionKind : std::uint8_t {
  Deny = 0,
  Throttle = 1,
  Defer = 2,
  Degrade = 3,
};

const char* RestrictionKindName(RestrictionKind kind) noexcept;
Result<RestrictionKind> RestrictionKindFromName(std::string_view name);

/// Predicate aggregation over the records a selector matches.
enum class Aggregation : std::uint8_t {
  /// Satisfied when at least one matched record satisfies the comparison.
  Any = 0,
  /// Satisfied when every matched record satisfies the comparison.
  All = 1,
  /// Number of matched records that satisfy the comparison.
  Count = 2,
  Maximum = 3,
  Minimum = 4,
  Sum = 5,
};

const char* AggregationName(Aggregation aggregation) noexcept;
Result<Aggregation> AggregationFromName(std::string_view name);

enum class Comparator : std::uint8_t {
  AtLeast = 0,
  AtMost = 1,
  Equals = 2,
  NotEquals = 3,
};

const char* ComparatorName(Comparator comparator) noexcept;
Result<Comparator> ComparatorFromName(std::string_view name);

/// A decidable criterion over published evidence. subject and metric accept the
/// exact key or "*" for every key of the class.
struct Predicate {
  EvidenceClass cls = EvidenceClass::Incident;
  std::string subject = "*";
  std::string metric = "*";
  Aggregation aggregation = Aggregation::Any;
  Comparator comparator = Comparator::AtLeast;
  Scalar threshold;
  /// When true only evidence that is Fresh under policy may satisfy the
  /// predicate. Policy validation requires a matching declared requirement for
  /// every fresh-gated predicate.
  bool require_fresh = true;

  std::string ToString() const;
  friend bool operator==(const Predicate& a, const Predicate& b);
};

struct PredicateOutcome {
  bool satisfied = false;
  /// False when the predicate could not be decided (no usable records for an
  /// aggregate, mixed units, aggregate overflow). Unresolvable is never
  /// treated as satisfied.
  bool resolvable = false;
  std::size_t considered = 0;
  std::size_t matched = 0;
  Freshness worst_freshness = Freshness::Fresh;
  std::string detail;
};

PredicateOutcome EvaluatePredicate(const Predicate& predicate, const EvidenceView& view);

/// A freshness requirement the policy declares explicitly. Scope decides
/// whether it gates escalation, recovery, or both.
struct EvidenceRequirement {
  EvidenceClass cls = EvidenceClass::Incident;
  std::string subject = "*";
  std::string metric = "*";
  RequirementScope scope = RequirementScope::Both;
  Duration max_age_ms;
};

/// Obligation class protected by the policy, mapped to the service classes it
/// depends on.
struct ObligationClass {
  ObligationClassId id;
  std::string key;
  ProtectionLevel protection = ProtectionLevel::Protected;
  std::vector<std::string> service_classes;
};

struct RestrictionRule {
  RestrictionId id;
  std::string service_class;
  RestrictionKind kind = RestrictionKind::Deny;
  Scalar allowance;
};

struct ModeDefinition {
  ModeId id;
  std::string key;
  OperatingClass cls = OperatingClass::Nominal;
  RevisionId revision;
  LatchMode latch = LatchMode::None;
  Duration min_dwell_ms;
  Duration recovery_hold_ms;
  std::vector<Predicate> entry;
  std::vector<Predicate> exit;
  std::vector<RestrictionRule> restrictions;
  std::vector<ObligationClassId> protected_obligations;
};

/// The complete degraded operating mode policy. A policy generation is the
/// authority for exactly one immutable policy document; the digest of the
/// canonical encoding is the document identity.
struct PolicyDocument {
  PolicyId id;
  PolicyGeneration generation;
  std::string revision_key;
  std::vector<std::string> service_classes;
  std::vector<ObligationClass> obligations;
  std::vector<ModeDefinition> modes;
  std::vector<RestrictionRule> indeterminate_restrictions;
  std::vector<EvidenceRequirement> requirements;
  /// Maximum number of operating classes a single recovery evaluation may
  /// descend. Zero is not a legal value.
  std::uint8_t max_recovery_step = 1;
  Duration default_max_age_ms;

  /// Modes ordered by (severity rank, mode id); the canonical iteration order.
  std::vector<const ModeDefinition*> OrderedModes() const;
};

/// Digest of the canonical encoding of the policy.
Digest PolicyDigest(const PolicyDocument& policy);

/// Full structural validation. Validation is a precondition of evaluation: an
/// invalid policy is refused as a whole rather than partially applied.
Status ValidatePolicy(const PolicyDocument& policy);

Result<const ModeDefinition*> FindMode(const PolicyDocument& policy, ModeId id);
/// Mode of the given class with the smallest id, or nullptr.
const ModeDefinition* FindFirstModeOfClass(const PolicyDocument& policy, OperatingClass cls);
/// Requirement covering a selector, if any. Exact matches win over wildcards.
const EvidenceRequirement* FindCoveringRequirement(const PolicyDocument& policy,
                                                   const Predicate& predicate,
                                                   bool for_recovery);

}  // namespace dom
