// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <windows.h>
#include <string>
// Called only after our opaque surfaces cover every monitor.
bool nativeDesktopTakeover(HWND host);
void nativeDesktopRelease();
// Also called by the independent shell watchdog after the main process exits.
void nativeDesktopRecover(const std::wstring& recoveryFile);
