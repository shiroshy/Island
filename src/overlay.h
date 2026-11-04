// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#define NOMINMAX
#include <windows.h>

class Overlay {
    HMONITOR monitor=nullptr;
    UINT adapter=0,source=0;
    bool exclusive();
public:
    ~Overlay(){reset();}
    void reset();
    bool visible(HWND hwnd);
    void raise(HWND hwnd);
};
