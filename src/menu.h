// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "graphics.h"
struct MenuItem {std::wstring text;const wchar_t* icon=nullptr;int id=0;bool checked=false;};
struct MenuColors {unsigned background,card,text,muted,accent;unsigned outline=0x77717f;float width=0;bool smooth=true;};
int customMenu(HWND owner,POINT anchor,const std::vector<MenuItem>& items,MenuColors colors);
