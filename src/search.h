// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "tasks.h"
#include <condition_variable>
#include <atomic>

struct SearchResult {
    std::wstring name,path,target,arguments,workingDirectory;
    bool application=false,directory=false,administrator=false;
    std::shared_ptr<const PixelImage> icon;
};
struct SearchState {
    std::wstring query;
    uint64_t revision=0;
    bool busy=false;
    long indexError=0;
    std::vector<SearchResult> results;
};
class WindowsSearch {
    HWND owner=nullptr;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping=false,dirty=false;
    uint64_t revision=0;
    std::wstring query;
    std::vector<std::wstring> sources;
    std::shared_ptr<const SearchState> current=std::make_shared<SearchState>();
    void run();
public:
    ~WindowsSearch(){stop();}
    void start(HWND window);
    void stop();
    void request(const std::wstring& text);
    void setSources(const std::vector<std::wstring>& paths);
    std::shared_ptr<const SearchState> snapshot();
};
bool searchOpen(HWND owner,const SearchResult& item,bool administrator=false);
bool searchLocation(const SearchResult& item);

SearchState searchOnce(const std::wstring& text,const std::vector<std::wstring>& sources={});

SearchState searchFolder(const std::wstring& query,const std::wstring& folder);
