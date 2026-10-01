// Degraded Operation Manager - evidence intake, posture and freshness.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dom/evidence.hpp"

#include <algorithm>
#include <string>

#include "codec/binary_reader.hpp"
#include "codec/canonical.hpp"
#include "dom/limits.hpp"

namespace dom {
namespace {

constexpr EvidenceClass kAllClasses[kEvidenceClassCount] = {
    EvidenceClass::Incident,   EvidenceClass::FailureDomain, EvidenceClass::Capacity,
    EvidenceClass::Redundancy, EvidenceClass::Power,         EvidenceClass::Cooling,
    EvidenceClass::Maintenance, EvidenceClass::Obligation,
};

std::size_t ClassIndex(EvidenceClass cls) { return static_cast<std::size_t>(cls); }

int FreshnessSeverity(Freshness freshness) {
  switch (freshness) {
    case Freshness::Fresh:
      return 0;
    case Freshness::Stale:
      return 1;
    case Freshness::Expired:
      return 2;
    case Freshness::FutureDated:
      return 3;
    case Freshness::Missing:
      return 4;
    case Freshness::Conflicted:
      return 5;
  }
  return 5;
}

bool RecordLess(const EvidenceRecord& a, const EvidenceRecord& b) {
  if (a.cls != b.cls) {
    return static_cast<int>(a.cls) < static_cast<int>(b.cls);
  }
  if (a.subject != b.subject) {
    return a.subject < b.subject;
  }
  if (a.metric != b.metric) {
    return a.metric < b.metric;
  }
  if (a.producer != b.producer) {
    return a.producer < b.producer;
  }
  if (a.generation != b.generation) {
    return a.generation < b.generation;
  }
  if (a.observed_at_ms != b.observed_at_ms) {
    return a.observed_at_ms < b.observed_at_ms;
  }
  if (a.value != b.value) {
    return a.value.value() < b.value.value();
  }
  return a.source_digest < b.source_digest;
}

bool SameKey(const EvidenceKey& key, const EvidenceRecord& record) {
  return key.cls == record.cls && key.subject == record.subject &&
         key.metric == record.metric;
}

bool KeyLess(const EvidenceKey& a, const EvidenceKey& b) {
  if (a.cls != b.cls) {
    return static_cast<int>(a.cls) < static_cast<int>(b.cls);
  }
  if (a.subject != b.subject) {
    return a.subject < b.subject;
  }
  return a.metric < b.metric;
}

bool KeyEqual(const EvidenceKey& a, const EvidenceKey& b) {
  return !KeyLess(a, b) && !KeyLess(b, a);
}

/// Specificity of an age rule for a key: exact subject and metric score higher
/// than wildcards.
int RuleSpecificity(const AgeRule& rule, const KeyPosture& posture) {
  if (rule.cls != posture.key.cls) {
    return -1;
  }
  if (rule.subject != "*" && rule.subject != posture.key.subject) {
    return -1;
  }
  if (rule.metric != "*" && rule.metric != posture.key.metric) {
    return -1;
  }
  int score = 0;
  if (rule.subject != "*") {
    score += 2;
  }
  if (rule.metric != "*") {
    score += 1;
  }
  return score;
}

}  // namespace

std::span<const EvidenceClass> AllEvidenceClasses() {
  return std::span<const EvidenceClass>(kAllClasses, kEvidenceClassCount);
}

const char* EvidenceClassName(EvidenceClass cls) noexcept {
  switch (cls) {
    case EvidenceClass::Incident:
      return "incident";
    case EvidenceClass::FailureDomain:
      return "failure-domain";
    case EvidenceClass::Capacity:
      return "capacity";
    case EvidenceClass::Redundancy:
      return "redundancy";
    case EvidenceClass::Power:
      return "power";
    case EvidenceClass::Cooling:
      return "cooling";
    case EvidenceClass::Maintenance:
      return "maintenance";
    case EvidenceClass::Obligation:
      return "obligation";
  }
  return "unknown";
}

Result<EvidenceClass> EvidenceClassFromName(std::string_view name) {
  for (EvidenceClass cls : kAllClasses) {
    if (name == EvidenceClassName(cls)) {
      return Result<EvidenceClass>::Ok(cls);
    }
  }
  return Result<EvidenceClass>::Err(ErrorCode::InvalidArgument, "unknown evidence class name");
}

bool operator==(const EvidenceRecord& a, const EvidenceRecord& b) {
  return a.cls == b.cls && a.subject == b.subject && a.metric == b.metric &&
         a.value == b.value && a.observed_at_ms == b.observed_at_ms &&
         a.valid_until_ms == b.valid_until_ms && a.generation == b.generation &&
         a.source_digest == b.source_digest && a.producer == b.producer;
}

std::string EvidenceKey::ToString() const {
  return std::string(EvidenceClassName(cls)) + "/" + subject + "/" + metric;
}

bool operator==(const EvidenceKey& a, const EvidenceKey& b) { return KeyEqual(a, b); }
bool operator<(const EvidenceKey& a, const EvidenceKey& b) { return KeyLess(a, b); }

const char* FreshnessName(Freshness freshness) noexcept {
  switch (freshness) {
    case Freshness::Fresh:
      return "fresh";
    case Freshness::Stale:
      return "stale";
    case Freshness::Expired:
      return "expired";
    case Freshness::FutureDated:
      return "future-dated";
    case Freshness::Missing:
      return "missing";
    case Freshness::Conflicted:
      return "conflicted";
  }
  return "unknown";
}

bool IsUsable(Freshness freshness) noexcept { return freshness == Freshness::Fresh; }

Result<EvidenceSnapshot> EvidenceSnapshot::Build(std::vector<EvidenceRecord> records,
                                                 EvidenceRevision revision) {
  if (records.size() > limits::kMaxEvidenceRecords) {
    return Result<EvidenceSnapshot>::Err(ErrorCode::LimitExceeded,
                                         "evidence snapshot exceeds the record limit");
  }
  for (const EvidenceRecord& record : records) {
    if (record.subject.empty()) {
      return Result<EvidenceSnapshot>::Err(ErrorCode::InvalidArgument,
                                           "evidence subject must not be empty");
    }
    if (record.metric.empty()) {
      return Result<EvidenceSnapshot>::Err(ErrorCode::InvalidArgument,
                                           "evidence metric must not be empty");
    }
    if (record.producer.empty()) {
      return Result<EvidenceSnapshot>::Err(ErrorCode::InvalidArgument,
                                           "evidence producer must not be empty");
    }
    if (record.subject.size() > limits::kMaxStringBytes ||
        record.metric.size() > limits::kMaxStringBytes ||
        record.producer.size() > limits::kMaxStringBytes) {
      return Result<EvidenceSnapshot>::Err(ErrorCode::LimitExceeded,
                                           "evidence string exceeds the byte limit");
    }
    Status utf8 = internal::ValidateUtf8(record.subject);
    if (!utf8.ok()) {
      return Result<EvidenceSnapshot>::Err(utf8);
    }
    utf8 = internal::ValidateUtf8(record.metric);
    if (!utf8.ok()) {
      return Result<EvidenceSnapshot>::Err(utf8);
    }
    utf8 = internal::ValidateUtf8(record.producer);
    if (!utf8.ok()) {
      return Result<EvidenceSnapshot>::Err(utf8);
    }
  }

  std::sort(records.begin(), records.end(), RecordLess);
  records.erase(std::unique(records.begin(), records.end()), records.end());

  EvidenceSnapshot snapshot;
  snapshot.records_ = std::move(records);
  snapshot.revision_ = revision;

  std::size_t index = 0;
  while (index < snapshot.records_.size()) {
    EvidenceKey key;
    key.cls = snapshot.records_[index].cls;
    key.subject = snapshot.records_[index].subject;
    key.metric = snapshot.records_[index].metric;
    std::size_t end = index + 1;
    while (end < snapshot.records_.size() && SameKey(key, snapshot.records_[end])) {
      ++end;
    }
    if (end - index > 1) {
      snapshot.conflicted_keys_.push_back(key);
    }
    snapshot.keys_.push_back(std::move(key));
    index = end;
  }

  auto payload = internal::EncodeEvidencePayload(snapshot.records_, snapshot.revision_);
  if (!payload) {
    return Result<EvidenceSnapshot>::Err(payload.status());
  }
  snapshot.digest_ = Sha256::Of(payload.value());
  return Result<EvidenceSnapshot>::Ok(std::move(snapshot));
}

Generation EvidenceSnapshot::ClassGeneration(EvidenceClass cls) const {
  Generation generation;
  for (const EvidenceRecord& record : records_) {
    if (record.cls == cls && generation < record.generation) {
      generation = record.generation;
    }
  }
  return generation;
}

std::size_t EvidenceSnapshot::ClassRecordCount(EvidenceClass cls) const {
  std::size_t count = 0;
  for (const EvidenceRecord& record : records_) {
    if (record.cls == cls) {
      ++count;
    }
  }
  return count;
}

EvidenceView::EvidenceView(const EvidenceSnapshot& snapshot, Instant now_ms,
                           Duration default_max_age_ms, const std::vector<AgeRule>& ages)
    : snapshot_(&snapshot),
      now_ms_(now_ms),
      default_max_age_ms_(default_max_age_ms),
      ages_(ages) {
  postures_.reserve(snapshot.keys().size());
  // Keys and records share the same canonical order, so one pass over the
  // records yields every key range and keeps construction linear.
  std::size_t record_cursor = 0;
  std::size_t conflicted_cursor = 0;
  for (const EvidenceKey& key : snapshot.keys()) {
    KeyPosture posture;
    posture.key = key;
    const std::size_t record_index = record_cursor;
    while (record_cursor < snapshot.records().size() &&
           SameKey(key, snapshot.records()[record_cursor])) {
      ++record_cursor;
    }
    const std::size_t record_end = record_cursor;
    posture.record_count = record_end - record_index;

    while (conflicted_cursor < snapshot.conflicted_keys().size() &&
           KeyLess(snapshot.conflicted_keys()[conflicted_cursor], key)) {
      ++conflicted_cursor;
    }
    const bool conflicted =
        conflicted_cursor < snapshot.conflicted_keys().size() &&
        KeyEqual(snapshot.conflicted_keys()[conflicted_cursor], key);
    if (conflicted) {
      posture.freshness = Freshness::Conflicted;
      posture.detail = std::to_string(posture.record_count) +
                       " disagreeing publications for one key";
      postures_.push_back(std::move(posture));
      continue;
    }

    const EvidenceRecord* authoritative = nullptr;
    for (std::size_t i = record_index; i < record_end; ++i) {
      const EvidenceRecord& candidate = snapshot.records()[i];
      if (authoritative == nullptr) {
        authoritative = &candidate;
        continue;
      }
      if (authoritative->generation < candidate.generation ||
          (authoritative->generation == candidate.generation &&
           (authoritative->observed_at_ms < candidate.observed_at_ms ||
            (authoritative->observed_at_ms == candidate.observed_at_ms &&
             authoritative->producer < candidate.producer)))) {
        authoritative = &candidate;
      }
    }
    if (authoritative == nullptr) {
      posture.freshness = Freshness::Missing;
      posture.detail = "no publication";
      postures_.push_back(std::move(posture));
      continue;
    }

    posture.has_authoritative = true;
    posture.generation = authoritative->generation;
    posture.observed_at_ms = authoritative->observed_at_ms;
    posture.value = authoritative->value;
    posture.producer = authoritative->producer;

    if (authoritative->observed_at_ms > now_ms_) {
      posture.freshness = Freshness::FutureDated;
      posture.detail = "observed after the evaluation instant";
      postures_.push_back(std::move(posture));
      continue;
    }
    if (authoritative->HasExplicitExpiry() && now_ms_ > authoritative->valid_until_ms) {
      posture.freshness = Freshness::Expired;
      posture.detail = "explicit validity has passed";
      postures_.push_back(std::move(posture));
      continue;
    }
    Duration max_age = default_max_age_ms_;
    int best_score = -1;
    for (const AgeRule& rule : ages_) {
      const int score = RuleSpecificity(rule, posture);
      if (score < 0) {
        continue;
      }
      if (score > best_score || (score == best_score && rule.max_age_ms < max_age)) {
        best_score = score;
        max_age = rule.max_age_ms;
      }
    }
    const Duration age = Duration::FromValue(now_ms_.value() - authoritative->observed_at_ms.value());
    if (max_age.value() != 0 && age > max_age) {
      posture.freshness = Freshness::Stale;
      posture.detail = "age " + age.ToString() + "ms exceeds " + max_age.ToString() + "ms";
      postures_.push_back(std::move(posture));
      continue;
    }
    posture.freshness = Freshness::Fresh;
    posture.detail = "age " + age.ToString() + "ms within " + max_age.ToString() + "ms";
    postures_.push_back(std::move(posture));
  }

  std::size_t cursor = 0;
  for (std::size_t cls = 1; cls <= kEvidenceClassCount; ++cls) {
    ranges_[cls].begin = cursor;
    while (cursor < postures_.size() &&
           static_cast<std::size_t>(postures_[cursor].key.cls) == cls) {
      ++cursor;
    }
    ranges_[cls].end = cursor;
  }
  ranges_[0].begin = 0;
  ranges_[0].end = 0;
}

const KeyPosture* EvidenceView::Find(EvidenceClass cls, std::string_view subject,
                                     std::string_view metric) const {
  for (const KeyPosture& posture : postures_) {
    if (posture.key.cls == cls && posture.key.subject == subject &&
        posture.key.metric == metric) {
      return &posture;
    }
  }
  return nullptr;
}

Freshness EvidenceView::FreshnessOf(EvidenceClass cls, std::string_view subject,
                                    std::string_view metric) const {
  const KeyPosture* posture = Find(cls, subject, metric);
  if (posture == nullptr) {
    return Freshness::Missing;
  }
  return posture->freshness;
}

Freshness EvidenceView::ClassFreshness(EvidenceClass cls) const {
  const std::size_t index = ClassIndex(cls);
  const ClassRange range = index <= kEvidenceClassCount ? ranges_[index] : ClassRange{};
  bool any = false;
  int worst = 0;
  for (std::size_t i = range.begin; i < range.end; ++i) {
    any = true;
    worst = (std::max)(worst, FreshnessSeverity(postures_[i].freshness));
  }
  if (!any) {
    return Freshness::Missing;
  }
  for (int severity = 0; severity <= 5; ++severity) {
    if (severity == worst) {
      switch (severity) {
        case 0:
          return Freshness::Fresh;
        case 1:
          return Freshness::Stale;
        case 2:
          return Freshness::Expired;
        case 3:
          return Freshness::FutureDated;
        case 4:
          return Freshness::Missing;
        default:
          return Freshness::Conflicted;
      }
    }
  }
  return Freshness::Conflicted;
}

std::size_t EvidenceView::FreshKeyCount(EvidenceClass cls) const {
  const std::size_t index = ClassIndex(cls);
  const ClassRange range = index <= kEvidenceClassCount ? ranges_[index] : ClassRange{};
  std::size_t count = 0;
  for (std::size_t i = range.begin; i < range.end; ++i) {
    if (postures_[i].freshness == Freshness::Fresh) {
      ++count;
    }
  }
  return count;
}

std::vector<const KeyPosture*> EvidenceView::Select(EvidenceClass cls, std::string_view subject,
                                                    std::string_view metric,
                                                    bool require_fresh) const {
  std::vector<const KeyPosture*> selected;
  const std::size_t class_index = ClassIndex(cls);
  const ClassRange range =
      class_index <= kEvidenceClassCount ? ranges_[class_index] : ClassRange{};
  for (std::size_t i = range.begin; i < range.end; ++i) {
    const KeyPosture& posture = postures_[i];
    if (subject != "*" && posture.key.subject != subject) {
      continue;
    }
    if (metric != "*" && posture.key.metric != metric) {
      continue;
    }
    if (require_fresh) {
      if (posture.freshness != Freshness::Fresh) {
        continue;
      }
    } else if (posture.freshness == Freshness::Conflicted ||
               posture.freshness == Freshness::FutureDated ||
               posture.freshness == Freshness::Missing) {
      continue;
    }
    if (!posture.has_authoritative) {
      continue;
    }
    selected.push_back(&posture);
  }
  return selected;
}

SelectionPosture EvidenceView::PostureFor(EvidenceClass cls, std::string_view subject,
                                          std::string_view metric) const {
  SelectionPosture selection;
  int worst = -1;
  const std::size_t class_index = ClassIndex(cls);
  const ClassRange range =
      class_index <= kEvidenceClassCount ? ranges_[class_index] : ClassRange{};
  for (std::size_t i = range.begin; i < range.end; ++i) {
    const KeyPosture& posture = postures_[i];
    if (subject != "*" && posture.key.subject != subject) {
      continue;
    }
    if (metric != "*" && posture.key.metric != metric) {
      continue;
    }
    ++selection.keys;
    if (posture.freshness == Freshness::Fresh) {
      ++selection.fresh_keys;
    }
    const int severity = FreshnessSeverity(posture.freshness);
    if (severity > worst) {
      worst = severity;
      selection.detail = posture.key.ToString() + " is " + FreshnessName(posture.freshness);
    }
  }
  if (selection.keys == 0) {
    selection.freshness = Freshness::Missing;
    selection.detail = "no publication for the requirement";
    return selection;
  }
  switch (worst) {
    case 0:
      selection.freshness = Freshness::Fresh;
      break;
    case 1:
      selection.freshness = Freshness::Stale;
      break;
    case 2:
      selection.freshness = Freshness::Expired;
      break;
    case 3:
      selection.freshness = Freshness::FutureDated;
      break;
    case 4:
      selection.freshness = Freshness::Missing;
      break;
    default:
      selection.freshness = Freshness::Conflicted;
      break;
  }
  return selection;
}

Duration EvidenceView::MaxAgeOf(EvidenceClass cls, std::string_view subject,
                                std::string_view metric) const {
  KeyPosture posture;
  posture.key.cls = cls;
  posture.key.subject = std::string(subject);
  posture.key.metric = std::string(metric);
  Duration max_age = default_max_age_ms_;
  int best_score = -1;
  for (const AgeRule& rule : ages_) {
    const int score = RuleSpecificity(rule, posture);
    if (score < 0) {
      continue;
    }
    if (score > best_score || (score == best_score && rule.max_age_ms < max_age)) {
      best_score = score;
      max_age = rule.max_age_ms;
    }
  }
  return max_age;
}

}  // namespace dom
