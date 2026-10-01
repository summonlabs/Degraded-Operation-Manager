// Degraded Operation Manager - test harness implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "testkit/testkit.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace dom::test {
namespace {

std::size_t& FailureCount() {
  static std::size_t count = 0;
  return count;
}

std::size_t& CheckCount() {
  static std::size_t count = 0;
  return count;
}

bool& Failing() {
  static bool failing = false;
  return failing;
}

}  // namespace

std::vector<TestCase>& Cases() {
  static std::vector<TestCase> cases;
  return cases;
}

int RegisterTest(const std::string& name, Body body) {
  TestCase test;
  test.name = name;
  test.body = std::move(body);
  Cases().push_back(std::move(test));
  return 0;
}

void Fail(const std::string& message, const char* file, int line) {
  ++FailureCount();
  std::cout << "FAIL " << file << ":" << line << ": " << message << "\n" << std::flush;
}

void Note(const std::string& message) {
  std::cout << "note " << message << "\n" << std::flush;
}

void ExpectOk(const Status& status, const char* expression, const char* file, int line) {
  ++CheckCount();
  if (!status.ok()) {
    Fail(std::string(expression) + " failed: " + status.ToString(), file, line);
    throw CaseAborted{};
  }
}

void ExpectErr(const Status& status, ErrorCode code, const char* expression, const char* file,
               int line) {
  ++CheckCount();
  if (status.ok()) {
    Fail(std::string(expression) + " unexpectedly succeeded", file, line);
    return;
  }
  if (status.code() != code) {
    Fail(std::string(expression) + " failed with " + ErrorCodeName(status.code()) +
             " instead of " + ErrorCodeName(code) + ": " + status.message(),
         file, line);
  }
}

int RunAll(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    }
  }
  std::size_t executed = 0;
  for (const TestCase& test : Cases()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    const std::size_t before = FailureCount();
    std::cout << "[ RUN  ] " << test.name << "\n" << std::flush;
    try {
      test.body();
    } catch (const CaseAborted&) {
      // The case already recorded why it stopped.
    } catch (const std::exception& error) {
      Fail(std::string("unhandled exception: ") + error.what(), __FILE__, __LINE__);
    } catch (...) {
      Fail("unhandled non-standard exception", __FILE__, __LINE__);
    }
    const bool failed = FailureCount() != before;
    Failing() = Failing() || failed;
    std::cout << (failed ? "[ FAIL ] " : "[  OK  ] ") << test.name << "\n" << std::flush;
  }
  std::cout << "cases=" << executed << " checks=" << CheckCount()
            << " failures=" << FailureCount() << "\n";
  if (executed == 0) {
    std::cout << "no test case matched the filter\n";
    return 2;
  }
  return Failing() ? 1 : 0;
}

}  // namespace dom::test

int main(int argc, char** argv) { return dom::test::RunAll(argc, argv); }
