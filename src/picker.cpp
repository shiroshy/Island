// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <string>
#include <algorithm>

namespace {
struct Picker {
    HWND hwnd=nullptr;HBITMAP capture=nullptr;HDC memory=nullptr;HGDIOBJ old=nullptr;
    int width=0,height=0;bool done=false;std::wstring result;
    ~Picker(){if(memory){SelectObject(memory,old);DeleteDC(memory);}if(capture)DeleteObject(capture);}
};
LRESULT CALLBACK pickerProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    auto p=(Picker*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(msg==WM_NCCREATE){p=(Picker*)((CREATESTRUCTW*)lp)->lpCreateParams;p->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)p);}
    if(!p)return DefWindowProcW(hwnd,msg,wp,lp);
    if(msg==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);BitBlt(dc,0,0,p->width,p->height,p->memory,0,0,SRCCOPY);RECT box={p->width/2-220,35,p->width/2+220,79};HBRUSH b=CreateSolidBrush(RGB(16,16,18));FillRect(dc,&box,b);DeleteObject(b);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,242));DrawTextW(dc,L"Пипетка · выберите цвет кликом · Esc — отмена",-1,&box,DT_CENTER|DT_VCENTER|DT_SINGLELINE);EndPaint(hwnd,&ps);return 0;}
    if(msg==WM_LBUTTONDOWN){COLORREF color=GetPixel(p->memory,GET_X_LPARAM(lp),GET_Y_LPARAM(lp));wchar_t text[16];swprintf(text,16,L"#%02X%02X%02X",GetRValue(color),GetGValue(color),GetBValue(color));p->result=text;DestroyWindow(hwnd);return 0;}
    if(msg==WM_KEYDOWN&&wp==VK_ESCAPE){DestroyWindow(hwnd);return 0;}
    if(msg==WM_DESTROY){p->done=true;return 0;}
    if(msg==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_CROSS));return TRUE;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}
std::wstring pickScreenColor() {
    Picker p;
    int left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
    p.width=GetSystemMetrics(SM_CXVIRTUALSCREEN);p.height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC screen=GetDC(nullptr);p.memory=CreateCompatibleDC(screen);p.capture=CreateCompatibleBitmap(screen,p.width,p.height);p.old=SelectObject(p.memory,p.capture);BitBlt(p.memory,0,0,p.width,p.height,screen,left,top,SRCCOPY|CAPTUREBLT);ReleaseDC(nullptr,screen);
    WNDCLASSW cls={};cls.lpfnWndProc=pickerProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"OwnIsland.ColorPicker";RegisterClassW(&cls);
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,cls.lpszClassName,L"Пипетка",WS_POPUP,left,top,p.width,p.height,nullptr,nullptr,cls.hInstance,&p);
    if(!window)return {};
    ShowWindow(window,SW_SHOW);SetForegroundWindow(window);
    MSG msg;while(!p.done&&GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    return p.result;
}
