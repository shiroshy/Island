// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "overlay.h"
#include "tasks.h"
#include <dwmapi.h>
#include <cstddef>

namespace {
// Documented GDI graphics-query ABI; loaded dynamically for older display drivers.
struct OpenAdapter {HDC dc;UINT adapter;LUID luid;UINT source;};
struct CheckOwnership {UINT adapter,source;};
struct CloseAdapter {UINT adapter;};
static_assert(sizeof(OpenAdapter)==24&&offsetof(OpenAdapter,source)==20);
using OpenFn=LONG (WINAPI*)(OpenAdapter*);
using CheckFn=LONG (WINAPI*)(const CheckOwnership*);
using CloseFn=LONG (WINAPI*)(const CloseAdapter*);
using ExclusiveFn=BYTE (WINAPI*)();
struct DisplayApi {
    HMODULE gdi=GetModuleHandleW(L"gdi32.dll");
    OpenFn open=(OpenFn)GetProcAddress(gdi,"D3DKMTOpenAdapterFromHdc");
    CheckFn check=(CheckFn)GetProcAddress(gdi,"D3DKMTCheckVidPnExclusiveOwnership");
    CloseFn close=(CloseFn)GetProcAddress(gdi,"D3DKMTCloseAdapter");
    ExclusiveFn anyExclusive=(ExclusiveFn)GetProcAddress(gdi,"D3DKMTCheckExclusiveOwnership");
} api;
bool cloaked(HWND hwnd){DWORD value=0;return SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&value,sizeof(value)))&&value;}
}
void Overlay::reset(){if(adapter&&api.close){CloseAdapter close{adapter};api.close(&close);}adapter=0;monitor=nullptr;}
bool Overlay::exclusive(){
    if(!adapter||!api.check)return false;
    if(api.anyExclusive&&!api.anyExclusive())return false;
    CheckOwnership check{adapter,source};LONG status=api.check(&check);
    // STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE: a different DMM client owns this output.
    if((ULONG)status==0xC01E0342u)return true;
    if(status<0)reset();return false;
}
bool Overlay::visible(HWND hwnd){
    if(!hwnd||!IsWindowVisible(hwnd)||IsIconic(hwnd)||cloaked(hwnd))return false;
    auto current=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
    if(current!=monitor){
        reset();monitor=current;
        if(api.open&&api.check&&api.close){MONITORINFOEXW info{};info.cbSize=sizeof(info);if(GetMonitorInfoW(current,(MONITORINFO*)&info)){
            HDC dc=CreateDCW(L"DISPLAY",info.szDevice,nullptr,nullptr);
            if(dc){OpenAdapter open{dc};if(api.open(&open)>=0){adapter=open.adapter;source=open.source;}DeleteDC(dc);}
        }}
    }
    if(exclusive())return false;
    RECT bounds{};GetWindowRect(hwnd,&bounds);
    HRGN remaining=CreateRectRgn(0,0,0,0);
    if(GetWindowRgn(hwnd,remaining)==ERROR)SetRectRgn(remaining,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top);
    OffsetRgn(remaining,bounds.left,bounds.top);
    MONITORINFO info={sizeof(info)};GetMonitorInfoW(current,&info);
    HRGN screen=CreateRectRgnIndirect(&info.rcMonitor);CombineRgn(remaining,remaining,screen,RGN_AND);DeleteObject(screen);
    HRGN cover=CreateRectRgn(0,0,0,0);bool covered=false;
    for(HWND above=GetWindow(hwnd,GW_HWNDPREV);above;above=GetWindow(above,GW_HWNDPREV)){
        if(!IsWindowVisible(above)||IsIconic(above)||cloaked(above))continue;
        LONG_PTR style=GetWindowLongPtrW(above,GWL_EXSTYLE);
        if(style&(WS_EX_TRANSPARENT|WS_EX_NOREDIRECTIONBITMAP))continue;
        if(style&WS_EX_LAYERED){COLORREF key;BYTE alpha=0;DWORD flags=0;if(!GetLayeredWindowAttributes(above,&key,&alpha,&flags)||(flags&LWA_COLORKEY)||!(flags&LWA_ALPHA)||alpha!=255)continue;}
        RECT outer{},frame{},overlap{};GetWindowRect(above,&outer);
        if(FAILED(DwmGetWindowAttribute(above,DWMWA_EXTENDED_FRAME_BOUNDS,&frame,sizeof(frame))))frame=outer;
        if(!IntersectRect(&overlap,&bounds,&frame))continue;
        if(GetWindowRgn(above,cover)!=ERROR)OffsetRgn(cover,outer.left,outer.top);else SetRectRgn(cover,frame.left,frame.top,frame.right,frame.bottom);
        if(CombineRgn(remaining,remaining,cover,RGN_DIFF)==NULLREGION){covered=true;break;}
    }
    DeleteObject(cover);DeleteObject(remaining);return !covered;
}
void Overlay::raise(HWND hwnd){
    if(!hwnd||!IsWindowVisible(hwnd)||IsIconic(hwnd))return;
    HWND foreground=GetForegroundWindow();DWORD pid=0;if(foreground)GetWindowThreadProcessId(foreground,&pid);
    if(!isTaskWindow(foreground))return;
    wchar_t cls[100]={};if(foreground)GetClassNameW(foreground,cls,100);
    if(pid==GetCurrentProcessId()||!wcscmp(cls,L"NotifyIconOverflowWindow")||!wcscmp(cls,L"#32768"))return;
    SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
}
