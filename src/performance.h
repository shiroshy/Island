// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "platform.h"
struct PerformanceSettings {
    int desktopFPS=30,videoFPS=30,analysisFPS=30;
    bool video=true;
    void load(const std::wstring& ini){desktopFPS=std::clamp((int)GetPrivateProfileIntW(L"performance",L"desktop-fps",30,ini.c_str()),10,60);videoFPS=std::clamp((int)GetPrivateProfileIntW(L"performance",L"video-fps",30,ini.c_str()),10,60);analysisFPS=std::clamp((int)GetPrivateProfileIntW(L"performance",L"analysis-fps",30,ini.c_str()),5,60);video=GetPrivateProfileIntW(L"performance",L"video",1,ini.c_str())!=0;}
    void save(const std::wstring& ini)const{for(auto value:{std::pair{L"desktop-fps",desktopFPS},std::pair{L"video-fps",videoFPS},std::pair{L"analysis-fps",analysisFPS},std::pair{L"video",video?1:0}})WritePrivateProfileStringW(L"performance",value.first,std::to_wstring(value.second).c_str(),ini.c_str());}
};
