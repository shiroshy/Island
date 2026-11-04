// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <string>
#include <windows.h>

// Fixed-width, naturally aligned fields: the observer may be a different
// bitness. gate is either a protected-operation reference count or -1 during
// recovery.
namespace health {
constexpr LONG Magic = 0x49534831;
struct Shared {
  LONG magic;
  DWORD pid;
  FILETIME created;
  volatile LONG gate, ready, awake, stopping, window;
};
inline HANDLE mapping = nullptr;
inline Shared *shared = nullptr;
inline std::wstring name(DWORD pid) {
  return L"Local\\OwnIsland.Health." + std::to_wstring(pid);
}
inline LONG read(volatile LONG &value) {
  return InterlockedCompareExchange(&value, 0, 0);
}
inline bool initialize() {
  if (shared)
    return true;
  mapping =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         sizeof(Shared), name(GetCurrentProcessId()).c_str());
  if (!mapping)
    return false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mapping);
    mapping = nullptr;
    return false;
  }
  shared = (Shared *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                   sizeof(Shared));
  if (!shared) {
    CloseHandle(mapping);
    mapping = nullptr;
    return false;
  }
  FILETIME exit{}, kernel{}, user{};
  GetProcessTimes(GetCurrentProcess(), &shared->created, &exit, &kernel, &user);
  shared->pid = GetCurrentProcessId();
  shared->awake = 1;
  InterlockedExchange(&shared->magic, Magic);
  return true;
}
class Hold {
  Shared *state = shared;
  bool acquired = false;

public:
  Hold() {
    if (!state) {
      acquired = true;
      return;
    }
    for (LONG value = read(state->gate); value >= 0 && value < 0x100000;
         value = read(state->gate))
      if (InterlockedCompareExchange(&state->gate, value + 1, value) == value) {
        acquired = true;
        break;
      }
  }
  ~Hold() {
    if (state && acquired)
      InterlockedDecrement(&state->gate);
  }
  explicit operator bool() const { return acquired; }
  Hold(const Hold &) = delete;
  Hold &operator=(const Hold &) = delete;
};
inline void ready(HWND hwnd) {
  if (!shared)
    return;
  InterlockedExchange(&shared->window, (LONG)(UINT_PTR)hwnd);
  InterlockedExchange(&shared->ready, 1);
}
inline void awake(bool value) {
  if (shared)
    InterlockedExchange(&shared->awake, value);
}
inline void stopping(bool value) {
  if (shared)
    InterlockedExchange(&shared->stopping, value);
}
} // namespace health
