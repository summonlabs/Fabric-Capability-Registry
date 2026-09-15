// Fabric Capability Registry test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic, dependency free, timeout free test harness. A hanging test is
// a defect: no watchdog, alarm or timeout is used anywhere.

#pragma once

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace fcr::test {

struct TestCase {
  std::string suite;
  std::string name;
  void (*body)();
};

std::vector<TestCase>& Registry();

struct Registrar {
  Registrar(const char* suite, const char* name, void (*body)());
};

/// Aborts the current test case without aborting the process.
class TestAbort : public std::exception {
 public:
  explicit TestAbort(std::string message) : message_(std::move(message)) {}
  const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

void ReportFailure(const char* file, int line, std::string_view message);
void ReportNote(std::string_view message);

std::size_t ChecksRun();
std::size_t FailuresSeen();

/// Runs every registered case whose suite or "suite.case" matches the filter.
/// An empty filter runs everything. Returns the process exit code.
int RunAll(std::string_view filter);

/// Deterministic pseudo random generator used by property and race tests so
/// that failures can be reproduced from a printed seed.
class DeterministicRng {
 public:
  explicit DeterministicRng(std::uint64_t seed) : state_(seed * 6364136223846793005ull + 1) {}

  std::uint64_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  std::uint32_t NextU32() { return static_cast<std::uint32_t>(Next() >> 32); }
  std::uint64_t NextBelow(std::uint64_t bound) { return bound == 0 ? 0 : Next() % bound; }
  std::uint64_t Seed() const { return seed_; }

 private:
  std::uint64_t state_;
  std::uint64_t seed_ = 0;
};

}  // namespace fcr::test

#define FCR_TEST(suite_name, case_name)                                            \
  static void fcr_test_##suite_name##_##case_name();                               \
  static const ::fcr::test::Registrar fcr_registrar_##suite_name##_##case_name(    \
      #suite_name, #case_name, &fcr_test_##suite_name##_##case_name);              \
  static void fcr_test_##suite_name##_##case_name()

#define FCR_FAIL(message) ::fcr::test::ReportFailure(__FILE__, __LINE__, (message))

#define FCR_CHECK(condition)                                                       \
  do {                                                                             \
    if (!(condition)) {                                                            \
      ::fcr::test::ReportFailure(__FILE__, __LINE__, "check failed: " #condition); \
    }                                                                              \
  } while (false)

#define FCR_REQUIRE(condition)                                                     \
  do {                                                                             \
    if (!(condition)) {                                                            \
      ::fcr::test::ReportFailure(__FILE__, __LINE__, "require failed: " #condition); \
      throw ::fcr::test::TestAbort("require failed: " #condition);                 \
    }                                                                              \
  } while (false)

#define FCR_CHECK_EQ(actual, expected)                                             \
  do {                                                                             \
    const auto& fcr_a = (actual);                                                  \
    const auto& fcr_b = (expected);                                                \
    if (!(fcr_a == fcr_b)) {                                                       \
      ::fcr::test::ReportFailure(__FILE__, __LINE__,                               \
                                 std::string("expected ") + #actual + " == " +     \
                                     #expected + " (" + ::fcr::test::Describe(fcr_a) + \
                                     " vs " + ::fcr::test::Describe(fcr_b) + ")");  \
    }                                                                              \
  } while (false)

#define FCR_CHECK_NE(actual, unexpected)                                           \
  do {                                                                             \
    const auto& fcr_a = (actual);                                                  \
    const auto& fcr_b = (unexpected);                                              \
    if (fcr_a == fcr_b) {                                                          \
      ::fcr::test::ReportFailure(__FILE__, __LINE__,                               \
                                 std::string("expected ") + #actual + " != " +     \
                                     #unexpected + " (" + ::fcr::test::Describe(fcr_a) + ")"); \
    }                                                                              \
  } while (false)

/// Asserts that an Outcome carries a value; returns nothing but aborts the
/// case when it does not.
#define FCR_REQUIRE_OK(expression)                                                 \
  do {                                                                             \
    const auto& fcr_outcome = (expression);                                        \
    if (!fcr_outcome.HasValue()) {                                                 \
      ::fcr::test::ReportFailure(__FILE__, __LINE__,                               \
                                 std::string(#expression " failed: ") +            \
                                     fcr_outcome.ErrorString());                   \
      throw ::fcr::test::TestAbort(#expression " failed");                         \
    }                                                                              \
  } while (false)

/// Asserts that an Outcome carries a value without aborting the case.
#define FCR_CHECK_OK(expression)                                                   \
  do {                                                                             \
    const auto& fcr_outcome = (expression);                                        \
    if (!fcr_outcome.HasValue()) {                                                 \
      ::fcr::test::ReportFailure(__FILE__, __LINE__,                               \
                                 std::string(#expression " failed: ") +            \
                                     fcr_outcome.ErrorString());                   \
    }                                                                              \
  } while (false)

/// Asserts that an Outcome failed with a specific error code.
#define FCR_REQUIRE_CODE(expression, expected_code)                                \
  do {                                                                             \
    const auto& fcr_outcome = (expression);                                        \
    if (fcr_outcome.HasValue()) {                                                  \
      ::fcr::test::ReportFailure(__FILE__, __LINE__,                               \
                                 std::string(#expression " unexpectedly succeeded")); \
      throw ::fcr::test::TestAbort(#expression " unexpectedly succeeded");         \
    }                                                                              \
    if (fcr_outcome.Code() != (expected_code)) {                                   \
      ::fcr::test::ReportFailure(__FILE__, __LINE__,                               \
                                 std::string(#expression " failed with ") +        \
                                     std::string(::fabric::capability::ErrorCodeName(fcr_outcome.Code())) + \
                                     " instead of " +                               \
                                     std::string(::fabric::capability::ErrorCodeName(  \
                                         expected_code)));                          \
      throw ::fcr::test::TestAbort(#expression " wrong error code");               \
    }                                                                              \
  } while (false)

#define FCR_CHECK_CODE(expression, expected_code)                                  \
  do {                                                                             \
    const auto& fcr_outcome = (expression);                                        \
    if (fcr_outcome.HasValue()) {                                                  \
      ::fcr::test::ReportFailure(__FILE__, __LINE__,                               \
                                 std::string(#expression " unexpectedly succeeded")); \
    } else if (fcr_outcome.Code() != (expected_code)) {                            \
      ::fcr::test::ReportFailure(__FILE__, __LINE__,                               \
                                 std::string(#expression " failed with ") +        \
                                     std::string(::fabric::capability::ErrorCodeName(fcr_outcome.Code())) + \
                                     " instead of " +                               \
                                     std::string(::fabric::capability::ErrorCodeName(  \
                                         expected_code)));                          \
    }                                                                              \
  } while (false)

namespace fcr::test {

// Describe() renders values that the framework knows about; unknown types fall
// back to a placeholder so that check macros always compile.
template <class T>
std::string Describe(const T&) {
  return "<value>";
}
std::string Describe(bool value);
std::string Describe(int value);
std::string Describe(long long value);
std::string Describe(unsigned value);
std::string Describe(unsigned long long value);
std::string Describe(const std::string& value);
std::string Describe(std::string_view value);
std::string Describe(const char* value);

}  // namespace fcr::test
