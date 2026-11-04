// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "desktop.h"
#include <memory>

enum class ComponentRole { Panel, Desktop, Files };
extern ComponentRole CurrentComponent;
extern DWORD ProtectionOwner;
constexpr UINT ComponentActionMessage=WM_APP+80;
struct ComponentAction { int code; std::wstring value; };
bool receiveComponentAction(HWND sender,const COPYDATASTRUCT* data,std::unique_ptr<ComponentAction>& action);
int runDesktopComponent(const std::wstring& channel);
int runFilesComponent(const std::wstring& path,bool selectFile=false,bool newWindow=false);
bool routeFileBrowser(const std::wstring& path,bool selectFile,bool newWindow);
bool windowsFileManager();
void setWindowsFileManager(bool enabled);
size_t fileBrowserWindowCount();
HWND lastFileBrowserWindow();
void componentFilesDisplayChanged();

class DesktopProcess {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    DesktopProcess();
    ~DesktopProcess();
    void start(HWND owner,AudioMeter* audio,std::function<DesktopData()> snapshot,std::function<void(int,const std::wstring&)> action);
    void stop();
    void maintain();
    void displayChanged();
    void reveal();
    bool needsAudio()const;
    int analysisRate()const;
    void refreshFiles();
    void appearance();
    void audioFrame();
    DWORD processId()const;
};
