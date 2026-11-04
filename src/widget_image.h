// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "wallpaper.h"
#include <atomic>
struct WidgetImageState {std::shared_ptr<const PixelImage> image;std::wstring name,error;bool animated=false,loading=false;};
class WidgetImageMedia {
    std::mutex mutex;std::condition_variable wake;std::thread worker;
    bool stopping=false,active=false,pending=false;int fps=30;uint64_t generation=0;
    std::wstring key,request;WidgetImageState state;
    void run();
public:
    ~WidgetImageMedia(){stop();}
    void start(const std::wstring& id);void stop();void load(const std::wstring& path);void choose(HWND owner);void clear();
    void setActive(bool value,int maximumFPS=30);
    WidgetImageState snapshot(){std::lock_guard lock(mutex);return state;}
};

