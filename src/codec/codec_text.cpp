// Degraded Operation Manager - canonical text codec.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The text form is line oriented and canonical: one keyword per line, values
// quoted when they are free text, numbers in decimal, and every collection in
// the same canonical order the binary codec uses. Parsing is strict - unknown
// keywords, wrong arity, malformed numbers, bad units and non-canonical
// ordering are all refused with the offending line number.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "codec/binary_reader.hpp"
#include "codec/canonical.hpp"
#include "dom/codec.hpp"
#include "dom/limits.hpp"

namespace dom {
namespace {

struct Line {
  std::size_t number = 0;
  std::string text;
};

Status TextError(std::size_t line, const std::string& detail) {
  return Status::Error(ErrorCode::DecodeError,
                       "line " + std::to_string(line) + ": " + detail);
}

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

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// Splits one line into tokens. Quoted tokens are unescaped; a quote may not
/// appear inside an unquoted token.
Result<std::vector<std::string>> Tokenize(std::string_view line, std::size_t line_number) {
  std::vector<std::string> tokens;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && IsSpace(line[i])) {
      ++i;
    }
    if (i >= line.size()) {
      break;
    }
    // A quoted section may start anywhere inside a token, so name="value with
    // spaces" is one token and its value keeps the spaces.
    std::string token;
    bool started = false;
    while (i < line.size() && !IsSpace(line[i])) {
      if (line[i] == '#' && !started) {
        break;
      }
      if (line[i] == '"') {
        ++i;
        bool closed = false;
        while (i < line.size()) {
          const char c = line[i];
          if (c == '\\') {
            if (i + 1 >= line.size()) {
              return Result<std::vector<std::string>>::Err(
                  TextError(line_number, "dangling escape in a quoted value"));
            }
            const char next = line[i + 1];
            switch (next) {
              case '"':
                token.push_back('"');
                break;
              case '\\':
                token.push_back('\\');
                break;
              case 'n':
                token.push_back('\n');
                break;
              case 'r':
                token.push_back('\r');
                break;
              case 't':
                token.push_back('\t');
                break;
              default:
                return Result<std::vector<std::string>>::Err(
                    TextError(line_number, "unsupported escape sequence in a quoted value"));
            }
            i += 2;
            continue;
          }
          if (c == '"') {
            closed = true;
            ++i;
            break;
          }
          token.push_back(c);
          ++i;
        }
        if (!closed) {
          return Result<std::vector<std::string>>::Err(
              TextError(line_number, "unterminated quoted value"));
        }
        started = true;
        continue;
      }
      token.push_back(line[i]);
      ++i;
      started = true;
    }
    if (!started) {
      break;
    }
    tokens.push_back(std::move(token));
  }
  return Result<std::vector<std::string>>::Ok(std::move(tokens));
}

Result<std::uint64_t> ParseU64(const std::string& token, std::size_t line_number,
                               std::string_view what) {
  if (token.empty()) {
    return Result<std::uint64_t>::Err(TextError(line_number, std::string(what) + " is empty"));
  }
  std::uint64_t value = 0;
  const char* begin = token.data();
  const char* end = token.data() + token.size();
  const auto parsed = std::from_chars(begin, end, value, 10);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return Result<std::uint64_t>::Err(
        TextError(line_number, std::string(what) + " is not an unsigned decimal integer"));
  }
  return Result<std::uint64_t>::Ok(value);
}

Result<std::int64_t> ParseI64(const std::string& token, std::size_t line_number,
                              std::string_view what) {
  std::int64_t value = 0;
  const char* begin = token.data();
  const char* end = token.data() + token.size();
  const auto parsed = std::from_chars(begin, end, value, 10);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return Result<std::int64_t>::Err(
        TextError(line_number, std::string(what) + " is not a decimal integer"));
  }
  return Result<std::int64_t>::Ok(value);
}

