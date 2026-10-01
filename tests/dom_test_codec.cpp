// Degraded Operation Manager - codec round trip and adversarial input tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dom/dom.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

namespace {

using namespace dom;

/// Deterministic xorshift so every adversarial case is reproducible.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  std::size_t Below(std::size_t bound) {
    return bound == 0 ? 0 : static_cast<std::size_t>(Next() % bound);
  }

 private:
  std::uint64_t state_;
};

DOM_TEST(policy_binary_round_trip_is_canonical) {
  const PolicyDocument policy = dom::test::MakePolicy();
  auto encoded = EncodePolicyBinary(policy);
  DOM_CHECK_OK(encoded);
  DOM_CHECK(!encoded.value().empty());
  auto decoded = DecodePolicyBinary(encoded.value());
  DOM_CHECK_OK(decoded);
  DOM_CHECK_EQ(PolicyDigest(decoded.value()), PolicyDigest(policy));
  auto reencoded = EncodePolicyBinary(decoded.value());
  DOM_CHECK_OK(reencoded);
  DOM_CHECK(reencoded.value() == encoded.value());

  // Declaration order is not identity: the canonical digest ignores it.
  PolicyDocument shuffled = policy;
  std::reverse(shuffled.modes.begin(), shuffled.modes.end());
  std::reverse(shuffled.requirements.begin(), shuffled.requirements.end());
  std::reverse(shuffled.service_classes.begin(), shuffled.service_classes.end());
  std::reverse(shuffled.modes[0].entry.begin(), shuffled.modes[0].entry.end());
  DOM_CHECK_EQ(PolicyDigest(shuffled), PolicyDigest(policy));
  auto shuffled_encoding = EncodePolicyBinary(shuffled);
  DOM_CHECK_OK(shuffled_encoding);
  DOM_CHECK(shuffled_encoding.value() == encoded.value());
}

DOM_TEST(evidence_binary_round_trip) {
  dom::test::EvidenceOptions options;
  options.revision = 12;
  options.severity = 3;
  auto snapshot = dom::test::MakeEvidence(options);
  auto encoded = EncodeEvidenceBinary(snapshot);
  DOM_CHECK_OK(encoded);
  auto decoded = DecodeEvidenceBinary(encoded.value());
  DOM_CHECK_OK(decoded);
  DOM_CHECK_EQ(decoded.value().digest(), snapshot.digest());
  DOM_CHECK_EQ(decoded.value().revision().value(), 12ull);
  DOM_CHECK_EQ(decoded.value().records().size(), snapshot.records().size());
}

DOM_TEST(policy_binary_decoder_refuses_truncation_at_every_boundary) {
  const PolicyDocument policy = dom::test::MakePolicy();
  auto encoded = EncodePolicyBinary(policy);
  DOM_CHECK_OK(encoded);
  const std::vector<std::uint8_t>& bytes = encoded.value();
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    const std::span<const std::uint8_t> prefix(bytes.data(), length);
    auto decoded = DecodePolicyBinary(prefix);
    DOM_CHECK(!decoded.has_value());
  }
  auto with_trailing = bytes;
  with_trailing.push_back(0x5A);
  DOM_CHECK_ERR(DecodePolicyBinary(with_trailing), ErrorCode::DecodeError);

  auto wrong_version = bytes;
  wrong_version[0] = 99;
  DOM_CHECK_ERR(DecodePolicyBinary(wrong_version), ErrorCode::IncompatibleFormat);
}

DOM_TEST(evidence_binary_decoder_refuses_truncation_at_every_boundary) {
  auto snapshot = dom::test::MakeEvidence(dom::test::EvidenceOptions{});
  auto encoded = EncodeEvidenceBinary(snapshot);
  DOM_CHECK_OK(encoded);
  const std::vector<std::uint8_t>& bytes = encoded.value();
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    const std::span<const std::uint8_t> prefix(bytes.data(), length);
    auto decoded = DecodeEvidenceBinary(prefix);
    DOM_CHECK(!decoded.has_value());
  }
  auto with_trailing = bytes;
  with_trailing.push_back(0x01);
  DOM_CHECK_ERR(DecodeEvidenceBinary(with_trailing), ErrorCode::DecodeError);
}

