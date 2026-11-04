// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "health.h"
#include "menu.h"
#include <windowsx.h>
#include <shellscalingapi.h>
#include <memory>
#include <cmath>

namespace {
struct Menu {
    HWND hwnd=nullptr;std::unique_ptr<Graphics> gfx;
    const std::vector<MenuItem>* items=nullptr;MenuColors colors{};
    float scale=1;int width=256,height=0,hover=-1,result=0;bool done=false,pressed=false;
    void draw(){
        if(!gfx)return;gfx->outlineColor=colors.outline;gfx->outlineWidth=colors.width;gfx->outlineSmooth=colors.smooth;gfx->begin();gfx->fill({1,1,(float)width-2,(float)height-2},colors.background,12);
        gfx->stroke({1,1,(float)width-2,(float)height-2},colors.card,12);
        for(size_t i=0;i<items->size();i++){
            auto& item=(*items)[i];float y=7+i*35.f;
            if((int)i==hover)gfx->fill({6,y,(float)width-12,33},colors.card,7);
            if(item.icon)gfx->icon(item.icon,{12,y,27,33},14,item.checked?colors.accent:colors.muted);
            gfx->text(item.text,{45,y,(float)width-71,33},12,colors.text);
            if(item.checked)gfx->icon(L"\uE73E",{(float)width-28,y,18,33},11,colors.accent);
        }
        gfx->end();
    }
};
void relocate(Menu* m,POINT anchor){
    MONITORINFO info={sizeof(info)};HMONITOR monitor=MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST);GetMonitorInfoW(monitor,&info);
    UINT dx=96,dy=96;GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&dx,&dy);m->scale=dx/96.f;
    int w=(int)std::ceil(m->width*m->scale),h=(int)std::ceil(m->height*m->scale);
    anchor.x=std::clamp(anchor.x,info.rcWork.left,info.rcWork.right-w);anchor.y=std::clamp(anchor.y,info.rcWork.top,info.rcWork.bottom-h);
    m->hover=-1;m->pressed=false;if(m->gfx)m->gfx->resize(w,h,m->scale);
    SetWindowRgn(m->hwnd,CreateRoundRectRgn(0,0,w,h,(int)(22*m->scale),(int)(22*m->scale)),FALSE);
    SetWindowPos(m->hwnd,HWND_TOPMOST,anchor.x,anchor.y,w,h,SWP_NOACTIVATE);m->draw();
}
LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    auto m=(Menu*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(msg==WM_NCCREATE){m=(Menu*)((CREATESTRUCTW*)lp)->lpCreateParams;m->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)m);}
    if(!m)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_PAINT:{PAINTSTRUCT ps;BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);m->draw();return 0;}
    case WM_ERASEBKGND:return 1;
    case WM_MOUSEMOVE:{float x=GET_X_LPARAM(lp)/m->scale,y=GET_Y_LPARAM(lp)/m->scale;int row=(int)((y-7)/35);if(x<6||x>m->width-6||y<7||y>m->height-7||row<0||row>=(int)m->items->size())row=-1;if(row!=m->hover){m->hover=row;m->draw();}return 0;}
    case WM_LBUTTONDOWN:{float x=GET_X_LPARAM(lp)/m->scale,y=GET_Y_LPARAM(lp)/m->scale;if(x<0||y<0||x>=m->width||y>=m->height){DestroyWindow(hwnd);return 0;}m->pressed=true;return 0;}
    case WM_LBUTTONUP:if(!m->pressed)return 0;if(m->hover>=0)m->result=(*m->items)[m->hover].id;DestroyWindow(hwnd);return 0;
    case WM_RBUTTONUP:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(hwnd,&p);relocate(m,p);return 0;}
    case WM_ACTIVATE:if(LOWORD(wp)==WA_INACTIVE)DestroyWindow(hwnd);return 0;
    case WM_KEYDOWN:
        if(wp==VK_ESCAPE)DestroyWindow(hwnd);
        else if(wp==VK_UP||wp==VK_DOWN){int n=(int)m->items->size();m->hover=(m->hover+(wp==VK_UP?-1:1)+n)%n;m->draw();}
        else if(wp==VK_RETURN&&m->hover>=0){m->result=(*m->items)[m->hover].id;DestroyWindow(hwnd);}
        return 0;
    case WM_CAPTURECHANGED:if(!m->done)DestroyWindow(hwnd);return 0;
    case WM_DESTROY:m->done=true;if(GetCapture()==hwnd)ReleaseCapture();return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}
int customMenu(HWND owner,POINT anchor,const std::vector<MenuItem>& items,MenuColors colors){
    health::Hold protection;if(!protection||items.empty())return 0;
    Menu menu;menu.items=&items;menu.colors=colors;menu.height=14+(int)items.size()*35;
    HMONITOR monitor=MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST);MONITORINFO info={sizeof(info)};GetMonitorInfoW(monitor,&info);
    UINT dx=96,dy=96;GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&dx,&dy);menu.scale=dx/96.f;
    int w=(int)std::ceil(menu.width*menu.scale),h=(int)std::ceil(menu.height*menu.scale);
    anchor.x=std::clamp(anchor.x,info.rcWork.left,info.rcWork.right-w);anchor.y=std::clamp(anchor.y,info.rcWork.top,info.rcWork.bottom-h);
    WNDCLASSW cls={};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"OwnIsland.Menu";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);
    // A popup owned by an Explorer child raises the shared desktop host on all monitors.
    // Keep desktop menus independent; mini-panel menus retain their normal owner.
    HWND menuOwner=owner;DWORD rootPid=0;GetWindowThreadProcessId(GetAncestor(owner,GA_ROOT),&rootPid);wchar_t ownerClass[100]={};GetClassNameW(owner,ownerClass,100);if(rootPid!=GetCurrentProcessId()||!wcscmp(ownerClass,L"OwnIsland.Desktop"))menuOwner=nullptr;
    HWND hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOREDIRECTIONBITMAP,cls.lpszClassName,L"",WS_POPUP,anchor.x,anchor.y,w,h,menuOwner,nullptr,cls.hInstance,&menu);
    if(!hwnd)return 0;
    menu.gfx=std::make_unique<Graphics>();
    if(!menu.gfx->init(hwnd,w,h,menu.scale)){DestroyWindow(hwnd);return 0;}
    HRGN rgn=CreateRoundRectRgn(0,0,w,h,(int)(22*menu.scale),(int)(22*menu.scale));SetWindowRgn(hwnd,rgn,FALSE);
    menu.draw();ShowWindow(hwnd,SW_SHOWNORMAL);SetForegroundWindow(hwnd);SetCapture(hwnd);
    MSG msg;
    while(!menu.done){BOOL status=GetMessageW(&msg,nullptr,0,0);if(status<=0){if(status==0)PostQuitMessage((int)msg.wParam);break;}TranslateMessage(&msg);DispatchMessageW(&msg);}
    if(IsWindow(hwnd))DestroyWindow(hwnd);
    if(menuOwner&&IsWindow(menuOwner)&&!wcscmp(ownerClass,L"OwnIsland.Files")&&!GetForegroundWindow()){SetForegroundWindow(menuOwner);SetFocus(menuOwner);}
    return menu.result;
}
