// Degraded Operation Manager - shared command line support.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dom/dom.hpp"

namespace dom::tools {

/// Parsed command line: positional arguments plus --name value options.
struct Arguments {
  std::vector<std::string> positional;
  std::map<std::string, std::string> options;

  bool Has(const std::string& name) const { return options.find(name) != options.end(); }

  std::optional<std::string> Get(const std::string& name) const {
    const auto found = options.find(name);
    if (found == options.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::string GetOr(const std::string& name, const std::string& fallback) const {
    const auto found = options.find(name);
    return found == options.end() ? fallback : found->second;
  }
};

inline Result<std::uint64_t> ParseUnsigned(const std::string& text, const char* what) {
  if (text.empty()) {
    return Result<std::uint64_t>::Err(ErrorCode::InvalidArgument,
                                      std::string(what) + " must not be empty");
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return Result<std::uint64_t>::Err(ErrorCode::InvalidArgument,
                                        std::string(what) + " must be a decimal integer");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) {
      return Result<std::uint64_t>::Err(ErrorCode::Overflow,
                                        std::string(what) + " does not fit in 64 bits");
    }
    value = value * 10ull + digit;
  }
  return Result<std::uint64_t>::Ok(value);
}

inline Result<Arguments> ParseArguments(int argc, char** argv) {
  Arguments arguments;
  int index = 1;
  while (index < argc) {
    const std::string token = argv[index];
    if (token.size() > 2 && token.compare(0, 2, "--") == 0) {
      const std::string name = token.substr(2);
      if (index + 1 >= argc) {
        return Result<Arguments>::Err(ErrorCode::InvalidArgument,
                                      "option --" + name + " requires a value");
      }
      arguments.options[name] = argv[index + 1];
      index += 2;
      continue;
    }
    arguments.positional.push_back(token);
    ++index;
  }
  return Result<Arguments>::Ok(std::move(arguments));
}

inline Result<std::string> ReadWholeFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Result<std::string>::Err(ErrorCode::NotFound, "cannot open " + path);
  }
  std::string text;
  // The staging buffer lives on the heap: a 64 KB stack frame is a real
  // limitation on threads with small stacks.
  std::vector<char> buffer(16384);
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = stream.gcount();
    if (got <= 0) {
      break;
    }
    text.append(buffer.data(), static_cast<std::size_t>(got));
    if (text.size() > 64u * 1024u * 1024u) {
      return Result<std::string>::Err(ErrorCode::LimitExceeded, "input file is too large");
    }
  }
  return Result<std::string>::Ok(std::move(text));
}

inline Result<PolicyDocument> LoadPolicy(const std::string& path) {
  auto text = ReadWholeFile(path);
  if (!text) {
    return Result<PolicyDocument>::Err(text.status());
  }
  auto policy = DecodePolicyText(text.value());
  if (!policy) {
    return Result<PolicyDocument>::Err(policy.status());
  }
  Status validation = ValidatePolicy(policy.value());
  if (!validation.ok()) {
    return Result<PolicyDocument>::Err(validation);
  }
  return policy;
}

inline Result<EvidenceSnapshot> LoadEvidence(const std::string& path) {
  auto text = ReadWholeFile(path);
  if (!text) {
    return Result<EvidenceSnapshot>::Err(text.status());
  }
  return DecodeEvidenceText(text.value());
}

inline Result<Digest> LoadDigestOr(const std::optional<std::string>& text, const Digest& fallback) {
  if (!text.has_value()) {
    return Result<Digest>::Ok(fallback);
  }
  return Digest::FromHex(text.value());
}

/// Deterministic idempotency key derived from the operation name and its
/// canonical arguments, so replaying the same command line is idempotent while
/// a different command line is a different request.
inline Digest DerivedKey(const std::string& operation,
                         const std::vector<std::string>& parts) {
  Sha256 hasher;
  hasher.Update(std::string_view(operation));
  for (const std::string& part : parts) {
    hasher.Update(std::string_view("\x1f"));
    hasher.Update(std::string_view(part));
  }
  return hasher.Finish();
}

inline std::uint64_t NowMillis() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

inline int Fail(const Status& status) {
  std::cerr << "error: " << status.ToString() << "\n";
  return 2;
}

inline int Fail(const std::string& message) {
  std::cerr << "error: " << message << "\n";
  return 1;
}

}  // namespace dom::tools
