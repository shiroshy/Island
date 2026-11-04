// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "menu.h"
bool openFileBrowser(const std::wstring& path=L"",bool selectFile=false,bool newWindow=false);
bool openFileLocation(const std::wstring& path);
void fileBrowserColors(MenuColors colors);
void closeFileBrowsers();
void fileBrowserDisplayChanged();
bool fileOperationsActive();
