// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <windows.h>
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <map>
#include <cstdint>

struct PixelImage {
    unsigned width=0,height=0;
    uint64_t id=0;
    std::vector<unsigned char> bgra;
};
std::shared_ptr<const PixelImage> loadShellIcon(const std::wstring& path,uint64_t id);
std::shared_ptr<const PixelImage> copyIconPixels(HICON icon,uint64_t id);
struct TaskWindow {
    HWND hwnd=nullptr;
    DWORD pid=0;
    std::wstring title;
};
struct TaskGroup {
    std::wstring key,name,path;
    std::shared_ptr<const PixelImage> icon;
    std::vector<TaskWindow> windows;
    bool active=false;
};
struct TaskSnapshot {
    std::vector<TaskGroup> groups;
    std::map<std::wstring,std::shared_ptr<const PixelImage>> shortcuts;
};
class TaskTracker {
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping=false,dirty=true;
    HWND target=nullptr;
    std::vector<std::wstring> pinned;
    std::shared_ptr<const TaskSnapshot> current=std::make_shared<TaskSnapshot>();
    void run();
public:
    ~TaskTracker(){stop();}
    void start(HWND window);
    void stop();
    void refresh();
    void setPinned(const std::vector<std::wstring>& paths);
    std::shared_ptr<const TaskSnapshot> snapshot();
};
bool validTaskWindow(const TaskWindow& window);
bool isTaskWindow(HWND hwnd);
std::vector<TaskWindow> taskWindows();
bool activateTaskWindow(const TaskWindow& window,bool toggle=false);

void openPinnedTarget(const std::wstring& path,const std::shared_ptr<const TaskSnapshot>& snapshot);
