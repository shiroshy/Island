// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include "tray.h"

// State machine is separate from input injection so combinations can be tested locally.
struct WinKeyState {
    unsigned held=0;
    bool chord=false;
    bool event(DWORD key,bool down,bool modifiers=false){
        unsigned bit=key==VK_LWIN?1:key==VK_RWIN?2:0;
        if(bit){if(down){if(!(held&bit)){if(!held)chord=modifiers;else chord=true;held|=bit;}return false;}
            bool solo=(held&bit)&&!chord;held&=~bit;return solo&&!held;}
        if(down&&held)chord=true;return false;
    }
};
struct DesktopLanguageChord {
    bool alt=false,shift=false,armed=false,used=false;
    bool event(DWORD key,bool down,bool blocked=false){
        bool a=key==VK_MENU||key==VK_LMENU||key==VK_RMENU,s=key==VK_SHIFT||key==VK_LSHIFT||key==VK_RSHIFT;
        if(!a&&!s){if(down&&(alt||shift))used=true;return false;}
        if(down){if(!alt&&!shift){used=false;armed=false;}if(a)alt=true;if(s)shift=true;if(blocked)used=true;if(alt&&shift&&!used)armed=true;return false;}
        bool fire=armed&&!used;if(a)alt=false;if(s)shift=false;if(fire)armed=false;if(!alt&&!shift){used=false;armed=false;}return fire;
    }
};
bool desktopLanguageTarget(HWND window);
inline int standaloneShortcut(DWORD key,bool modifiers){if(modifiers)return -1;return key=='D'?0:key=='R'?1:key=='E'?2:key=='S'?3:-1;}
void synchronizeDesktopLanguage(HWND root,HKL before);
bool startShellProtection();
void shellProtectionReady();
void stopShellProtection();
class ShellPanel {
    HWND owner=nullptr;
    std::wstring state;
    bool active=false;
    HWINEVENTHOOK showHook=nullptr;
    HWINEVENTHOOK overflowHook=nullptr;
    POINT overflowAnchor{};
    bool overflowAnchored=false;
    TrayHost ownTray;
    MenuColors trayColors{0x09090b,0x252328,0xeae9ed,0x8a8790,0xb8d8c9};
    HANDLE keyReadyEvent=nullptr;
    std::atomic<bool> keyInstalled=false,keyStopping=false;
    std::thread keyThread;
    std::atomic<DWORD> keyThreadId=0;
    void monitorOverflowMouse(bool enabled);
public:
    ~ShellPanel(){stop();}
    bool start(HWND target);
    void stop();
    void maintain();
    bool hiddenIcons(POINT anchor);
    bool overflowVisible()const;
    void updateOverflow();
    void closeOverflow();
    bool contextInteraction()const;
    bool dismissInactiveOverflow();
    void setAwake(bool awake){ownTray.setAwake(awake);}
    void colors(MenuColors value){trayColors=value;ownTray.colors(value);}
};
int shellWatch(DWORD parent);
bool launchCommand(HWND owner,const std::wstring& text,bool elevated=false);
