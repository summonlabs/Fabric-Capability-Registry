// Fabric Capability Registry test support: real operating system processes.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fcr::test {

enum class ExitWait { Exited, StillRunning };

/// A real child process. Children are assigned to a kill-on-close job object
/// so that a crashed test can never leave an orphan behind, and the destructor
/// terminates and reaps a child that is still running.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Starts a process. Returns nothing when the executable cannot be started.
  static std::optional<ChildProcess> Start(const std::string& executable,
                                           const std::vector<std::string>& arguments,
                                           const std::string& working_directory = {});

  /// Starts a process whose stdout and stderr are captured through a pipe.
  /// Call ReadCaptured() after the child has exited.
  static std::optional<ChildProcess> StartCapturing(const std::string& executable,
                                                    const std::vector<std::string>& arguments,
                                                    const std::string& working_directory = {});

  /// Starts a process with a private stdin pipe that stays open for as long as this object
  /// lives, so a server child does not observe an immediate end of input and shut itself
  /// down. Use WriteStdin() to request a graceful stop.
  static std::optional<ChildProcess> StartWithStdinControl(
      const std::string& executable, const std::vector<std::string>& arguments,
      const std::string& working_directory = {});

  bool Valid() const noexcept { return process_ != nullptr; }
  unsigned long Pid() const noexcept { return pid_; }
  bool Running() const;

  /// Blocks until the child exits. Only used where protocol progress already
  /// guarantees termination.
  unsigned long WaitForExit();
  /// Bounded wait used by negative tests: a still-running child is a defect
  /// that the test reports, never a silent skip.
  ExitWait WaitBounded(unsigned long milliseconds);
  /// Hard terminate (TerminateProcess) followed by a reap. This is a real
  /// process kill, not a cooperative shutdown.
  void Kill();
  /// Cooperative close of handles without terminating.
  void CloseHandles();

  /// Reads everything the captured child wrote. Only valid for a child started
  /// with StartCapturing() and only after it exited.
  std::string ReadCaptured();

  /// Writes to the child stdin pipe created by StartWithStdinControl().
  bool WriteStdin(const std::string& text);
  /// Closes the child stdin pipe, which a serving child reads as an orderly stop request.
  void CloseStdin();

 private:
  void Release();

  void* process_ = nullptr;     // HANDLE
  void* capture_ = nullptr;     // HANDLE: read end of the capture pipe
  void* stdin_write_ = nullptr; // HANDLE: write end of the child stdin pipe
  unsigned long pid_ = 0;
};

/// Absolute paths of the distributed test executables. Overridable at run time
/// through FCR_COORDINATOR_EXE and FCR_PUBLISHER_EXE.
std::string CoordinatorExecutablePath();
std::string PublisherExecutablePath();
/// Absolute path of the fcrctl inspection CLI for CLI level tests.
std::string CliExecutablePath();

/// Picks a currently unused loopback TCP port. The socket is closed before the
/// port is returned.
std::uint16_t FindFreeLoopbackPort();

/// True when the process with the given pid is still alive.
bool ProcessAlive(unsigned long pid);

}  // namespace fcr::test