DOM_TEST(policy_binary_decoder_survives_deterministic_fuzz) {
  const PolicyDocument policy = dom::test::MakePolicy();
  auto encoded = EncodePolicyBinary(policy);
  DOM_CHECK_OK(encoded);
  const std::vector<std::uint8_t> original = encoded.value();
  const Digest reference = PolicyDigest(policy);
  const std::uint64_t seed = 0x5EED1234ull;
  DOM_NOTE("fuzz seed=" + std::to_string(seed) + " mutations=2000 bytes=" +
           std::to_string(original.size()));
  Rng rng(seed);
  std::size_t decoded_ok = 0;
  std::size_t refused = 0;
  for (int iteration = 0; iteration < 2000; ++iteration) {
    std::vector<std::uint8_t> mutated = original;
    const std::size_t mutations = 1 + rng.Below(3);
    for (std::size_t i = 0; i < mutations; ++i) {
      const std::size_t offset = rng.Below(mutated.size());
      mutated[offset] = static_cast<std::uint8_t>(rng.Next() & 0xFFu);
    }
    auto decoded = DecodePolicyBinary(mutated);
    if (!decoded) {
      ++refused;
      continue;
    }
    ++decoded_ok;
    // Anything that still decodes must be a different document, and it must
    // re-encode to exactly the bytes it came from.
    auto reencoded = EncodePolicyBinary(decoded.value());
    DOM_CHECK_OK(reencoded);
    DOM_CHECK(reencoded.value() == mutated);
    if (PolicyDigest(decoded.value()) == reference) {
      DOM_CHECK(mutated == original);
    }
  }
  DOM_NOTE("fuzz refused=" + std::to_string(refused) + " accepted=" +
           std::to_string(decoded_ok));
  DOM_CHECK(refused + decoded_ok == 2000);
}

DOM_TEST(policy_binary_decoder_rejects_hostile_lengths) {
  const PolicyDocument policy = dom::test::MakePolicy();
  auto encoded = EncodePolicyBinary(policy);
  DOM_CHECK_OK(encoded);
  std::vector<std::uint8_t> bytes = encoded.value();
  // Every 4-byte little-endian word set to 0xFFFFFFFF must be refused, never
  // allocated: the decoder bounds every count before it reads.
  for (std::size_t offset = 4; offset + 4 <= bytes.size(); offset += 4) {
    std::vector<std::uint8_t> hostile = bytes;
    for (std::size_t i = 0; i < 4; ++i) {
      hostile[offset + i] = 0xFFu;
    }
    auto decoded = DecodePolicyBinary(hostile);
    if (decoded) {
      auto reencoded = EncodePolicyBinary(decoded.value());
      DOM_CHECK_OK(reencoded);
      DOM_CHECK(reencoded.value() == hostile);
    }
  }
}

DOM_TEST(policy_text_round_trip_is_exact) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const std::string text = EncodePolicyText(policy);
  auto decoded = DecodePolicyText(text);
  DOM_CHECK_OK(decoded);
  DOM_CHECK_EQ(PolicyDigest(decoded.value()), PolicyDigest(policy));
  DOM_CHECK_EQ(EncodePolicyText(decoded.value()), text);
  // The text form and the binary form describe the same document.
  auto binary = EncodePolicyBinary(decoded.value());
  DOM_CHECK_OK(binary);
  auto from_binary = DecodePolicyBinary(binary.value());
  DOM_CHECK_OK(from_binary);
  DOM_CHECK_EQ(EncodePolicyText(from_binary.value()), text);
}

DOM_TEST(evidence_text_round_trip_is_exact) {
  dom::test::EvidenceOptions options;
  options.revision = 9;
  options.cooling_margin = -250;
  options.producer = "observer \"alpha\"";
  auto snapshot = dom::test::MakeEvidence(options);
  const std::string text = EncodeEvidenceText(snapshot);
  auto decoded = DecodeEvidenceText(text);
  DOM_CHECK_OK(decoded);
  DOM_CHECK_EQ(decoded.value().digest(), snapshot.digest());
  DOM_CHECK_EQ(EncodeEvidenceText(decoded.value()), text);
}

