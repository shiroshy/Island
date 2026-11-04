// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "tasks.h"
std::shared_ptr<const PixelImage> readWallpaper(const std::wstring& path,uint64_t id);
std::wstring chooseWallpaper(HWND owner,bool video=false);
struct WallpaperState {
    std::shared_ptr<const PixelImage> image;
    std::wstring name,error;
    bool loading=false,video=false,enabled=false;
};
class WallpaperMedia {
    std::mutex mutex;std::condition_variable wake;std::thread worker;
    std::wstring prefix,request;WallpaperState state;
    bool stopping=false,active=false;uint64_t generation=0;
    void run();
public:
    ~WallpaperMedia(){stop();}
    void start(const std::wstring& key,bool enabled=false);
    void stop();
    void choose(HWND owner,bool video=false);
    void clear();
    void enable(bool value);
    void setActive(bool value);
    WallpaperState snapshot();
};
