// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "platform.h"
#include "audio.h"
#include "performance.h"
#include "network.h"
#include <functional>

struct DesktopData {
    PerformanceSettings performance;NetworkKind network=NetworkKind::Disconnected;
    MediaState media;
    WeatherState weather;
    std::shared_ptr<const TaskSnapshot> tasks;
    std::vector<std::wstring> pinned;
    std::wstring note;
    unsigned background=0x09090b,card=0x252328,accent=0xb8d8c9,ink=0xeae9ed,muted=0x8a8790,hover=0x322f37;
    unsigned outline=0x77717f;float outlineWidth=0;bool outlineSmooth=true;
    int timerSeconds=300;
    bool timerRunning=false,timerPaused=false,mutedAudio=false,awake=true;
    float volume=0;
};
enum DesktopAction {DeskSearch=1,DeskApps,DeskAddApp,DeskSettings,DeskWeather,DeskNote,DeskTimerToggle,DeskTimerReset,DeskSaveNote,DeskPinApp,DeskUnpinApp,DeskReorderApp,DeskVolume,DeskToggleMute,DeskSetTimer,DeskAppearance};
class Desktop {

    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    Desktop();
    ~Desktop();
    void start(HWND owner,AudioMeter* audio,std::function<DesktopData()> snapshot,std::function<void(int,const std::wstring&)> action);
    void stop();
    void maintain();
    void displayChanged();
    void reveal();
    bool needsAudio()const;
    int analysisRate()const;
    void refreshFiles();
    void appearance();
};

