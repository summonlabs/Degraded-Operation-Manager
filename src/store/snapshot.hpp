// Degraded Operation Manager - snapshot file format (private header).
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Layout (all integers little endian):
//   [ 0,  8) magic "DOMSNAP1"
//   [ 8, 10) format version (u16)
//   [10, 12) reserved, must be zero
//   [12, 20) state sequence (u64)
//   [20, 28) payload length (u64)
//   [28, 60) digest of the previous published generation (32 bytes)
//   [60, 92) payload digest (32 bytes)
//   [92, 92+len) payload
//   [92+len, 124+len) SHA-256 over header and payload
// A file whose length does not match exactly, or whose digests do not verify,
// is refused; it is never partially applied.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "dom/ids.hpp"
#include "dom/status.hpp"
#include "dom/store.hpp"

namespace dom::internal {

inline constexpr std::size_t kSnapshotHeaderSize = 92;
inline constexpr std::size_t kSnapshotFooterSize = 32;

Result<std::vector<std::uint8_t>> EncodeSnapshotFile(const PersistedState& state);

/// Decodes and fully verifies a snapshot file. The file sequence is returned so
/// the caller can compare it with the payload's own sequence.
Result<PersistedState> DecodeSnapshotFile(std::span<const std::uint8_t> bytes,
                                          StateSequence& file_sequence);

/// Digest of a published generation, used to verify the generation chain.
Result<Digest> SnapshotChainDigest(std::span<const std::uint8_t> bytes);

}  // namespace dom::internal