Result<std::uint8_t> ParseU8(const std::string& token, std::size_t line_number,
                             std::string_view what) {
  auto value = ParseU64(token, line_number, what);
  if (!value) {
    return Result<std::uint8_t>::Err(value.status());
  }
  auto narrowed = NarrowU8(value.value());
  if (!narrowed) {
    return Result<std::uint8_t>::Err(TextError(line_number, std::string(what) + " is too large"));
  }
  return Result<std::uint8_t>::Ok(narrowed.value());
}

Result<Scalar> ParseScalar(const std::string& token, std::size_t line_number) {
  const std::size_t separator = token.find(':');
  if (separator == std::string::npos) {
    return Result<Scalar>::Err(
        TextError(line_number, "scalar must be written as unit:value"));
  }
  auto unit = UnitFromName(std::string_view(token).substr(0, separator));
  if (!unit) {
    return Result<Scalar>::Err(TextError(line_number, "unknown scalar unit"));
  }
  auto value = ParseI64(token.substr(separator + 1), line_number, "scalar value");
  if (!value) {
    return Result<Scalar>::Err(value.status());
  }
  return Result<Scalar>::Ok(Scalar(unit.value(), value.value()));
}

Result<Digest> ParseDigest(const std::string& token, std::size_t line_number) {
  auto digest = Digest::FromHex(token);
  if (!digest) {
    return Result<Digest>::Err(TextError(line_number, "invalid 64 character digest"));
  }
  return digest;
}

Result<EvidenceClass> ParseEvidenceClass(const std::string& token, std::size_t line_number) {
  auto cls = EvidenceClassFromName(token);
  if (!cls) {
    return Result<EvidenceClass>::Err(TextError(line_number, "unknown evidence class"));
  }
  return cls;
}

Result<OperatingClass> ParseOperatingClass(const std::string& token, std::size_t line_number) {
  auto cls = OperatingClassFromName(token);
  if (!cls) {
    return Result<OperatingClass>::Err(TextError(line_number, "unknown operating class"));
  }
  return cls;
}

Result<std::vector<Line>> SplitLines(std::string_view text) {
  const Status utf8 = internal::ValidateUtf8(text);
  if (!utf8.ok()) {
    return Result<std::vector<Line>>::Err(utf8);
  }
  std::vector<Line> lines;
  std::size_t number = 1;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view raw = text.substr(start, end - start);
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' ' || raw.back() == '\t')) {
      raw.remove_suffix(1);
    }
    std::size_t begin = 0;
    while (begin < raw.size() && (raw[begin] == ' ' || raw[begin] == '\t')) {
      ++begin;
    }
    raw = raw.substr(begin);
    if (!raw.empty() && raw[0] != '#') {
      Line line;
      line.number = number;
      line.text = std::string(raw);
      lines.push_back(std::move(line));
    }
    if (end == text.size()) {
      break;
    }
    start = end + 1;
    ++number;
  }
  return Result<std::vector<Line>>::Ok(std::move(lines));
}

Status ExpectTokens(const std::vector<std::string>& tokens, std::size_t expected,
                    std::size_t line_number, std::string_view keyword) {
  if (tokens.size() != expected) {
    return TextError(line_number, std::string(keyword) + " expects " +
                                      std::to_string(expected - 1) + " value(s)");
  }
  return Status::Ok();
}

}  // namespace

