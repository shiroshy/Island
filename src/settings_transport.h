#pragma once
#include "settings_catalog.h"
#include <cwchar>
#include <filesystem>
namespace island_settings {
inline bool processPath(DWORD pid,const wchar_t* expected){HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!p)return false;wchar_t path[32768]{};DWORD n=32768;bool same=QueryFullProcessImageNameW(p,0,path,&n)&&!_wcsicmp(path,expected);CloseHandle(p);return same;}
inline bool settingsOwner(HWND w){if(!IsWindow(w))return false;struct Find{bool found=false;wchar_t expected[32768]{};} f;GetWindowsDirectoryW(f.expected,32768);wcscat_s(f.expected,L"\\ImmersiveControlPanel\\SystemSettings.exe");DWORD pid{};GetWindowThreadProcessId(w,&pid);f.found=processPath(pid,f.expected);if(!f.found)EnumChildWindows(w,[](HWND child,LPARAM data)->BOOL{auto f=(Find*)data;DWORD pid{};GetWindowThreadProcessId(child,&pid);if(processPath(pid,f->expected)){f->found=true;return FALSE;}return TRUE;},(LPARAM)&f);return f.found;}
inline HWND foregroundSettings(){auto w=GetForegroundWindow();return settingsOwner(w)?w:nullptr;}
inline int settingsMonitorIndex(const std::wstring& ini,HWND owner=nullptr){int saved=(int)GetPrivateProfileIntW(L"desktop",L"settings-monitor",-1,ini.c_str());if(saved>=0&&GetPrivateProfileIntW(L"desktop",L"settings-monitor-explicit",saved>0?1:0,ini.c_str()))return saved;bool internalOwner=false;
#ifdef ISLAND_INTERNAL_SETTINGS
wchar_t ownerClass[64]={};if(IsWindow(owner))GetClassNameW(owner,ownerClass,64);internalOwner=!wcscmp(ownerClass,L"OwnIsland.Surface");
#endif
if(!settingsOwner(owner)&&!internalOwner){owner=foregroundSettings();if(!owner)EnumWindows([](HWND w,LPARAM p)->BOOL{if(IsWindowVisible(w)&&settingsOwner(w)){*(HWND*)p=w;return FALSE;}return TRUE;},(LPARAM)&owner);}auto target=MonitorFromWindow(owner,MONITOR_DEFAULTTOPRIMARY);struct State{HMONITOR target;int index=0,result=0;} state{target};EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR m,HDC,LPRECT,LPARAM p)->BOOL{auto s=(State*)p;if(m==s->target)s->result=s->index;++s->index;return TRUE;},(LPARAM)&state);return state.result;}
inline std::wstring expectedPanelPath(){HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&expectedPanelPath,&module);wchar_t path[32768]={};GetModuleFileNameW(module,path,32768);auto folder=std::filesystem::path(path).parent_path();auto candidate=folder/L"Island.exe";if(GetFileAttributesW(candidate.c_str())==INVALID_FILE_ATTRIBUTES)candidate=folder.parent_path()/L"Island.exe";return candidate.wstring();}
inline HWND panel(){auto w=FindWindowW(L"OwnIsland.Surface",nullptr);DWORD pid{};if(w)GetWindowThreadProcessId(w,&pid);return w&&processPath(pid,expectedPanelPath().c_str())?w:nullptr;}
inline bool send(const Packet& p){auto w=panel();if(!w)return false;COPYDATASTRUCT cd{PacketTag,sizeof(p),(void*)&p};DWORD_PTR result{};return SendMessageTimeoutW(w,WM_COPYDATA,0,(LPARAM)&cd,SMTO_ABORTIFHUNG|SMTO_BLOCK,5000,&result)&&result==1;}
}
