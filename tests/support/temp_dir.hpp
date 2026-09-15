// Fabric Capability Registry test support: temporary directories.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <filesystem>
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
    path_ = base / ("fcr-" + name + "-" + std::to_string(counter()));
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_, error);
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
  static unsigned counter() {
    static unsigned value = 0;
    return ++value;
  }

  std::filesystem::path path_;
};

}  // namespace fcr::test