std::string EncodePolicyText(const PolicyDocument& policy) {
  PolicyDocument canonical = policy;
  internal::CanonicalizePolicy(canonical);

  std::string out;
  out += "dom.policy 1\n";
  out += "id " + canonical.id.ToString() + "\n";
  out += "generation " + canonical.generation.ToString() + "\n";
  out += "revision " + Quote(canonical.revision_key) + "\n";
  out += "max-recovery-step " + std::to_string(canonical.max_recovery_step) + "\n";
  out += "default-max-age-ms " + canonical.default_max_age_ms.ToString() + "\n";
  for (const std::string& service : canonical.service_classes) {
    out += "service " + Quote(service) + "\n";
  }
  for (const ObligationClass& obligation : canonical.obligations) {
    out += "obligation " + obligation.id.ToString() + " " + Quote(obligation.key) + " " +
           ProtectionLevelName(obligation.protection);
    for (const std::string& service : obligation.service_classes) {
      out += " service " + Quote(service);
    }
    out += "\n";
  }
  for (const ModeDefinition& mode : canonical.modes) {
    out += "mode " + mode.id.ToString() + " " + Quote(mode.key) + " " +
           OperatingClassName(mode.cls) + " rev=" + mode.revision.ToString() +
           " latch=" + LatchModeName(mode.latch) + " dwell=" + mode.min_dwell_ms.ToString() +
           " hold=" + mode.recovery_hold_ms.ToString() + "\n";
    for (const Predicate& predicate : mode.entry) {
      out += "  entry " + std::string(EvidenceClassName(predicate.cls)) + " " +
             Quote(predicate.subject) + " " + Quote(predicate.metric) + " " +
             AggregationName(predicate.aggregation) + " " + ComparatorName(predicate.comparator) +
             " " + predicate.threshold.ToString() +
             (predicate.require_fresh ? " fresh" : " any-age") + "\n";
    }
    for (const Predicate& predicate : mode.exit) {
      out += "  exit " + std::string(EvidenceClassName(predicate.cls)) + " " +
             Quote(predicate.subject) + " " + Quote(predicate.metric) + " " +
             AggregationName(predicate.aggregation) + " " + ComparatorName(predicate.comparator) +
             " " + predicate.threshold.ToString() +
             (predicate.require_fresh ? " fresh" : " any-age") + "\n";
    }
    for (const RestrictionRule& rule : mode.restrictions) {
      out += "  restrict " + rule.id.ToString() + " " + Quote(rule.service_class) + " " +
             RestrictionKindName(rule.kind) + " " + rule.allowance.ToString() + "\n";
    }
    for (const ObligationClassId id : mode.protected_obligations) {
      out += "  protect " + id.ToString() + "\n";
    }
  }
  for (const RestrictionRule& rule : canonical.indeterminate_restrictions) {
    out += "indeterminate restrict " + rule.id.ToString() + " " + Quote(rule.service_class) +
           " " + RestrictionKindName(rule.kind) + " " + rule.allowance.ToString() + "\n";
  }
  for (const EvidenceRequirement& requirement : canonical.requirements) {
    out += "requirement " + std::string(EvidenceClassName(requirement.cls)) + " " +
           Quote(requirement.subject) + " " + Quote(requirement.metric) + " " +
           RequirementScopeName(requirement.scope) + " " + requirement.max_age_ms.ToString() +
           "\n";
  }
  return out;
}

