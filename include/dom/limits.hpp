// Degraded Operation Manager - explicit resource bounds.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace dom::limits {

/// Every externally influenced structure is bounded. Decoders refuse input that
/// exceeds these bounds instead of allocating whatever the input claims.
inline constexpr std::uint32_t kMaxStringBytes = 4096;
inline constexpr std::uint32_t kMaxCollectionItems = 65536;
inline constexpr std::uint32_t kMaxModes = 1024;
inline constexpr std::uint32_t kMaxEvidenceRecords = 200000;
inline constexpr std::uint32_t kMaxPredicatesPerMode = 256;
inline constexpr std::uint32_t kMaxRestrictionsPerMode = 512;
inline constexpr std::uint32_t kMaxObligations = 1024;
inline constexpr std::uint32_t kMaxServiceClasses = 1024;
inline constexpr std::uint64_t kMaxEnvelopeBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint32_t kMaxHistoryEntries = 65536;
inline constexpr std::uint32_t kMaxIdempotencyEntries = 4096;
inline constexpr std::uint32_t kMaxAuthorityRecords = 8192;

}  // namespace dom::limits
