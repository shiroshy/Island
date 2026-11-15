// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>

struct NativeTrayGrid {int columns=0,rows=0,width=0,height=0;};
inline NativeTrayGrid nativeTrayGrid(int count,int cellWidth,int cellHeight,int availableWidth,int availableHeight){
    if(count<1||count>4096||cellWidth<1||cellHeight<1)return {};
    int maxColumns=availableWidth/cellWidth,maxRows=availableHeight/cellHeight;
    if(maxColumns<1||maxRows<1||(long long)count>(long long)maxColumns*maxRows)return {};
    int columns=std::clamp((int)std::ceil(std::sqrt((double)count)),1,maxColumns);
    if((count+columns-1)/columns>maxRows)columns=(count+maxRows-1)/maxRows;
    int rows=(count+columns-1)/columns;return {columns,rows,columns*cellWidth,rows*cellHeight};
}

// Standard toolbar messages return scalar values: no remote pointers, memory
// writes, injected code, subclassing or competing taskbar-button positioning.
inline bool fitNativeTrayOverflow(HWND popup,RECT work){
    if(!IsWindow(popup))return false;wchar_t cls[64]={};GetClassNameW(popup,cls,64);if(wcscmp(cls,L"NotifyIconOverflowWindow"))return false;
    HWND toolbar=FindWindowExW(popup,nullptr,TOOLBARCLASSNAMEW,nullptr);if(!toolbar)return false;
    DWORD popupPid=0,toolbarPid=0;GetWindowThreadProcessId(popup,&popupPid);GetWindowThreadProcessId(toolbar,&toolbarPid);if(!popupPid||popupPid!=toolbarPid)return false;
    DWORD_PTR resized=0;if(!SendMessageTimeoutW(toolbar,TB_AUTOSIZE,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&resized))return false;
    DWORD_PTR count=0,cell=0;if(!SendMessageTimeoutW(toolbar,TB_BUTTONCOUNT,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&count)||count<1||count>4096||!SendMessageTimeoutW(toolbar,TB_GETBUTTONSIZE,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&cell))return false;
    RECT outer{},client{},child{};if(!GetWindowRect(popup,&outer)||!GetClientRect(popup,&client)||!GetWindowRect(toolbar,&child))return false;MapWindowPoints(nullptr,popup,(POINT*)&child,2);
    // Explorer keeps hidden placeholder buttons in TB_BUTTONCOUNT. The native
    // wrapping rows provide a safe upper bound on visible cells without reading
    // private button records from another process.
    DWORD_PTR nativeRows=0;int currentColumns=(child.right-child.left)/std::max(1,(int)LOWORD(cell));
    if(currentColumns>0&&SendMessageTimeoutW(toolbar,TB_GETROWS,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&nativeRows)&&nativeRows>0&&nativeRows<=4096)
        count=std::min<DWORD_PTR>(count,nativeRows*currentColumns);
    int left=std::max(0L,child.left),top=std::max(0L,child.top),right=std::max(0L,client.right-child.right),bottom=std::max(0L,client.bottom-child.bottom);
    int frameW=outer.right-outer.left-client.right,frameH=outer.bottom-outer.top-client.bottom;
    auto grid=nativeTrayGrid((int)count,LOWORD(cell),HIWORD(cell),work.right-work.left-left-right-frameW,work.bottom-work.top-top-bottom-frameH);if(!grid.columns)return false;
    int width=grid.width+left+right+frameW,height=grid.height+top+bottom+frameH;
    if(outer.right-outer.left!=width||outer.bottom-outer.top!=height)
        if(!SetWindowPos(popup,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER))return false;
    RECT current{};GetWindowRect(toolbar,&current);MapWindowPoints(nullptr,popup,(POINT*)&current,2);
    if(current.left!=left||current.top!=top||current.right-current.left!=grid.width||current.bottom-current.top!=grid.height)
        if(!SetWindowPos(toolbar,nullptr,left,top,grid.width,grid.height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER))return false;
    return true;
}