Result<PolicyDocument> DecodePolicyText(std::string_view text) {
  auto lines = SplitLines(text);
  if (!lines) {
    return Result<PolicyDocument>::Err(lines.status());
  }
  PolicyDocument policy;
  bool seen_header = false;
  ModeDefinition* current_mode = nullptr;

  for (const Line& line : lines.value()) {
    auto tokens = Tokenize(line.text, line.number);
    if (!tokens) {
      return Result<PolicyDocument>::Err(tokens.status());
    }
    const std::vector<std::string>& t = tokens.value();
    if (t.empty()) {
      continue;
    }
    const std::string& keyword = t[0];
    if (!seen_header) {
      if (keyword != "dom.policy" || t.size() != 2 || t[1] != "1") {
        return Result<PolicyDocument>::Err(
            TextError(line.number, "the first line must be 'dom.policy 1'"));
      }
      seen_header = true;
      continue;
    }
    if (keyword == "id") {
      Status arity = ExpectTokens(t, 2, line.number, keyword);
      if (!arity.ok()) {
        return Result<PolicyDocument>::Err(arity);
      }
      auto value = ParseU64(t[1], line.number, "policy id");
      if (!value) {
        return Result<PolicyDocument>::Err(value.status());
      }
      policy.id = PolicyId::FromValue(value.value());
    } else if (keyword == "generation") {
      Status arity = ExpectTokens(t, 2, line.number, keyword);
      if (!arity.ok()) {
        return Result<PolicyDocument>::Err(arity);
      }
      auto value = ParseU64(t[1], line.number, "policy generation");
      if (!value) {
        return Result<PolicyDocument>::Err(value.status());
      }
      policy.generation = PolicyGeneration::FromValue(value.value());
    } else if (keyword == "revision") {
      Status arity = ExpectTokens(t, 2, line.number, keyword);
      if (!arity.ok()) {
        return Result<PolicyDocument>::Err(arity);
      }
      policy.revision_key = t[1];
    } else if (keyword == "max-recovery-step") {
      Status arity = ExpectTokens(t, 2, line.number, keyword);
      if (!arity.ok()) {
        return Result<PolicyDocument>::Err(arity);
      }
      auto value = ParseU8(t[1], line.number, "max recovery step");
      if (!value) {
        return Result<PolicyDocument>::Err(value.status());
      }
      policy.max_recovery_step = value.value();
    } else if (keyword == "default-max-age-ms") {
      Status arity = ExpectTokens(t, 2, line.number, keyword);
      if (!arity.ok()) {
        return Result<PolicyDocument>::Err(arity);
      }
      auto value = ParseU64(t[1], line.number, "default maximum age");
      if (!value) {
        return Result<PolicyDocument>::Err(value.status());
      }
      policy.default_max_age_ms = Duration::FromValue(value.value());
    } else if (keyword == "service") {
      Status arity = ExpectTokens(t, 2, line.number, keyword);
      if (!arity.ok()) {
        return Result<PolicyDocument>::Err(arity);
      }
      policy.service_classes.push_back(t[1]);
    } else if (keyword == "obligation") {
      if (t.size() < 4 || (t.size() - 4) % 2 != 0) {
        return Result<PolicyDocument>::Err(
            TextError(line.number, "obligation expects id, key, protection and service pairs"));
      }
      auto id = ParseU64(t[1], line.number, "obligation id");
      if (!id) {
        return Result<PolicyDocument>::Err(id.status());
      }
      auto protection = ProtectionLevelFromName(t[3]);
      if (!protection) {
        return Result<PolicyDocument>::Err(TextError(line.number, "unknown protection level"));
      }
      ObligationClass obligation;
      obligation.id = ObligationClassId::FromValue(id.value());
      obligation.key = t[2];
      obligation.protection = protection.value();
      for (std::size_t i = 4; i < t.size(); i += 2) {
        if (t[i] != "service") {
          return Result<PolicyDocument>::Err(
              TextError(line.number, "obligation services are written as 'service \"name\"'"));
        }
        obligation.service_classes.push_back(t[i + 1]);
      }
      policy.obligations.push_back(std::move(obligation));
    } else if (keyword == "mode") {
      if (t.size() != 8) {
        return Result<PolicyDocument>::Err(
            TextError(line.number, "mode expects id, key, class, rev=, latch=, dwell=, hold="));
      }
      auto id = ParseU64(t[1], line.number, "mode id");
      if (!id) {
        return Result<PolicyDocument>::Err(id.status());
      }
      auto cls = ParseOperatingClass(t[3], line.number);
      if (!cls) {
        return Result<PolicyDocument>::Err(cls.status());
      }
      ModeDefinition mode;
      mode.id = ModeId::FromValue(id.value());
      mode.key = t[2];
      mode.cls = cls.value();
      for (std::size_t i = 4; i < 8; ++i) {
        const std::string& attribute = t[i];
        const std::size_t separator = attribute.find('=');
        if (separator == std::string::npos) {
          return Result<PolicyDocument>::Err(
              TextError(line.number, "mode attributes are written as name=value"));
        }
        const std::string name = attribute.substr(0, separator);
        const std::string value = attribute.substr(separator + 1);
        if (name == "rev") {
          auto revision = ParseU64(value, line.number, "mode revision");
          if (!revision) {
            return Result<PolicyDocument>::Err(revision.status());
          }
          mode.revision = RevisionId::FromValue(revision.value());
        } else if (name == "latch") {
          auto latch = LatchModeFromName(value);
          if (!latch) {
            return Result<PolicyDocument>::Err(TextError(line.number, "unknown latch mode"));
          }
          mode.latch = latch.value();
        } else if (name == "dwell") {
          auto dwell = ParseU64(value, line.number, "minimum dwell");
          if (!dwell) {
            return Result<PolicyDocument>::Err(dwell.status());
          }
          mode.min_dwell_ms = Duration::FromValue(dwell.value());
        } else if (name == "hold") {
          auto hold = ParseU64(value, line.number, "recovery hold");
          if (!hold) {
            return Result<PolicyDocument>::Err(hold.status());
          }
          mode.recovery_hold_ms = Duration::FromValue(hold.value());
        } else {
          return Result<PolicyDocument>::Err(
              TextError(line.number, "unknown mode attribute '" + name + "'"));
        }
      }
      policy.modes.push_back(std::move(mode));
      current_mode = &policy.modes.back();
    } else if (keyword == "entry" || keyword == "exit") {
      if (current_mode == nullptr) {
        return Result<PolicyDocument>::Err(
            TextError(line.number, "criteria must follow a mode line"));
      }
      if (t.size() != 8) {
        return Result<PolicyDocument>::Err(TextError(
            line.number, "criteria expect class, subject, metric, aggregation, comparator, "
                         "threshold and freshness"));
      }
      auto cls = ParseEvidenceClass(t[1], line.number);
      if (!cls) {
        return Result<PolicyDocument>::Err(cls.status());
      }
      auto aggregation = AggregationFromName(t[4]);
      if (!aggregation) {
        return Result<PolicyDocument>::Err(TextError(line.number, "unknown aggregation"));
      }
      auto comparator = ComparatorFromName(t[5]);
      if (!comparator) {
        return Result<PolicyDocument>::Err(TextError(line.number, "unknown comparator"));
      }
      auto threshold = ParseScalar(t[6], line.number);
      if (!threshold) {
        return Result<PolicyDocument>::Err(threshold.status());
      }
      bool require_fresh = true;
      if (t[7] == "fresh") {
        require_fresh = true;
      } else if (t[7] == "any-age") {
        require_fresh = false;
      } else {
        return Result<PolicyDocument>::Err(
            TextError(line.number, "freshness must be 'fresh' or 'any-age'"));
      }
      Predicate predicate;
      predicate.cls = cls.value();
      predicate.subject = t[2];
      predicate.metric = t[3];
      predicate.aggregation = aggregation.value();
      predicate.comparator = comparator.value();
      predicate.threshold = threshold.value();
      predicate.require_fresh = require_fresh;
      if (keyword == "entry") {
        current_mode->entry.push_back(std::move(predicate));
      } else {
        current_mode->exit.push_back(std::move(predicate));
      }
    } else if (keyword == "restrict") {
      if (current_mode == nullptr) {
        return Result<PolicyDocument>::Err(
            TextError(line.number, "restrictions must follow a mode line"));
      }
      Status arity = ExpectTokens(t, 5, line.number, keyword);
      if (!arity.ok()) {
        return Result<PolicyDocument>::Err(arity);
      }
      auto id = ParseU64(t[1], line.number, "restriction id");
      if (!id) {
        return Result<PolicyDocument>::Err(id.status());
      }
      auto kind = RestrictionKindFromName(t[3]);
      if (!kind) {
        return Result<PolicyDocument>::Err(TextError(line.number, "unknown restriction kind"));
      }
      auto allowance = ParseScalar(t[4], line.number);
      if (!allowance) {
        return Result<PolicyDocument>::Err(allowance.status());
      }
      RestrictionRule rule;
      rule.id = RestrictionId::FromValue(id.value());
      rule.service_class = t[2];
      rule.kind = kind.value();
      rule.allowance = allowance.value();
      current_mode->restrictions.push_back(std::move(rule));
    } else if (keyword == "protect") {
      if (current_mode == nullptr) {
        return Result<PolicyDocument>::Err(
            TextError(line.number, "protected obligations must follow a mode line"));
      }
      Status arity = ExpectTokens(t, 2, line.number, keyword);
      if (!arity.ok()) {
        return Result<PolicyDocument>::Err(arity);
      }
      auto id = ParseU64(t[1], line.number, "obligation id");
      if (!id) {
        return Result<PolicyDocument>::Err(id.status());
      }
      current_mode->protected_obligations.push_back(ObligationClassId::FromValue(id.value()));
    } else if (keyword == "indeterminate") {
      if (t.size() != 6 || t[1] != "restrict") {
        return Result<PolicyDocument>::Err(
            TextError(line.number, "indeterminate restrictions are written as "
                                   "'indeterminate restrict <id> \"service\" <kind> <allowance>'"));
      }
      auto id = ParseU64(t[2], line.number, "restriction id");
      if (!id) {
        return Result<PolicyDocument>::Err(id.status());
      }
      auto kind = RestrictionKindFromName(t[4]);
      if (!kind) {
        return Result<PolicyDocument>::Err(TextError(line.number, "unknown restriction kind"));
      }
      auto allowance = ParseScalar(t[5], line.number);
      if (!allowance) {
        return Result<PolicyDocument>::Err(allowance.status());
      }
      RestrictionRule rule;
      rule.id = RestrictionId::FromValue(id.value());
      rule.service_class = t[3];
      rule.kind = kind.value();
      rule.allowance = allowance.value();
      policy.indeterminate_restrictions.push_back(std::move(rule));
      current_mode = nullptr;
    } else if (keyword == "requirement") {
      Status arity = ExpectTokens(t, 6, line.number, keyword);
      if (!arity.ok()) {
        return Result<PolicyDocument>::Err(arity);
      }
      auto cls = ParseEvidenceClass(t[1], line.number);
      if (!cls) {
        return Result<PolicyDocument>::Err(cls.status());
      }
      auto scope = RequirementScopeFromName(t[4]);
      if (!scope) {
        return Result<PolicyDocument>::Err(TextError(line.number, "unknown requirement scope"));
      }
      auto age = ParseU64(t[5], line.number, "maximum age");
      if (!age) {
        return Result<PolicyDocument>::Err(age.status());
      }
      EvidenceRequirement requirement;
      requirement.cls = cls.value();
      requirement.subject = t[2];
      requirement.metric = t[3];
      requirement.scope = scope.value();
      requirement.max_age_ms = Duration::FromValue(age.value());
      policy.requirements.push_back(std::move(requirement));
      current_mode = nullptr;
    } else {
      return Result<PolicyDocument>::Err(
          TextError(line.number, "unknown keyword '" + keyword + "'"));
    }
  }

  if (!seen_header) {
    return Result<PolicyDocument>::Err(
        Status::Error(ErrorCode::DecodeError, "the policy text is empty"));
  }
  internal::CanonicalizePolicy(policy);
  return Result<PolicyDocument>::Ok(std::move(policy));
}

