// Degraded Operation Manager - build identity.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

namespace dom {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

/// Product identity as it appears in tools, reports and durable state.
inline constexpr std::string_view kProductName = "Degraded Operation Manager";
inline constexpr std::string_view kProductShortName = "dom";
inline constexpr std::string_view kVersionString = "1.0.0";

/// Durable store identity. The format version is written into every file and a
/// reader refuses any other value instead of guessing.
inline constexpr std::string_view kStoreFormatName = "dom-state";
inline constexpr std::uint16_t kStoreFormatVersion = 1;

inline std::string_view VersionString() { return kVersionString; }

}  // namespace dom
