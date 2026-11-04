// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "native_desktop.h"
#include "platform.h"
#include "components.h"
#include <vector>
#include <sstream>
#include <algorithm>

DWORD ProtectionOwner=0;
namespace {
constexpr auto section=L"native-desktop", marker=L"OwnIsland.NativeDesktopOwner";
struct Record {HWND window=nullptr;DWORD process=0,owner=0;bool visible=false,enabled=false;};
std::vector<Record> held;
struct Lock {
    HANDLE handle=CreateMutexW(nullptr,FALSE,L"Local\\OwnIsland.ShellRestoreLock");
    bool ready=false;Lock(){if(handle){auto result=WaitForSingleObject(handle,2000);ready=result==WAIT_OBJECT_0||result==WAIT_ABANDONED;}}
    ~Lock(){if(ready)ReleaseMutex(handle);if(handle)CloseHandle(handle);}
};
bool isClass(HWND w,const wchar_t* name){wchar_t cls[100]={};return w&&GetClassNameW(w,cls,100)&&!wcscmp(cls,name);}
bool desktopView(HWND w){HWND root=GetParent(w);return isClass(w,L"SHELLDLL_DefView")&&(isClass(root,L"WorkerW")||isClass(root,L"Progman"));}
std::wstring file(){return stateDirectory()+L"\\shell-restore.ini";}
std::wstring key(size_t index){return L"view-"+std::to_wstring(index);}
bool valid(const Record& r){DWORD pid=0;GetWindowThreadProcessId(r.window,&pid);return desktopView(r.window)&&pid==r.process&&(DWORD)(UINT_PTR)GetPropW(r.window,marker)==r.owner;}
bool restoreRecord(const Record& r){
    if(!valid(r))return false;
    EnableWindow(r.window,r.enabled);ShowWindow(r.window,r.visible?SW_SHOWNOACTIVATE:SW_HIDE);
    RemovePropW(r.window,marker);return true;
}
std::wstring encode(const Record& r){return std::to_wstring((UINT_PTR)r.window)+L","+std::to_wstring(r.process)+L","+std::to_wstring(r.owner)+L","+std::to_wstring(r.visible)+L","+std::to_wstring(r.enabled);}
bool decode(const wchar_t* text,Record& r){unsigned long long hwnd=0;unsigned process=0,owner=0;int visible=0,enabled=0;if(swscanf(text,L"%llu,%u,%u,%d,%d",&hwnd,&process,&owner,&visible,&enabled)!=5||!hwnd||!process||!owner)return false;r={(HWND)(UINT_PTR)hwnd,process,owner,visible!=0,enabled!=0};return true;}
std::vector<HWND> nativeViews(){
    std::vector<HWND> out;DWORD shell=0;GetWindowThreadProcessId(GetShellWindow(),&shell);if(!shell)return out;
    struct Context{DWORD pid;std::vector<HWND>* out;} context{shell,&out};
    EnumWindows([](HWND root,LPARAM p)->BOOL{auto& c=*(Context*)p;DWORD pid=0;GetWindowThreadProcessId(root,&pid);if(pid!=c.pid||(!isClass(root,L"WorkerW")&&!isClass(root,L"Progman")))return TRUE;
        for(HWND child=FindWindowExW(root,nullptr,L"SHELLDLL_DefView",nullptr);child;child=FindWindowExW(root,child,L"SHELLDLL_DefView",nullptr))c.out->push_back(child);return TRUE;},(LPARAM)&context);return out;
}
}
void nativeDesktopRecover(const std::wstring& recoveryFile){
    int count=std::clamp((int)GetPrivateProfileIntW(section,L"count",0,recoveryFile.c_str()),0,16);
    for(int i=0;i<count;i++){wchar_t text[200]={};GetPrivateProfileStringW(section,key(i).c_str(),L"",text,200,recoveryFile.c_str());Record r;if(decode(text,r))restoreRecord(r);}
    WritePrivateProfileStringW(section,nullptr,nullptr,recoveryFile.c_str());
}
bool nativeDesktopTakeover(HWND host){
    HWND window=host?FindWindowExW(host,nullptr,L"SHELLDLL_DefView",nullptr):nullptr;
    if(!window||!desktopView(window))return false;
    DWORD shell=0,pid=0;GetWindowThreadProcessId(GetShellWindow(),&shell);GetWindowThreadProcessId(window,&pid);if(!shell||pid!=shell)return false;
    auto found=std::find_if(held.begin(),held.end(),[&](auto& r){return r.window==window&&valid(r);});
    if(found==held.end()){
        Lock lock;if(!lock.ready)return false;auto recoveryFile=file();DWORD owner=GetCurrentProcessId();
        if((DWORD)GetPrivateProfileIntW(L"shell",L"pid",0,recoveryFile.c_str())!=(ProtectionOwner?ProtectionOwner:owner)||!GetPrivateProfileIntW(L"shell",L"armed",0,recoveryFile.c_str())||held.size()>=16)return false;
        // Preserve the view's own style, not the visibility of its parent.
        auto style=GetWindowLongPtrW(window,GWL_STYLE);Record r{window,pid,owner,(style&WS_VISIBLE)!=0,(style&WS_DISABLED)==0};
        if(!SetPropW(window,marker,(HANDLE)(UINT_PTR)owner))return false;
        if(!WritePrivateProfileStringW(section,key(held.size()).c_str(),encode(r).c_str(),recoveryFile.c_str())||!WritePrivateProfileStringW(section,L"count",std::to_wstring(held.size()+1).c_str(),recoveryFile.c_str())){RemovePropW(window,marker);return false;}
        held.push_back(r);
    }
    if(IsWindowEnabled(window))EnableWindow(window,FALSE);
    if(GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE)ShowWindow(window,SW_HIDE);
    return !(GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE)&&!IsWindowEnabled(window);
}
void nativeDesktopRelease(){Lock lock;if(!lock.ready)return;auto recoveryFile=file();if((DWORD)GetPrivateProfileIntW(L"shell",L"pid",0,recoveryFile.c_str())==(ProtectionOwner?ProtectionOwner:GetCurrentProcessId()))nativeDesktopRecover(recoveryFile);held.clear();}



