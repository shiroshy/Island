// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "graphics.h"

struct EngineWallpaperState {
    bool enabled=false,loading=false;
    HWND window=nullptr;
    std::wstring name,error;
};

// Uses Wallpaper Engine's documented CLI and Win32 embedding of our uniquely
// named pop-out. Existing desktop renderers and the main engine are not moved.
class EngineWallpaper {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    EngineWallpaper();
    ~EngineWallpaper();
    void start(const std::wstring& section,RECT monitor,int index,HWND backgroundHost);
    void stop();
    void setActive(bool active);
    void choose(HWND owner,bool current=false);
    void disable();
    EngineWallpaperState snapshot();
};
int wallpaperEngineWatch(const std::vector<std::wstring>& arguments);
int wallpaperEngineMonitorIndex(RECT monitor);
