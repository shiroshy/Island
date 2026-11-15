// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <filesystem>
#include <fstream>
std::wstring quote(const std::wstring& s){return L"\""+s+L"\"";}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;if(m==WM_WINDOWPOSCHANGED&&GetParent(w)&&GetPropW(GetParent(w),L"Island.TestPauseOnAttach")&&!GetPropW(w,L"Island.TestAttached")){SetPropW(w,L"Island.TestAttached",(HANDLE)1);DWORD_PTR result=0;SendMessageTimeoutW(GetParent(w),WM_APP+71,0,0,SMTO_ABORTIFHUNG,2000,&result);}if(m==WM_TIMER){InvalidateRect(w,nullptr,FALSE);return 0;}if(m==WM_PAINT){PAINTSTRUCT ps;auto dc=BeginPaint(w,&ps);RECT r;GetClientRect(w,&r);auto brush=CreateSolidBrush(RGB((GetTickCount64()/100)%100,230,40));FillRect(dc,&r,brush);DeleteObject(brush);RECT lower=r;lower.top=r.bottom/2;brush=CreateSolidBrush(RGB(40,20,230));FillRect(dc,&lower,brush);DeleteObject(brush);EndPaint(w,&ps);return 0;}if(m==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(w,m,a,b);}
int wmain(int argc,wchar_t** argv){
    if(argc>1&&!wcscmp(argv[1],L"-control")){
        std::wstring action=argc>2?argv[2]:L"",title;int width=320,height=180;for(int i=3;i+1<argc;i++){std::wstring key=argv[i];if(key==L"-playInWindow"||key==L"-location")title=argv[++i];else if(key==L"-width")width=_wtoi(argv[++i]);else if(key==L"-height")height=_wtoi(argv[++i]);}
        if(action==L"getWallpaper"){wchar_t self[32768];GetModuleFileNameW(nullptr,self,32768);auto root=std::filesystem::path(self).parent_path();auto initial=(root/L"project.json").wstring();wchar_t path[32768]={};GetPrivateProfileStringW(L"fixture",L"current-file",initial.c_str(),path,32768,(root/L"settings.ini").c_str());std::wstring s=path;DWORD n;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"\xFF\xFE",2,&n,nullptr);WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data(),(DWORD)s.size()*2,&n,nullptr);return 0;}
        if(action==L"closeWallpaper"){if(auto window=FindWindowW(L"Island.EngineFixture",title.c_str()))PostMessageW(window,WM_CLOSE,0,0);return 0;}
        if(action==L"openWallpaper"){wchar_t self[32768];GetModuleFileNameW(nullptr,self,32768);auto command=quote(self)+L" --renderer "+quote(title)+L" "+std::to_wstring(width)+L" "+std::to_wstring(height);STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION p{};if(!CreateProcessW(self,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&p))return 2;CloseHandle(p.hThread);CloseHandle(p.hProcess);return 0;}
        return 3;
    }
    WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=proc;cls.lpszClassName=L"Island.EngineFixture";RegisterClassW(&cls);bool renderer=argc>2&&!wcscmp(argv[1],L"--renderer");auto w=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,renderer?argv[2]:L"EngineFixture.Main",WS_POPUP,-30000,-30000,renderer?_wtoi(argv[3]):16,renderer?_wtoi(argv[4]):16,nullptr,nullptr,cls.hInstance,nullptr);if(renderer){ShowWindow(w,SW_SHOWNOACTIVATE);SetTimer(w,1,33,nullptr);UpdateWindow(w);}MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return 0;
}
