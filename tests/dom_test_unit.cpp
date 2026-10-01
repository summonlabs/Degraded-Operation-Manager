// Degraded Operation Manager - domain unit tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

namespace {

using namespace dom;

std::string Repeat(const std::string& unit, int times) {
  std::string text;
  text.reserve(unit.size() * static_cast<std::size_t>(times));
  for (int i = 0; i < times; ++i) {
    text += unit;
  }
  return text;
}

DOM_TEST(sha256_known_vectors) {
  DOM_CHECK_EQ(Sha256::Of(std::string_view("")).ToHex(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  DOM_CHECK_EQ(Sha256::Of(std::string_view("abc")).ToHex(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  DOM_CHECK_EQ(
      Sha256::Of(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
          .ToHex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // Incremental updates must agree with a single shot over the same bytes.
  const std::string material = Repeat("abcdefghij", 1000);
  Sha256 hasher;
  for (int i = 0; i < 1000; ++i) {
    hasher.Update(std::string_view("abcdefghij"));
  }
  DOM_CHECK_EQ(hasher.Finish().ToHex(), Sha256::Of(material).ToHex());

  auto digest = Digest::FromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  DOM_CHECK_OK(digest);
  DOM_CHECK_EQ(digest.value().ToHex(),
               std::string("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"));
  DOM_CHECK_ERR(Digest::FromHex("zz"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(Digest::FromHex(std::string(63, 'a')), ErrorCode::InvalidArgument);
  DOM_CHECK(Digest::Zero().IsZero());
  DOM_CHECK(!digest.value().IsZero());
}

DOM_TEST(checked_arithmetic_refuses_wrap) {
  DOM_CHECK_ERR(CheckedAdd(0xFFFFFFFFFFFFFFFFull, 1ull), ErrorCode::Overflow);
  DOM_CHECK_ERR(CheckedSub(0ull, 1ull), ErrorCode::Underflow);
  DOM_CHECK_ERR(CheckedMul(0xFFFFFFFFFFFFFFFFull, 2ull), ErrorCode::Overflow);
  DOM_CHECK_OK(CheckedAdd(1ull, 2ull));
  DOM_CHECK_EQ(CheckedAdd(1ull, 2ull).value(), 3ull);
  DOM_CHECK_ERR(CheckedAddSigned(9223372036854775807ll, 1ll), ErrorCode::Overflow);
  DOM_CHECK_ERR(CheckedSubSigned(-9223372036854775807ll - 1ll, 1ll), ErrorCode::Underflow);
  DOM_CHECK_ERR(NarrowU32(0x100000000ull), ErrorCode::Overflow);
  DOM_CHECK_ERR(NarrowU8(256ull), ErrorCode::Overflow);
  DOM_CHECK_EQ(NarrowU8(255ull).value(), 255u);

  const auto maximum = StateSequence::FromValue(StateSequence::kMax);
  DOM_CHECK_ERR(maximum.Next(), ErrorCode::Overflow);
  DOM_CHECK_EQ(StateSequence::FromValue(1).Next().value().value(), 2ull);
  DOM_CHECK_EQ(StateSequence::FromValue(5).Add(5).value().value(), 10ull);
  DOM_CHECK_ERR(StateSequence::FromValue(StateSequence::kMax).Add(1), ErrorCode::Overflow);

  DOM_CHECK_ERR(Elapsed(Instant::FromValue(10), Instant::FromValue(5)), ErrorCode::Underflow);
  DOM_CHECK_EQ(Elapsed(Instant::FromValue(5), Instant::FromValue(10)).value().value(), 5ull);
  DOM_CHECK_EQ(Advance(Instant::FromValue(5), Duration::FromValue(7)).value().value(), 12ull);
  DOM_CHECK_ERR(Advance(Instant::FromValue(Instant::kMax), Duration::FromValue(1)),
                ErrorCode::Overflow);
}

DOM_TEST(scalar_units_are_never_coerced) {
  DOM_CHECK_ERR(CompareScalar(Scalar(Unit::Count, 1), Scalar(Unit::MilliPercent, 1)),
                ErrorCode::InvalidArgument);
  DOM_CHECK_EQ(CompareScalar(Scalar(Unit::Count, 1), Scalar(Unit::Count, 2)).value(), -1);
  DOM_CHECK_EQ(CompareScalar(Scalar(Unit::Count, 2), Scalar(Unit::Count, 2)).value(), 0);
  DOM_CHECK_OK(AddScalar(Scalar(Unit::Count, 2), Scalar(Unit::Count, 3)));
  DOM_CHECK_ERR(AddScalar(Scalar(Unit::Count, 2), Scalar(Unit::Watts, 3)),
                ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(AddScalar(Scalar(Unit::Count, 9223372036854775807ll), Scalar(Unit::Count, 1)),
                ErrorCode::Overflow);
  DOM_CHECK_EQ(Scalar(Unit::MilliCelsius, -5).ToString(), std::string("milli-celsius:-5"));
  DOM_CHECK_OK(UnitFromName("milli-ratio"));
  DOM_CHECK_ERR(UnitFromName("furlongs"), ErrorCode::InvalidArgument);
}

DOM_TEST(evidence_snapshot_is_canonical_and_detects_conflicts) {
  auto one = dom::test::MakeEvidence(dom::test::EvidenceOptions{});
  DOM_CHECK_EQ(one.records().size(), 3u);
  DOM_CHECK(one.digest() != Digest::Zero());

  // The same records in a different order produce the same snapshot digest.
  std::vector<EvidenceRecord> shuffled(one.records().rbegin(), one.records().rend());
  auto rebuilt = EvidenceSnapshot::Build(shuffled, EvidenceRevision::FromValue(1));
  DOM_CHECK_OK(rebuilt);
  DOM_CHECK_EQ(rebuilt.value().digest(), one.digest());

  // Exact duplicates collapse; disagreeing publications for one key conflict.
  std::vector<EvidenceRecord> duplicated = one.records();
  duplicated.push_back(one.records().front());
  auto deduped = EvidenceSnapshot::Build(duplicated, EvidenceRevision::FromValue(1));
  DOM_CHECK_OK(deduped);
  DOM_CHECK_EQ(deduped.value().records().size(), 3u);
  DOM_CHECK_EQ(deduped.value().conflicted_keys().size(), 0u);

  dom::test::EvidenceOptions conflicting_options;
  conflicting_options.cooling_conflict = true;
  auto conflicting = dom::test::MakeEvidence(conflicting_options);
  DOM_CHECK_EQ(conflicting.conflicted_keys().size(), 1u);

  std::vector<EvidenceRecord> missing_subject = one.records();
  missing_subject.front().subject.clear();
  DOM_CHECK_ERR(EvidenceSnapshot::Build(missing_subject, EvidenceRevision::FromValue(1)),
                ErrorCode::InvalidArgument);

  std::vector<EvidenceRecord> missing_producer = one.records();
  missing_producer.front().producer.clear();
  DOM_CHECK_ERR(EvidenceSnapshot::Build(missing_producer, EvidenceRevision::FromValue(1)),
                ErrorCode::InvalidArgument);
}

DOM_TEST(freshness_is_computed_against_policy_age) {
  auto snapshot = dom::test::MakeEvidence(dom::test::EvidenceOptions{});
  std::vector<AgeRule> ages;
  AgeRule rule;
  rule.cls = EvidenceClass::Incident;
  rule.subject = "*";
  rule.metric = "severity";
  rule.max_age_ms = Duration::FromValue(60000);
  ages.push_back(rule);

  EvidenceView fresh(snapshot, Instant::FromValue(1000), Duration::FromValue(30000), ages);
  DOM_CHECK_EQ(fresh.FreshnessOf(EvidenceClass::Incident, "facility", "severity"),
               Freshness::Fresh);
  DOM_CHECK_EQ(fresh.FreshnessOf(EvidenceClass::Cooling, "zone-a", "margin"), Freshness::Fresh);
  DOM_CHECK_EQ(fresh.FreshnessOf(EvidenceClass::Power, "nothing", "nothing"), Freshness::Missing);

  EvidenceView stale(snapshot, Instant::FromValue(200000), Duration::FromValue(30000), ages);
  DOM_CHECK_EQ(stale.FreshnessOf(EvidenceClass::Incident, "facility", "severity"),
               Freshness::Stale);
  DOM_CHECK_EQ(stale.FreshnessOf(EvidenceClass::Redundancy, "power", "ratio"), Freshness::Stale);

  EvidenceView future(snapshot, Instant::FromValue(0), Duration::FromValue(30000), ages);
  DOM_CHECK_EQ(future.FreshnessOf(EvidenceClass::Incident, "facility", "severity"),
               Freshness::FutureDated);

  dom::test::EvidenceOptions expiring;
  expiring.valid_until_ms = 1500;
  auto expiring_snapshot = dom::test::MakeEvidence(expiring);
  EvidenceView expired(expiring_snapshot, Instant::FromValue(2000), Duration::FromValue(30000), {});
  DOM_CHECK_EQ(expired.FreshnessOf(EvidenceClass::Incident, "facility", "severity"),
               Freshness::Expired);

  dom::test::EvidenceOptions conflicting_options;
  conflicting_options.cooling_conflict = true;
  auto conflicting = dom::test::MakeEvidence(conflicting_options);
  EvidenceView conflicted(conflicting, Instant::FromValue(1000), Duration::FromValue(30000), {});
  DOM_CHECK_EQ(conflicted.FreshnessOf(EvidenceClass::Cooling, "zone-a", "margin"),
               Freshness::Conflicted);
  const auto posture = conflicted.PostureFor(EvidenceClass::Cooling, "zone-a", "margin");
  DOM_CHECK_EQ(posture.freshness, Freshness::Conflicted);
  DOM_CHECK_EQ(posture.keys, 1u);
  DOM_CHECK_EQ(posture.fresh_keys, 0u);
}

DOM_TEST(predicates_are_decidable_and_conservative) {
  auto healthy = dom::test::MakeEvidence(dom::test::EvidenceOptions{});
  EvidenceView view(healthy, Instant::FromValue(1000), Duration::FromValue(30000), {});

  Predicate maximum;
  maximum.cls = EvidenceClass::Incident;
  maximum.subject = "*";
  maximum.metric = "severity";
  maximum.aggregation = Aggregation::Maximum;
  maximum.comparator = Comparator::AtMost;
  maximum.threshold = Scalar::Code(0);
  auto outcome = EvaluatePredicate(maximum, view);
  DOM_CHECK(outcome.resolvable);
  DOM_CHECK(outcome.satisfied);
  DOM_CHECK_EQ(outcome.matched, 1u);

  Predicate counted;
  counted.cls = EvidenceClass::Incident;
  counted.subject = "*";
  counted.metric = "severity";
  counted.aggregation = Aggregation::Count;
  counted.comparator = Comparator::AtLeast;
  counted.threshold = Scalar::Count(2);
  outcome = EvaluatePredicate(counted, view);
  DOM_CHECK(outcome.resolvable);
  DOM_CHECK(!outcome.satisfied);

  // Aggregates over nothing are unresolvable, never satisfied.
  Predicate absent;
  absent.cls = EvidenceClass::Power;
  absent.subject = "*";
  absent.metric = "headroom";
  absent.aggregation = Aggregation::Maximum;
  absent.comparator = Comparator::AtLeast;
  absent.threshold = Scalar(Unit::Watts, 0);
  outcome = EvaluatePredicate(absent, view);
  DOM_CHECK(!outcome.resolvable);
  DOM_CHECK(!outcome.satisfied);

  // A unit mismatch is unresolvable rather than coerced.
  Predicate mismatched;
  mismatched.cls = EvidenceClass::Incident;
  mismatched.subject = "*";
  mismatched.metric = "severity";
  mismatched.aggregation = Aggregation::Maximum;
  mismatched.comparator = Comparator::AtLeast;
  mismatched.threshold = Scalar(Unit::Watts, 1);
  outcome = EvaluatePredicate(mismatched, view);
  DOM_CHECK(!outcome.resolvable);
  DOM_CHECK(!outcome.satisfied);

  // An "all" predicate needs at least one record.
  Predicate all;
  all.cls = EvidenceClass::Cooling;
  all.subject = "zone-a";
  all.metric = "margin";
  all.aggregation = Aggregation::All;
  all.comparator = Comparator::AtLeast;
  all.threshold = Scalar(Unit::MilliCelsius, 1000);
  outcome = EvaluatePredicate(all, view);
  DOM_CHECK(outcome.resolvable);
  DOM_CHECK(outcome.satisfied);
}

DOM_TEST(policy_validation_accepts_the_fixture_and_rejects_defects) {
  const PolicyDocument policy = dom::test::MakePolicy();
  DOM_CHECK_OK(ValidatePolicy(policy));
  DOM_CHECK_OK(FindMode(policy, ModeId::FromValue(9)));
  DOM_CHECK_ERR(FindMode(policy, ModeId::FromValue(4242)), ErrorCode::NotFound);
  DOM_CHECK(FindFirstModeOfClass(policy, OperatingClass::Nominal) != nullptr);

  {
    PolicyDocument broken = policy;
    broken.modes[1].id = broken.modes[0].id;
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    PolicyDocument broken = policy;
    broken.modes[1].entry.clear();
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    PolicyDocument broken = policy;
    broken.service_classes.clear();
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    PolicyDocument broken = policy;
    broken.modes[2].restrictions[0].service_class = "unknown-service";
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    // Denying a service an inviolable obligation depends on is refused.
    PolicyDocument broken = policy;
    broken.modes[3].restrictions[0].service_class = "life-safety";
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    // A fresh-gated criterion without a declared requirement is refused.
    PolicyDocument broken = policy;
    broken.requirements.clear();
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    // An exit threshold weaker than the entry threshold is refused.
    PolicyDocument broken = policy;
    broken.modes[1].exit[0].threshold = Scalar::Code(4);
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    PolicyDocument broken = policy;
    broken.max_recovery_step = 0;
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    PolicyDocument broken = policy;
    broken.default_max_age_ms = Duration();
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    // A count aggregation must carry a count threshold.
    PolicyDocument broken = policy;
    broken.modes[1].entry[0].aggregation = Aggregation::Count;
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    PolicyDocument broken = policy;
    broken.obligations[0].service_classes = {"not-a-service"};
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    PolicyDocument broken = policy;
    broken.modes[0].protected_obligations.push_back(ObligationClassId::FromValue(77));
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    PolicyDocument broken = policy;
    broken.requirements[0].max_age_ms = Duration();
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
  {
    PolicyDocument broken = policy;
    broken.modes[1].restrictions[0].allowance = Scalar(Unit::Milliseconds, 5);
    DOM_CHECK_ERR(ValidatePolicy(broken), ErrorCode::PolicyRejected);
  }
}

DOM_TEST(restriction_sets_protect_obligations) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const ModeDefinition* restricted = FindMode(policy, ModeId::FromValue(6)).value();
  const RestrictionSet set = RestrictionsForMode(policy, *restricted);
  DOM_CHECK_EQ(set.rules.size(), 2u);
  DOM_CHECK_EQ(set.restricted_services.size(), 2u);
  for (const std::string& service : set.restricted_services) {
    DOM_CHECK(std::find(set.permitted_services.begin(), set.permitted_services.end(), service) ==
              set.permitted_services.end());
  }
  DOM_CHECK(std::find(set.permitted_services.begin(), set.permitted_services.end(), "storage") !=
            set.permitted_services.end());

  // A deny rule aimed at a protected obligation is dropped at runtime.
  PolicyDocument permissive = policy;
  ModeDefinition emergency = *FindMode(permissive, ModeId::FromValue(9)).value();
  RestrictionRule deny_storage;
  deny_storage.id = RestrictionId::FromValue(99);
  deny_storage.service_class = "storage";
  deny_storage.kind = RestrictionKind::Deny;
  deny_storage.allowance = Scalar();
  emergency.restrictions.push_back(deny_storage);
  const RestrictionSet guarded = RestrictionsForMode(permissive, emergency);
  for (const RestrictionRule& rule : guarded.rules) {
    DOM_CHECK(!(rule.id == RestrictionId::FromValue(99)));
    // No rule at all may deny a service a protected obligation depends on.
    DOM_CHECK(!(rule.kind == RestrictionKind::Deny && rule.service_class == "storage"));
  }

  // Union never loses a restriction.
  const ModeDefinition* conserve = FindMode(policy, ModeId::FromValue(5)).value();
  const RestrictionSet conservative = RestrictionsForMode(policy, *conserve);
  const RestrictionSet merged = UnionRestrictions(policy, conservative, guarded);
  DOM_CHECK(merged.restricted_services.size() >= guarded.restricted_services.size());
  DOM_CHECK(merged.rules.size() >= conservative.rules.size());
}

DOM_TEST(reason_traces_are_canonical_and_order_independent) {
  ReasonTrace first;
  Reason a;
  a.code = ReasonCode::Escalated;
  a.mode = ModeId::FromValue(9);
  a.has_class = true;
  a.cls = EvidenceClass::Cooling;
  a.subject = "zone-a";
  a.metric = "margin";
  a.detail = "hot";
  Reason b;
  b.code = ReasonCode::EvidenceMissing;
  b.metric = "ratio";
  first.Add(a);
  first.Add(b);
  first.Add(b);
  first.Canonicalize();
  DOM_CHECK_EQ(first.size(), 2u);

  ReasonTrace second;
  second.Add(b);
  second.Add(a);
  second.Canonicalize();
  DOM_CHECK_EQ(first.digest(), second.digest());
  const std::string text = first.ToText();
  DOM_CHECK(text.find("reason escalated") != std::string::npos);
  DOM_CHECK(text.find("reason evidence-missing") != std::string::npos);

  Reason quoted;
  quoted.code = ReasonCode::Held;
  quoted.detail = "line \"one\"\nline two";
  DOM_CHECK(ReasonTrace::RenderReason(quoted).find("\\n") != std::string::npos);
}

DOM_TEST(generation_vectors_are_indexed_by_class) {
  GenerationVector generations;
  generations.epoch = ControlEpoch::FromValue(3);
  generations.incarnation = Incarnation::FromValue(4);
  generations.policy = PolicyGeneration::FromValue(5);
  generations.evidence = EvidenceRevision::FromValue(6);
  for (const EvidenceClass cls : AllEvidenceClasses()) {
    generations.Set(cls, Generation::FromValue(static_cast<std::uint64_t>(cls) * 10));
  }
  DOM_CHECK_EQ(generations.Of(EvidenceClass::Incident).value(), 10ull);
  DOM_CHECK_EQ(generations.Of(EvidenceClass::Obligation).value(), 80ull);
  DOM_CHECK(generations.digest() != Digest::Zero());
  GenerationVector copy = generations;
  DOM_CHECK(copy == generations);
  copy.Set(EvidenceClass::Incident, Generation::FromValue(11));
  DOM_CHECK(!(copy == generations));
}

}  // namespace
