// Fabric Capability Registry test support: temporary directories.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <atomic>
#include <filesystem>
#include <random>
#include <string>

#include "test_framework.hpp"

namespace fcr::test {

/// RAII temporary directory under the system temp path. The directory and its
/// contents are removed on destruction, including when a case aborts.
class TempDir {
 public:
  explicit TempDir(const std::string& name) {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
      throw TestAbort("temporary directory is unavailable: " + error.message());
    }
    path_ = base / ("fcr-" + name + "-" + unique_suffix());

    std::error_code removal_error;
    std::filesystem::remove_all(path_, removal_error);
    std::error_code creation_error;
    if (!std::filesystem::create_directories(path_, creation_error) || creation_error) {
      // A directory that could not be made clean is never used silently: a stale store
      // would corrupt the proof and surface far away from its real cause.
      const std::string detail =
          creation_error ? creation_error.message()
                         : (removal_error ? removal_error.message()
                                          : std::string("the path already exists"));
      throw TestAbort("temporary directory " + path_.string() +
                      " could not be made clean for this run: " + detail);
    }
  }

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string string() const { return path_.string(); }

 private:
  /// A suffix that is unique across processes as well as inside one. A per-process
  /// counter alone is not enough: it makes the first directory of every process share a
  /// single path, so two suites running at the same time -- Release beside Debug, or two
  /// shards on one agent -- would share a store and corrupt each other's proofs, and a
  /// directory left behind by a killed run would be silently reused.
  static std::string unique_suffix() {
    static const unsigned long long process_seed =
        static_cast<unsigned long long>(std::random_device{}());
    static std::atomic<unsigned long long> counter{0};
    return std::to_string(process_seed) + "-" + std::to_string(++counter);
  }

  std::filesystem::path path_;
};

}  // namespace fcr::test
