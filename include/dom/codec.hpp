// Degraded Operation Manager - canonical binary and text codecs.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dom/evidence.hpp"
#include "dom/policy.hpp"
#include "dom/status.hpp"

namespace dom {

/// Canonical binary encoding. Encoding rules, all enforced on decode:
///  - little-endian fixed-width integers;
///  - strings are length-prefixed UTF-8 without embedded NUL;
///  - sequences are length-prefixed in canonical order (modes by severity rank
///    then id, requirements and restrictions by id, records by key);
///  - enum codes must be known, digests are 32 raw bytes;
///  - decoding rejects truncated input, trailing bytes, unknown codes,
///    out-of-order collections, oversized lengths and invalid UTF-8.
Result<std::vector<std::uint8_t>> EncodePolicyBinary(const PolicyDocument& policy);
Result<PolicyDocument> DecodePolicyBinary(std::span<const std::uint8_t> bytes);

Result<std::vector<std::uint8_t>> EncodeEvidenceBinary(const EvidenceSnapshot& snapshot);
Result<EvidenceSnapshot> DecodeEvidenceBinary(std::span<const std::uint8_t> bytes);

/// Canonical text encoding: line oriented, deterministic, stable ordering, and
/// strict parsing (unknown keys, wrong arity and bad values are refused with a
/// line number). Round-trips exactly: EncodeText(DecodeText(x)) == x.
std::string EncodePolicyText(const PolicyDocument& policy);
Result<PolicyDocument> DecodePolicyText(std::string_view text);

std::string EncodeEvidenceText(const EvidenceSnapshot& snapshot);
Result<EvidenceSnapshot> DecodeEvidenceText(std::string_view text);

}  // namespace dom
