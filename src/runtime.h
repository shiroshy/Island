// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <string>
// Runtime choice is independent of the Windows logon-shell configuration.
extern bool StandaloneMode;
extern std::wstring StateDirectoryOverride;
std::wstring runtimeArguments();
bool runtimeDesktopVisible();