std::string EncodeEvidenceText(const EvidenceSnapshot& snapshot) {
  std::string out;
  out += "dom.evidence 1\n";
  out += "revision " + snapshot.revision().ToString() + "\n";
  for (const EvidenceRecord& record : snapshot.records()) {
    out += "record " + std::string(EvidenceClassName(record.cls)) + " " + Quote(record.subject) +
           " " + Quote(record.metric) + " " + record.value.ToString() +
           " observed=" + record.observed_at_ms.ToString() +
           " until=" + record.valid_until_ms.ToString() +
           " gen=" + record.generation.ToString() + " src=" + record.source_digest.ToHex() +
           " producer=" + Quote(record.producer) + "\n";
  }
  return out;
}

Result<EvidenceSnapshot> DecodeEvidenceText(std::string_view text) {
  auto lines = SplitLines(text);
  if (!lines) {
    return Result<EvidenceSnapshot>::Err(lines.status());
  }
  std::vector<EvidenceRecord> records;
  EvidenceRevision revision;
  bool seen_header = false;

  for (const Line& line : lines.value()) {
    auto tokens = Tokenize(line.text, line.number);
    if (!tokens) {
      return Result<EvidenceSnapshot>::Err(tokens.status());
    }
    const std::vector<std::string>& t = tokens.value();
    if (t.empty()) {
      continue;
    }
    const std::string& keyword = t[0];
    if (!seen_header) {
      if (keyword != "dom.evidence" || t.size() != 2 || t[1] != "1") {
        return Result<EvidenceSnapshot>::Err(
            TextError(line.number, "the first line must be 'dom.evidence 1'"));
      }
      seen_header = true;
      continue;
    }
    if (keyword == "revision") {
      Status arity = ExpectTokens(t, 2, line.number, keyword);
      if (!arity.ok()) {
        return Result<EvidenceSnapshot>::Err(arity);
      }
      auto value = ParseU64(t[1], line.number, "evidence revision");
      if (!value) {
        return Result<EvidenceSnapshot>::Err(value.status());
      }
      revision = EvidenceRevision::FromValue(value.value());
    } else if (keyword == "record") {
      if (t.size() != 10) {
        return Result<EvidenceSnapshot>::Err(TextError(
            line.number, "record expects class, subject, metric, value, observed=, until=, "
                         "gen=, src= and producer="));
      }
      auto cls = ParseEvidenceClass(t[1], line.number);
      if (!cls) {
        return Result<EvidenceSnapshot>::Err(cls.status());
      }
      auto value = ParseScalar(t[4], line.number);
      if (!value) {
        return Result<EvidenceSnapshot>::Err(value.status());
      }
      EvidenceRecord record;
      record.cls = cls.value();
      record.subject = t[2];
      record.metric = t[3];
      record.value = value.value();
      bool seen_observed = false;
      bool seen_until = false;
      bool seen_generation = false;
      bool seen_source = false;
      bool seen_producer = false;
      for (std::size_t i = 5; i < t.size(); ++i) {
        const std::size_t separator = t[i].find('=');
        if (separator == std::string::npos) {
          return Result<EvidenceSnapshot>::Err(
              TextError(line.number, "record attributes are written as name=value"));
        }
        const std::string name = t[i].substr(0, separator);
        const std::string attribute = t[i].substr(separator + 1);
        if (name == "observed") {
          auto observed = ParseU64(attribute, line.number, "observed instant");
          if (!observed) {
            return Result<EvidenceSnapshot>::Err(observed.status());
          }
          record.observed_at_ms = Instant::FromValue(observed.value());
          seen_observed = true;
        } else if (name == "until") {
          auto until = ParseU64(attribute, line.number, "validity instant");
          if (!until) {
            return Result<EvidenceSnapshot>::Err(until.status());
          }
          record.valid_until_ms = Instant::FromValue(until.value());
          seen_until = true;
        } else if (name == "gen") {
          auto generation = ParseU64(attribute, line.number, "generation");
          if (!generation) {
            return Result<EvidenceSnapshot>::Err(generation.status());
          }
          record.generation = Generation::FromValue(generation.value());
          seen_generation = true;
        } else if (name == "src") {
          auto digest = ParseDigest(attribute, line.number);
          if (!digest) {
            return Result<EvidenceSnapshot>::Err(digest.status());
          }
          record.source_digest = digest.value();
          seen_source = true;
        } else if (name == "producer") {
          record.producer = attribute;
          seen_producer = true;
        } else {
          return Result<EvidenceSnapshot>::Err(
              TextError(line.number, "unknown record attribute '" + name + "'"));
        }
      }
      if (!seen_observed || !seen_until || !seen_generation || !seen_source || !seen_producer) {
        return Result<EvidenceSnapshot>::Err(
            TextError(line.number, "record is missing observed, until, gen, src or producer"));
      }
      records.push_back(std::move(record));
    } else {
      return Result<EvidenceSnapshot>::Err(
          TextError(line.number, "unknown keyword '" + keyword + "'"));
    }
  }

  if (!seen_header) {
    return Result<EvidenceSnapshot>::Err(
        Status::Error(ErrorCode::DecodeError, "the evidence text is empty"));
  }
  return EvidenceSnapshot::Build(std::move(records), revision);
}

}  // namespace dom
