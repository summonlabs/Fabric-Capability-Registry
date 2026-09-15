// Fabric Capability Registry test support: real operating system processes.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "process_helper.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

namespace fcr::test {

#ifdef _WIN32
namespace {

std::wstring Widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

/// Quotes one argument following the CommandLineToArgvW rules.
std::wstring QuoteArgument(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t ch : argument) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

/// Kill-on-close job object so that a crashed test leaves no orphan process.
HANDLE JobHandle() {
  static HANDLE job = []() -> HANDLE {
    HANDLE created = CreateJobObjectW(nullptr, nullptr);
    if (created == nullptr) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits)) == 0) {
      CloseHandle(created);
      return nullptr;
    }
    return created;
  }();
  return job;
}

/// Reads an environment variable without the deprecated CRT accessors.
std::string ReadEnvironment(const char* name) {
  const std::wstring wide_name = Widen(name);
  const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (size == 0) return std::string();
  std::wstring value(static_cast<std::size_t>(size), L'\0');
  const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
  if (written == 0 || written >= size) return std::string();
  value.resize(written);
  const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                            nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<std::size_t>(utf8_size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), utf8.data(),
                      utf8_size, nullptr, nullptr);
  return utf8;
}

void EnsureWinsock() {
  static const bool initialised = []() {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)initialised;
}

}  // namespace
#endif