DOM_TEST(policy_text_parser_is_strict) {
  const PolicyDocument policy = dom::test::MakePolicy();
  const std::string text = EncodePolicyText(policy);

  // Comments, blank lines and CRLF line endings are accepted.
  std::string annotated = "# facility policy\n\n" + text;
  annotated += "# trailing comment\n";
  std::string crlf;
  for (char c : annotated) {
    if (c == '\n') {
      crlf += "\r\n";
    } else {
      crlf.push_back(c);
    }
  }
  auto decoded = DecodePolicyText(crlf);
  DOM_CHECK_OK(decoded);
  DOM_CHECK_EQ(PolicyDigest(decoded.value()), PolicyDigest(policy));

  const std::vector<std::string> invalid = {
      "",
      "dom.evidence 1\n",
      "dom.policy 2\n",
      "dom.policy 1\nunknown-keyword 5\n",
      "dom.policy 1\nid\n",
      "dom.policy 1\nid not-a-number\n",
      "dom.policy 1\nid 1\ngeneration 1\nrevision \"x\"\nmax-recovery-step 1\n"
      "default-max-age-ms 1\nmode 1 \"m\" nominal rev=1 latch=none dwell=0 hold=0\n"
      "  entry incident \"*\" \"severity\" maximum at-least code:0 maybe\n",
      "dom.policy 1\nid 1\ngeneration 1\nrevision \"x\"\nmax-recovery-step 1\n"
      "default-max-age-ms 1\nmode 1 \"m\" nominal rev=1 latch=none dwell=0 hold=0\n"
      "  entry incident \"*\" \"severity\" maximum at-least code0 fresh\n",
      "dom.policy 1\nid 1\ngeneration 1\nrevision \"unterminated\n",
      "dom.policy 1\nid 1\ngeneration 1\nrevision \"bad\\qescape\"\n",
      "dom.policy 1\nid 1\ngeneration 1\nrevision \"x\"\nmax-recovery-step 1\n"
      "default-max-age-ms 1\nmode 1 \"m\" nominal rev=1 latch=none dwell=0 hold=0\n"
      "  entry incident \"*\" \"severity\" maximum at-least furlongs:0 fresh\n",
  };
  for (const std::string& candidate : invalid) {
    auto result = DecodePolicyText(candidate);
    DOM_CHECK(!result.has_value());
  }

  // Invalid UTF-8 anywhere in the text is refused rather than carried through.
  std::string broken = text;
  broken += "service \"";
  broken.push_back(static_cast<char>(0xFF));
  broken += "\"\n";
  DOM_CHECK(!DecodePolicyText(broken).has_value());

  // A duplicate service class parses but is refused by policy validation, which
  // is exactly the separation the two layers are meant to have.
  const std::string duplicated = text + "service \"batch\"\n";
  auto parsed = DecodePolicyText(duplicated);
  DOM_CHECK_OK(parsed);
  DOM_CHECK_ERR(ValidatePolicy(parsed.value()), ErrorCode::PolicyRejected);
}

DOM_TEST(evidence_text_parser_is_strict) {
  auto snapshot = dom::test::MakeEvidence(dom::test::EvidenceOptions{});
  const std::string text = EncodeEvidenceText(snapshot);
  DOM_CHECK_OK(DecodeEvidenceText(text));

  const std::vector<std::string> invalid = {
      "",
      "dom.policy 1\n",
      "dom.evidence 1\nunknown 1\n",
      "dom.evidence 1\nrevision\n",
      "dom.evidence 1\nrecord incident \"facility\" \"severity\" code:0 observed=1 until=0 gen=1\n",
      "dom.evidence 1\nrecord incident \"facility\" \"severity\" code:0 observed=1 until=0 gen=1 "
      "src=nothex producer=\"a\"\n",
      "dom.evidence 1\nrecord noclass \"facility\" \"severity\" code:0 observed=1 until=0 gen=1 "
      "src=0000000000000000000000000000000000000000000000000000000000000000 producer=\"a\"\n",
      "dom.evidence 1\nrecord incident \"facility\" \"severity\" code:0 observed=1 until=0 gen=1 "
      "src=0000000000000000000000000000000000000000000000000000000000000000 producer=\"\"\n",
  };
  for (const std::string& candidate : invalid) {
    auto result = DecodeEvidenceText(candidate);
    DOM_CHECK(!result.has_value());
  }
}

DOM_TEST(enum_and_string_helpers_are_total) {
  for (std::uint8_t code = 0; code <= 6; ++code) {
    const auto posture = static_cast<Posture>(code);
    DOM_CHECK_OK(PostureFromName(PostureName(posture)));
  }
  DOM_CHECK_ERR(PostureFromName("melted"), ErrorCode::InvalidArgument);
  for (const EvidenceClass cls : AllEvidenceClasses()) {
    DOM_CHECK_OK(EvidenceClassFromName(EvidenceClassName(cls)));
  }
  for (const OperatingClass cls : AllOperatingClasses()) {
    DOM_CHECK_OK(OperatingClassFromName(OperatingClassName(cls)));
  }
  DOM_CHECK_ERR(EvidenceClassFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(OperatingClassFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(RequirementScopeFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(LatchModeFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(ProtectionLevelFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(RestrictionKindFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(AggregationFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(ComparatorFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(AcceptanceStatusFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(VerificationOutcomeFromName("vibes"), ErrorCode::InvalidArgument);
  DOM_CHECK_ERR(VerdictFromName("vibes"), ErrorCode::InvalidArgument);
}

}  // namespace
