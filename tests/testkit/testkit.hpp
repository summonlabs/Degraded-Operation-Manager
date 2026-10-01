// Degraded Operation Manager - test harness.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A deliberately small harness: named cases, recorded failures with file and
// line, and a non-zero exit status when anything fails. Randomized cases print
// their seed so a failure is reproducible.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include "dom/dom.hpp"

namespace dom::test {

using Body = std::function<void()>;

struct TestCase {
  std::string name;
  Body body;
};

/// Thrown when a required precondition check fails, so the rest of the case
/// does not run against a value that is not there.
struct CaseAborted {};

std::vector<TestCase>& Cases();

int RegisterTest(const std::string& name, Body body);
int RunAll(int argc, char** argv);

void Fail(const std::string& message, const char* file, int line);
/// Prints a diagnostics line that is always shown.
void Note(const std::string& message);

/// Renders a value for a failure message. A single template keeps the
/// overload set unambiguous for every integer width and enum used by the tests.
template <class T>
std::string Describe(const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    return std::string(value);
  } else if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, char*>) {
    return std::string(value);
  } else if constexpr (std::is_same_v<T, Digest>) {
    return value.ToHex();
  } else if constexpr (std::is_same_v<T, Status>) {
    return value.ToString();
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else {
    return std::string("<value>");
  }
}

void ExpectOk(const Status& status, const char* expression, const char* file, int line);
template <class T>
void ExpectOk(const Result<T>& result, const char* expression, const char* file, int line) {
  if (!result.has_value()) {
    Fail(std::string(expression) + " failed: " + result.status().ToString(), file, line);
    throw CaseAborted{};
  }
}
void ExpectErr(const Status& status, ErrorCode code, const char* expression, const char* file,
               int line);
template <class T>
void ExpectErr(const Result<T>& result, ErrorCode code, const char* expression, const char* file,
               int line) {
  if (result.has_value()) {
    Fail(std::string(expression) + " unexpectedly succeeded", file, line);
    return;
  }
  if (result.status().code() != code) {
    Fail(std::string(expression) + " failed with " + ErrorCodeName(result.status().code()) +
             " instead of " + ErrorCodeName(code) + ": " + result.status().message(),
         file, line);
  }
}

}  // namespace dom::test

#define DOM_TEST(name)                                                          \
  static void name();                                                           \
  static const int name##_registered = ::dom::test::RegisterTest(#name, name);   \
  static void name()

#define DOM_CHECK(expression)                                                     \
  do {                                                                            \
    if (!(expression)) {                                                          \
      ::dom::test::Fail(std::string("expected: ") + #expression, __FILE__, __LINE__); \
    }                                                                             \
  } while (false)

#define DOM_CHECK_EQ(lhs, rhs)                                                          \
  do {                                                                                  \
    /* Copied, never bound by reference: a reference into a temporary Result would       \
       dangle before the comparison runs, which AddressSanitizer caught. */              \
    const auto lhs_value = (lhs);                                                       \
    const auto rhs_value = (rhs);                                                       \
    if (!(lhs_value == rhs_value)) {                                                    \
      ::dom::test::Fail(std::string(#lhs) + " == " + #rhs + " (" +                      \
                            ::dom::test::Describe(lhs_value) + " vs " +                 \
                            ::dom::test::Describe(rhs_value) + ")",                     \
                        __FILE__, __LINE__);                                            \
    }                                                                                   \
  } while (false)

#define DOM_CHECK_OK(expression) \
  ::dom::test::ExpectOk((expression), #expression, __FILE__, __LINE__)

#define DOM_CHECK_ERR(expression, code) \
  ::dom::test::ExpectErr((expression), (code), #expression, __FILE__, __LINE__)

#define DOM_NOTE(message) ::dom::test::Note(message)