ChildProcess::~ChildProcess() { Release(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : process_(other.process_), capture_(other.capture_), pid_(other.pid_) {
  other.process_ = nullptr;
  other.capture_ = nullptr;
  other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    Release();
    process_ = other.process_;
    capture_ = other.capture_;
    pid_ = other.pid_;
    other.process_ = nullptr;
    other.capture_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

void ChildProcess::Release() {
  if (process_ != nullptr) {
    Kill();
  }
  if (capture_ != nullptr) {
#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(capture_));
#endif
    capture_ = nullptr;
  }
}

std::optional<ChildProcess> ChildProcess::Start(const std::string& executable,
                                                const std::vector<std::string>& arguments,
                                                const std::string& working_directory) {
#ifdef _WIN32
  std::wstring command = QuoteArgument(Widen(executable));
  for (const std::string& argument : arguments) {
    command.push_back(L' ');
    command.append(QuoteArgument(Widen(argument)));
  }
  std::wstring mutable_command = command;

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  const std::wstring directory = Widen(working_directory);
  const BOOL ok = CreateProcessW(Widen(executable).c_str(), mutable_command.data(), nullptr,
                                 nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 directory.empty() ? nullptr : directory.c_str(), &startup,
                                 &info);
  if (ok == 0) {
    return std::nullopt;
  }
  if (JobHandle() != nullptr) {
    AssignProcessToJobObject(JobHandle(), info.hProcess);
  }
  ChildProcess child;
  child.process_ = info.hProcess;
  child.pid_ = info.dwProcessId;
  CloseHandle(info.hThread);
  return child;
#else
  (void)executable;
  (void)arguments;
  (void)working_directory;
  return std::nullopt;
#endif
}

std::optional<ChildProcess> ChildProcess::StartCapturing(const std::string& executable,
                                                       const std::vector<std::string>& arguments,
                                                       const std::string& working_directory) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
    return std::nullopt;
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  std::wstring command = QuoteArgument(Widen(executable));
  for (const std::string& argument : arguments) {
    command.push_back(L' ');
    command.append(QuoteArgument(Widen(argument)));
  }
  std::wstring mutable_command = command;

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION info{};
  const std::wstring directory = Widen(working_directory);
  const BOOL ok = CreateProcessW(Widen(executable).c_str(), mutable_command.data(), nullptr,
                                 nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 directory.empty() ? nullptr : directory.c_str(), &startup,
                                 &info);
  CloseHandle(write_end);
  if (ok == 0) {
    CloseHandle(read_end);
    return std::nullopt;
  }
  if (JobHandle() != nullptr) {
    AssignProcessToJobObject(JobHandle(), info.hProcess);
  }
  ChildProcess child;
  child.process_ = info.hProcess;
  child.capture_ = read_end;
  child.pid_ = info.dwProcessId;
  CloseHandle(info.hThread);
  return child;
#else
  (void)executable;
  (void)arguments;
  (void)working_directory;
  return std::nullopt;
#endif
}

std::string ChildProcess::ReadCaptured() {
#ifdef _WIN32
  if (capture_ == nullptr) return std::string();
  std::string output;
  char buffer[4096];
  for (;;) {
    DWORD read = 0;
    if (ReadFile(static_cast<HANDLE>(capture_), buffer, sizeof(buffer), &read, nullptr) == 0 ||
        read == 0) {
      break;
    }
    output.append(buffer, read);
    if (output.size() > (1u << 22)) break;
  }
  CloseHandle(static_cast<HANDLE>(capture_));
  capture_ = nullptr;
  return output;
#else
  return std::string();
#endif
}

bool ChildProcess::Running() const {
#ifdef _WIN32
  if (process_ == nullptr) return false;
  return WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
  return false;
#endif
}

unsigned long ChildProcess::WaitForExit() {
#ifdef _WIN32
  if (process_ == nullptr) return 0;
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  return code;
#else
  return 0;
#endif
}

ExitWait ChildProcess::WaitBounded(unsigned long milliseconds) {
#ifdef _WIN32
  if (process_ == nullptr) return ExitWait::Exited;
  const DWORD result = WaitForSingleObject(static_cast<HANDLE>(process_), milliseconds);
  return result == WAIT_OBJECT_0 ? ExitWait::Exited : ExitWait::StillRunning;
#else
  (void)milliseconds;
  return ExitWait::Exited;
#endif
}

void ChildProcess::Kill() {
#ifdef _WIN32
  if (process_ == nullptr) return;
  TerminateProcess(static_cast<HANDLE>(process_), 137);
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  CloseHandle(static_cast<HANDLE>(process_));
  process_ = nullptr;
  pid_ = 0;
#endif
}

void ChildProcess::CloseHandles() {
#ifdef _WIN32
  if (process_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
#endif
}

std::string CoordinatorExecutablePath() {
#ifdef _WIN32
  const std::string from_env = ReadEnvironment("FCR_COORDINATOR_EXE");
#else
  const char* raw = std::getenv("FCR_COORDINATOR_EXE");
  const std::string from_env = raw == nullptr ? std::string() : std::string(raw);
#endif
  if (!from_env.empty()) return from_env;
#ifdef FCR_COORDINATOR_EXE_PATH
  return FCR_COORDINATOR_EXE_PATH;
#else
  return {};
#endif
}

std::string PublisherExecutablePath() {
#ifdef _WIN32
  const std::string from_env = ReadEnvironment("FCR_PUBLISHER_EXE");
#else
  const char* raw = std::getenv("FCR_PUBLISHER_EXE");
  const std::string from_env = raw == nullptr ? std::string() : std::string(raw);
#endif
  if (!from_env.empty()) return from_env;
#ifdef FCR_PUBLISHER_EXE_PATH
  return FCR_PUBLISHER_EXE_PATH;
#else
  return {};
#endif
}

std::string CliExecutablePath() {
#ifdef _WIN32
  const std::string from_env = ReadEnvironment("FCR_FCRCTL_EXE");
#else
  const char* raw = std::getenv("FCR_FCRCTL_EXE");
  const std::string from_env = raw == nullptr ? std::string() : std::string(raw);
#endif
  if (!from_env.empty()) return from_env;
#ifdef FCR_FCRCTL_EXE_PATH
  return FCR_FCRCTL_EXE_PATH;
#else
  return {};
#endif
}

std::uint16_t FindFreeLoopbackPort() {
#ifdef _WIN32
  EnsureWinsock();
  const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (probe == INVALID_SOCKET) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    closesocket(probe);
    return 0;
  }
  int length = sizeof(address);
  if (getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    closesocket(probe);
    return 0;
  }
  const std::uint16_t port = ntohs(address.sin_port);
  closesocket(probe);
  return port;
#else
  return 0;
#endif
}

bool ProcessAlive(unsigned long pid) {
#ifdef _WIN32
  if (pid == 0) return false;
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (handle == nullptr) return false;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(handle, &code) != 0 && code == STILL_ACTIVE;
  CloseHandle(handle);
  return alive;
#else
  (void)pid;
  return false;
#endif
}

}  // namespace fcr::test
