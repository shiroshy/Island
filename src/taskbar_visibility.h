// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// Visibility/work-area ownership only. Never enable Explorer's auto-hide state.
// This header is also used by the small recovery launcher, without COM/graphics.
namespace taskbar_visibility {
inline constexpr auto Section=L"taskbar-visibility", Marker=L"OwnIsland.TaskbarOwner";
struct DpiScope {
    DPI_AWARENESS_CONTEXT previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope(){if(previous)SetThreadDpiAwarenessContext(previous);}
};
inline bool sameRect(const RECT& a,const RECT& b){return EqualRect(&a,&b)!=FALSE;}
inline bool barClass(HWND w){wchar_t c[80]={};return w&&GetClassNameW(w,c,80)&&(!wcscmp(c,L"Shell_TrayWnd")||!wcscmp(c,L"Shell_SecondaryTrayWnd"))&&!GetPropW(w,L"OwnIsland.TrayEndpoint");}
inline ULONGLONG created(DWORD pid){
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!p)return 0;
    FILETIME a{},b{},c{},d{};bool ok=GetProcessTimes(p,&a,&b,&c,&d)!=FALSE;CloseHandle(p);return ok?((ULONGLONG)a.dwHighDateTime<<32)|a.dwLowDateTime:0;
}
struct Generation {
    HWND bar=nullptr,shell=nullptr;DWORD pid=0;ULONGLONG stamp=0;
    bool operator==(const Generation&)const=default;
    explicit operator bool()const{return bar&&shell&&pid&&stamp;}
};
struct Probe {Generation generation;bool ready=false;};
inline HWND primary(){
    HWND result=nullptr;EnumWindows([](HWND w,LPARAM p)->BOOL{wchar_t c[64]={};GetClassNameW(w,c,64);if(!wcscmp(c,L"Shell_TrayWnd")&&!GetPropW(w,L"OwnIsland.TrayEndpoint")){*(HWND*)p=w;return FALSE;}return TRUE;},(LPARAM)&result);return result;
}
inline Probe probe(bool legacyHidden=false){
    Probe out;out.generation.bar=primary();out.generation.shell=GetShellWindow();DWORD shellPid=0;
    if(!out.generation.bar||!out.generation.shell)return out;
    GetWindowThreadProcessId(out.generation.bar,&out.generation.pid);GetWindowThreadProcessId(out.generation.shell,&shellPid);
    if(!shellPid||shellPid!=out.generation.pid)return out;
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,shellPid);if(!p)return out;
    FILETIME a{},b{},c{},d{};wchar_t path[32768]={},windows[MAX_PATH]={};DWORD length=32768;
    bool valid=GetProcessTimes(p,&a,&b,&c,&d)&&QueryFullProcessImageNameW(p,0,path,&length)&&GetWindowsDirectoryW(windows,MAX_PATH);
    if(valid){out.generation.stamp=((ULONGLONG)a.dwHighDateTime<<32)|a.dwLowDateTime;valid=!_wcsicmp(path,(std::wstring(windows)+L"\\explorer.exe").c_str());}
    bool idle=valid&&WaitForInputIdle(p,0)==0;CloseHandle(p);if(!idle)return out;
    struct Desktop {DWORD pid;bool found=false;} desktop{shellPid};
    EnumWindows([](HWND w,LPARAM data)->BOOL{auto& d=*(Desktop*)data;DWORD pid=0;GetWindowThreadProcessId(w,&pid);if(pid==d.pid&&FindWindowExW(w,nullptr,L"SHELLDLL_DefView",nullptr)){d.found=true;return FALSE;}return TRUE;},(LPARAM)&desktop);
    RECT r{};DWORD_PTR answer=0;
    out.ready=desktop.found&&FindWindowExW(out.generation.bar,nullptr,L"TrayNotifyWnd",nullptr)&&GetWindowRect(out.generation.bar,&r)&&r.right>r.left&&r.bottom>r.top&&
        (IsWindowVisible(out.generation.bar)||GetPropW(out.generation.bar,Marker)||legacyHidden)&&
        SendMessageTimeoutW(out.generation.bar,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,50,&answer);
    return out;
}
struct Gate {
    Generation generation;ULONGLONG since=0;bool observing=false,admitted=false;
    bool update(const Probe& p,ULONGLONG now){
        if(!(p.generation==generation)){generation=p.generation;observing=admitted=false;}
        if(!p.ready||!generation){observing=admitted=false;return false;}
        if(!observing){since=now;observing=true;return false;}
        // A quiet interval supplements read-only readiness checks; it is not
        // permission to call undocumented initialization routines or auto-hide.
        admitted=now-since>=750;return admitted;
    }
};
inline int read(const std::wstring& f,const wchar_t* key,int fallback=0){return GetPrivateProfileIntW(Section,key,fallback,f.c_str());}
inline std::wstring value(const std::wstring& f,const std::wstring& key){wchar_t text[512]={};GetPrivateProfileStringW(Section,key.c_str(),L"",text,512,f.c_str());return text;}
inline bool put(const std::wstring& f,const std::wstring& key,const std::wstring& text){return WritePrivateProfileStringW(Section,key.c_str(),text.c_str(),f.c_str())!=FALSE;}
inline void log(const std::wstring& f,const wchar_t* stage,const Generation& g){
    auto at=f.find_last_of(L"\\/");auto file=f.substr(0,at+1)+L"taskbar-startup.log";WIN32_FILE_ATTRIBUTE_DATA info{};
    if(GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&info)&&(info.nFileSizeHigh||info.nFileSizeLow>32768))MoveFileExW(file.c_str(),(file+L".previous").c_str(),MOVEFILE_REPLACE_EXISTING);
    HANDLE h=CreateFileW(file.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return;
    char text[256];int n=snprintf(text,sizeof(text),"tick=%llu pid=%lu %ls explorer=%lu created=%llu bar=%llu\r\n",GetTickCount64(),GetCurrentProcessId(),stage,g.pid,g.stamp,(ULONGLONG)(UINT_PTR)g.bar);DWORD written=0;if(n>0)WriteFile(h,text,std::min(n,255),&written,nullptr);CloseHandle(h);
}
struct Record {HWND window=nullptr;DWORD pid=0,owner=0;ULONGLONG stamp=0;};
inline std::wstring encode(const Record& r){return std::to_wstring((UINT_PTR)r.window)+L","+std::to_wstring(r.pid)+L","+std::to_wstring(r.stamp)+L","+std::to_wstring(r.owner);}
inline bool decode(const std::wstring& text,Record& r){unsigned long long hwnd=0,stamp=0;unsigned pid=0,owner=0;wchar_t tail=0;if(swscanf(text.c_str(),L"%llu,%u,%llu,%u%c",&hwnd,&pid,&stamp,&owner,&tail)!=4||!hwnd||!pid||!stamp||!owner)return false;r={(HWND)(UINT_PTR)hwnd,pid,owner,stamp};return true;}
inline bool matches(const Record& r){DWORD pid=0;GetWindowThreadProcessId(r.window,&pid);return barClass(r.window)&&pid==r.pid&&created(pid)==r.stamp;}
inline bool owned(const Record& r){return matches(r)&&(DWORD)(UINT_PTR)GetPropW(r.window,Marker)==r.owner;}
inline std::wstring rectText(const RECT& r){return std::to_wstring(r.left)+L","+std::to_wstring(r.top)+L","+std::to_wstring(r.right)+L","+std::to_wstring(r.bottom);}
inline bool parseRect(const std::wstring& s,RECT& r){wchar_t tail=0;return swscanf(s.c_str(),L"%ld,%ld,%ld,%ld%c",&r.left,&r.top,&r.right,&r.bottom,&tail)==4&&r.right>r.left&&r.bottom>r.top;}
inline RECT reclaim(const RECT& monitor,const RECT& work,const RECT& bar){
    RECT result=work;
    // Reclaim only the strip actually reserved by this taskbar. Preserve other edges.
    if(bar.left==monitor.left&&bar.right==monitor.right){
        if(bar.bottom==monitor.bottom&&work.bottom==bar.top)result.bottom=monitor.bottom;
        else if(bar.top==monitor.top&&work.top==bar.bottom)result.top=monitor.top;
    }else if(bar.top==monitor.top&&bar.bottom==monitor.bottom){
        if(bar.left==monitor.left&&work.left==bar.right)result.left=monitor.left;
        else if(bar.right==monitor.right&&work.right==bar.left)result.right=monitor.right;
    }
    return result;
}
inline bool shellMatches(DWORD pid,ULONGLONG stamp){DWORD current=0;GetWindowThreadProcessId(GetShellWindow(),&current);return current==pid&&created(current)==stamp;}
inline bool applicationNotice(const wchar_t* name,DWORD pid,DWORD shellPid){
    if(!wcsncmp(name,L"OwnIsland.",10)&&wcscmp(name,L"OwnIsland.Files"))return false;
    // Explorer's taskbar reacts to the broadcast by reserving its strip again.
    // Folder windows are ordinary applications and still need the new bounds.
    return pid!=shellPid||!wcscmp(name,L"CabinetWClass")||!wcscmp(name,L"ExploreWClass");
}
inline void notifyApplications(DWORD shellPid){
    struct Notice {DWORD shell;ULONGLONG end;} notice{shellPid,GetTickCount64()+250};
    EnumWindows([](HWND w,LPARAM data)->BOOL{
        auto& n=*(Notice*)data;if(GetTickCount64()>=n.end)return FALSE;
        if(!IsWindowVisible(w))return TRUE;DWORD pid=0;GetWindowThreadProcessId(w,&pid);wchar_t name[128]={};GetClassNameW(w,name,128);
        if(applicationNotice(name,pid,n.shell)){DWORD_PTR answer=0;SendMessageTimeoutW(w,WM_SETTINGCHANGE,SPI_SETWORKAREA,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,25,&answer);}return TRUE;
    },(LPARAM)&notice);
}
inline bool applyArea(RECT target){
    // No registry write and no HWND_BROADCAST: notifying Explorer here undoes
    // this change synchronously. Notify application windows after the batch.
    return SystemParametersInfoW(SPI_SETWORKAREA,0,&target,0)!=FALSE;
}
inline bool repairAllowed(const RECT& current,const RECT& before,const RECT& after){return !sameRect(before,after)&&sameRect(current,before);}
inline bool maintainAreas(const std::wstring& f,const Generation& g,DWORD owner){
    if(!g||!shellMatches(g.pid,g.stamp))return false;DpiScope dpi;bool changed=false;
    int count=std::clamp(read(f,L"areas"),0,16);
    for(int i=0;i<count;i++){
        auto key=L"area-"+std::to_wstring(i);Record r;RECT bounds{},before{},after{};
        if(!decode(value(f,key+L"-owner"),r)||r.pid!=g.pid||r.stamp!=g.stamp||r.owner!=owner||!owned(r)||IsWindowVisible(r.window)||!parseRect(value(f,key+L"-bounds"),bounds)||!parseRect(value(f,key+L"-before"),before)||!parseRect(value(f,key+L"-after"),after))continue;
        HMONITOR monitor=MonitorFromRect(&bounds,MONITOR_DEFAULTTONULL);MONITORINFOEXW m{};m.cbSize=sizeof(m);RECT bar{};
        if(!monitor||!GetMonitorInfoW(monitor,&m)||value(f,key+L"-device")!=m.szDevice||!sameRect(m.rcMonitor,bounds)||!GetWindowRect(r.window,&bar))continue;
        // Only repair the exact old taskbar reservation. A different work area
        // belongs to another setting/appbar; do not fight or overwrite it.
        if(repairAllowed(m.rcWork,before,after)&&sameRect(reclaim(bounds,before,bar),after)&&applyArea(after))changed=true;
    }
    if(changed){notifyApplications(g.pid);log(f,L"work-area-repaired",g);}return changed;
}
inline void restore(const std::wstring& f){
    DpiScope dpi;int count=std::clamp(read(f,L"count"),0,16);
    for(int i=0;i<count;i++){Record r;if(decode(value(f,L"bar-"+std::to_wstring(i)),r)&&owned(r)){ShowWindowAsync(r.window,SW_SHOWNOACTIVATE);RemovePropW(r.window,Marker);}}
    int areas=std::clamp(read(f,L"areas"),0,16);bool changed=false;DWORD shellPid=0;
    for(int i=0;i<areas;i++){
        auto key=L"area-"+std::to_wstring(i);RECT bounds{},before{},after{};Record owner;
        if(!decode(value(f,key+L"-owner"),owner)||!shellMatches(owner.pid,owner.stamp)||!parseRect(value(f,key+L"-bounds"),bounds)||!parseRect(value(f,key+L"-before"),before)||!parseRect(value(f,key+L"-after"),after))continue;
        HMONITOR monitor=MonitorFromRect(&bounds,MONITOR_DEFAULTTONULL);MONITORINFOEXW m{};m.cbSize=sizeof(m);
        if(monitor&&GetMonitorInfoW(monitor,&m)&&value(f,key+L"-device")==m.szDevice&&sameRect(m.rcMonitor,bounds)&&sameRect(m.rcWork,after)&&applyArea(before)){changed=true;shellPid=owner.pid;}
    }
    if(changed)notifyApplications(shellPid);
    WritePrivateProfileStringW(Section,nullptr,nullptr,f.c_str());
}
inline bool hide(const std::wstring& f,const Generation& g,DWORD owner,bool legacyHidden){
    if(!g||!shellMatches(g.pid,g.stamp))return false;DpiScope dpi;
    struct Context {DWORD pid;std::vector<HWND> windows;} context{g.pid};
    EnumWindows([](HWND w,LPARAM p)->BOOL{auto& c=*(Context*)p;DWORD pid=0;GetWindowThreadProcessId(w,&pid);if(pid==c.pid&&barClass(w))c.windows.push_back(w);return TRUE;},(LPARAM)&context);
    bool ok=true,changed=false;for(auto w:context.windows){
        Record record{w,g.pid,owner,g.stamp};if(!matches(record))continue;
        if(owned(record)){if(IsWindowVisible(w))ShowWindowAsync(w,SW_HIDE);continue;}
        if(GetPropW(w,Marker)||(!IsWindowVisible(w)&&!(legacyHidden&&w==g.bar)))continue;
        int count=read(f,L"count");if(count<0||count>=16){ok=false;continue;}
        RECT bar{};DWORD_PTR answer=0;
        if(!GetWindowRect(w,&bar)||bar.right<=bar.left||bar.bottom<=bar.top||!SendMessageTimeoutW(w,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,50,&answer))continue;
        MONITORINFOEXW m{};m.cbSize=sizeof(m);auto monitor=MonitorFromWindow(w,MONITOR_DEFAULTTONULL);
        bool geometry=monitor&&GetMonitorInfoW(monitor,&m);RECT target=geometry?reclaim(m.rcMonitor,m.rcWork,bar):RECT{};
        // Persist before setting the marker or hiding. A crash at any later point is recoverable.
        if(!put(f,L"bar-"+std::to_wstring(count),encode(record))||!put(f,L"count",std::to_wstring(count+1))||!SetPropW(w,Marker,(HANDLE)(UINT_PTR)owner)){ok=false;continue;}
        if(!ShowWindowAsync(w,SW_HIDE)){RemovePropW(w,Marker);ok=false;continue;}
        if(geometry&&!sameRect(target,m.rcWork)){
            int areas=read(f,L"areas");if(areas<0||areas>=16){ok=false;continue;}auto key=L"area-"+std::to_wstring(areas);
            bool saved=put(f,key+L"-owner",encode(record))&&put(f,key+L"-device",m.szDevice)&&put(f,key+L"-bounds",rectText(m.rcMonitor))&&put(f,key+L"-before",rectText(m.rcWork))&&put(f,key+L"-after",rectText(target))&&put(f,L"areas",std::to_wstring(areas+1));
            if(saved&&matches(record)&&applyArea(target))changed=true;
        }
    }
    if(changed)notifyApplications(g.pid);
    maintainAreas(f,g,owner);
    return ok;
}
// Old snapshots have no Explorer identity. Never replay their ABM_SETSTATE or
// hidden bit into a fresh Explorer; allow a later healthy takeover to restore visibility.
inline void rememberLegacy(const std::wstring& f){
    wchar_t policy[64]={};GetPrivateProfileStringW(L"shell",L"taskbar-policy",L"",policy,64,f.c_str());
    if(!*policy&&GetPrivateProfileIntW(L"shell",L"had-taskbar",0,f.c_str()))WritePrivateProfileStringW(L"shell",L"legacy-visibility-pending",L"1",f.c_str());
}
}
