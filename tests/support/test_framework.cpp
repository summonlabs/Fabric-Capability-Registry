// Fabric Capability Registry test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace fcr::test {
namespace {
std::size_t g_checks = 0;
std::size_t g_failures = 0;
std::string g_current_case;
std::string g_current_suite;
}  // namespace

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

Registrar::Registrar(const char* suite, const char* name, void (*body)()) {
  Registry().push_back(TestCase{suite, name, body});
}

void ReportFailure(const char* file, int line, std::string_view message) {
  ++g_failures;
  std::fprintf(stderr, "FAIL %s.%s\n  %s:%d\n  %.*s\n", g_current_suite.c_str(),
               g_current_case.c_str(), file, line, static_cast<int>(message.size()),
               message.data());
  std::fflush(stderr);
}

void ReportNote(std::string_view message) {
  std::fprintf(stdout, "NOTE %s.%s: %.*s\n", g_current_suite.c_str(), g_current_case.c_str(),
               static_cast<int>(message.size()), message.data());
  std::fflush(stdout);
}

std::size_t ChecksRun() { return g_checks; }
std::size_t FailuresSeen() { return g_failures; }

namespace {
bool Matches(const TestCase& test, std::string_view filter) {
  if (filter.empty()) return true;
  const std::string full = test.suite + "." + test.name;
  if (filter.size() > full.size()) return false;
  return full.compare(0, filter.size(), filter) == 0;
}
}  // namespace

int RunAll(std::string_view filter) {
  std::size_t ran = 0;
  std::size_t failed_cases = 0;
  for (const TestCase& test : Registry()) {
    if (!Matches(test, filter)) continue;
    g_current_suite = test.suite;
    g_current_case = test.name;
    const std::size_t before = g_failures;
    std::printf("RUN  %s.%s\n", test.suite.c_str(), test.name.c_str());
    std::fflush(stdout);
    try {
      test.body();
    } catch (const TestAbort& abort) {
      std::fprintf(stderr, "ABORT %s.%s: %s\n", test.suite.c_str(), test.name.c_str(),
                   abort.what());
      ++g_failures;
    } catch (const std::exception& error) {
      std::fprintf(stderr, "UNEXPECTED EXCEPTION %s.%s: %s\n", test.suite.c_str(),
                   test.name.c_str(), error.what());
      ++g_failures;
    } catch (...) {
      std::fprintf(stderr, "UNEXPECTED EXCEPTION %s.%s: unknown\n", test.suite.c_str(),
                   test.name.c_str());
      ++g_failures;
    }
    ++ran;
    if (g_failures != before) {
      ++failed_cases;
      std::printf("FAILED %s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    std::fflush(stdout);
  }
  std::printf("\nsummary: cases=%zu failed_cases=%zu failures=%zu\n", ran, failed_cases,
              g_failures - 0);
  std::fflush(stdout);
  if (ran == 0) {
    std::fprintf(stderr, "no test case matched filter '%.*s'\n", static_cast<int>(filter.size()),
                 filter.data());
    return 2;
  }
  return g_failures == 0 ? 0 : 1;
}

std::string Describe(bool value) { return value ? "true" : "false"; }
std::string Describe(int value) { return std::to_string(value); }
std::string Describe(long long value) { return std::to_string(value); }
std::string Describe(unsigned value) { return std::to_string(value); }
std::string Describe(unsigned long long value) { return std::to_string(value); }
std::string Describe(const std::string& value) { return "\"" + value + "\""; }
std::string Describe(std::string_view value) { return "\"" + std::string(value) + "\""; }
std::string Describe(const char* value) { return value == nullptr ? "<null>" : std::string(value); }

}  // namespace fcr::test

int main(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (arg.rfind("--filter=", 0) == 0) {
      filter = std::string(arg.substr(9));
    } else if (arg == "--list") {
      for (const auto& test : ::fcr::test::Registry()) {
        std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
      }
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %.*s\n", static_cast<int>(arg.size()), arg.data());
      return 2;
    }
  }
  return ::fcr::test::RunAll(filter);
}
