// Degraded Operation Manager - internal canonical payload codecs.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "dom/decision.hpp"
#include "dom/evidence.hpp"
#include "dom/policy.hpp"
#include "dom/store.hpp"

namespace dom::internal {

/// Canonical payload encoders. Every digest in the library is the SHA-256 of
/// one of these payloads, so identity, integrity and comparison all agree.
Status PutCommittedView(class BinaryWriter& writer, const CommittedView& committed);
Result<CommittedView> TakeCommittedView(class BinaryReader& reader);
Status PutRestrictionSet(class BinaryWriter& writer, const RestrictionSet& set);
Result<RestrictionSet> TakeRestrictionSet(class BinaryReader& reader);
Status PutReasonTrace(class BinaryWriter& writer, const ReasonTrace& trace);
Result<ReasonTrace> TakeReasonTrace(class BinaryReader& reader);
Status PutDecision(class BinaryWriter& writer, const ModeDecision& decision);
Result<ModeDecision> TakeDecision(class BinaryReader& reader);
Status PutHistoryEntry(class BinaryWriter& writer, const HistoryEntry& entry);
Result<HistoryEntry> TakeHistoryEntry(class BinaryReader& reader);
Status PutAuthorization(class BinaryWriter& writer, const RestrictionAuthorization& authorization);
Result<RestrictionAuthorization> TakeAuthorization(class BinaryReader& reader);
Status PutAcknowledgement(class BinaryWriter& writer, const AcknowledgementRecord& record);
Result<AcknowledgementRecord> TakeAcknowledgement(class BinaryReader& reader);
Status PutVerification(class BinaryWriter& writer, const EffectVerificationRecord& record);
Result<EffectVerificationRecord> TakeVerification(class BinaryReader& reader);
Status PutIdempotency(class BinaryWriter& writer, const IdempotencyEntry& entry);
Result<IdempotencyEntry> TakeIdempotency(class BinaryReader& reader);
Status PutGenerationVector(class BinaryWriter& writer, const GenerationVector& generations);
Result<GenerationVector> TakeGenerationVector(class BinaryReader& reader);

/// Full policy payload (canonicalised before writing).
Result<std::vector<std::uint8_t>> EncodePolicyPayload(const PolicyDocument& policy);
Result<PolicyDocument> DecodePolicyPayload(std::span<const std::uint8_t> bytes);

/// Evidence records payload with its revision.
Result<std::vector<std::uint8_t>> EncodeEvidencePayload(
    const std::vector<EvidenceRecord>& records, EvidenceRevision revision);
Result<std::vector<EvidenceRecord>> DecodeEvidencePayloadRecords(
    std::span<const std::uint8_t> bytes, EvidenceRevision& revision);

/// Authoritative state payload (everything except the framing header).
Result<std::vector<std::uint8_t>> EncodeStatePayload(const PersistedState& state);
Result<PersistedState> DecodeStatePayload(std::span<const std::uint8_t> bytes);

/// Ordering normalisation used by validation, encoding and comparison.
void CanonicalizePolicy(PolicyDocument& policy);

}  // namespace dom::internal
