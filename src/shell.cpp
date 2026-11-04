// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "health.h"
#include "shell.h"
#include "runtime.h"
#include "file_browser.h"
#include "native_desktop.h"
#include "platform.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <filesystem>

namespace {
bool protectionStarted=false,protectionReady=false;
HWND keyOwner=nullptr;
HWND shellOwner=nullptr;
bool overflowPending=false;
HWND overflowObserved=nullptr;
std::atomic<ULONGLONG> trayContextUntil=0;
std::atomic<bool> trayContextEnding=false;
WinKeyState winKeys;
DWORD ownedChordKey=0;
DesktopLanguageChord languageKeys;
HWND languageRoot=nullptr;HKL languageBefore=nullptr;
HWND desktopInputRoot(){
    HWND active=GetForegroundWindow();if(active)return desktopLanguageTarget(active)?active:nullptr;
    POINT point{};GetCursorPos(&point);HWND target=WindowFromPoint(point);
    for(HWND w=target;w;w=GetParent(w)){wchar_t name[100]={};GetClassNameW(w,name,100);if(!wcscmp(name,L"OwnIsland.Desktop"))return GetAncestor(w,GA_ROOT);}
    return nullptr;
}
LRESULT CALLBACK trayMouse(int code,WPARAM message,LPARAM data){
    if(code==HC_ACTION){
        auto now=GetTickCount64();auto& mouse=*(MSLLHOOKSTRUCT*)data;
        if(message==WM_RBUTTONDOWN){HWND popup=FindWindowW(L"NotifyIconOverflowWindow",nullptr);RECT bounds{};if(popup&&IsWindowVisible(popup)&&GetWindowRect(popup,&bounds)&&PtInRect(&bounds,mouse.pt)){trayContextEnding=false;trayContextUntil=now+5000;}}
        if(trayContextUntil>now){
            if(message==WM_MOUSEMOVE&&!trayContextEnding)trayContextUntil=now+5000;
            if(message==WM_LBUTTONDOWN){trayContextEnding=true;trayContextUntil=now+300;}
        }
    }
    return CallNextHookEx(nullptr,code,message,data);
}
void CALLBACK overflowChanged(HWINEVENTHOOK,DWORD event,HWND hwnd,LONG object,LONG child,DWORD,DWORD){
    if(object!=OBJID_WINDOW||child!=CHILDID_SELF||!hwnd)return;
    if(event!=EVENT_OBJECT_SHOW&&event!=EVENT_OBJECT_HIDE&&event!=EVENT_OBJECT_LOCATIONCHANGE&&event!=EVENT_OBJECT_DESTROY)return;
    wchar_t cls[100]={};GetClassNameW(hwnd,cls,100);
    if(!wcscmp(cls,L"NotifyIconOverflowWindow")||hwnd==overflowObserved){
        overflowObserved=event==EVENT_OBJECT_DESTROY?nullptr:hwnd;
        if(!overflowPending){overflowPending=true;PostMessageW(shellOwner,WM_APP+8,0,0);}
    }
}
void CALLBACK shellShown(HWINEVENTHOOK,DWORD,HWND hwnd,LONG object,LONG child,DWORD,DWORD){
    if(object!=OBJID_WINDOW||child!=CHILDID_SELF||!hwnd)return;
    wchar_t cls[100]={};GetClassNameW(hwnd,cls,100);
    if(!wcscmp(cls,L"Shell_TrayWnd")||!wcscmp(cls,L"Shell_SecondaryTrayWnd"))PostMessageW(shellOwner,WM_APP+7,0,0);
}
LRESULT CALLBACK keyboard(int code,WPARAM msg,LPARAM data){
    if(code==HC_ACTION){
        auto& key=*(KBDLLHOOKSTRUCT*)data;
        HWND foreground=desktopInputRoot();bool desktop=GetPropW(keyOwner,L"OwnIsland.DesktopCount")&&foreground;
        if(desktop){if(!languageKeys.alt&&!languageKeys.shift){languageRoot=foreground;languageBefore=GetKeyboardLayout(GetWindowThreadProcessId(foreground,nullptr));}bool blocked=((GetAsyncKeyState(VK_CONTROL)|GetAsyncKeyState(VK_LWIN)|GetAsyncKeyState(VK_RWIN))&0x8000)!=0;
            if(languageKeys.event(key.vkCode,msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN,blocked))PostMessageW(keyOwner,WM_APP+31,(WPARAM)languageRoot,(LPARAM)languageBefore);
        }else languageKeys={};
        if(!(key.flags&LLKHF_INJECTED)){
            bool down=msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN;
            if(down&&trayContextUntil>GetTickCount64()){
                if(key.vkCode==VK_ESCAPE||key.vkCode==VK_RETURN||(key.vkCode==VK_TAB&&(GetAsyncKeyState(VK_MENU)&0x8000))){trayContextEnding=true;trayContextUntil=GetTickCount64()+250;}
                else if(!trayContextEnding)trayContextUntil=GetTickCount64()+5000;
            }
            bool modifiers=(GetAsyncKeyState(VK_CONTROL)|GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_MENU))&0x8000;
            int shortcut=StandaloneMode&&winKeys.held&&down?standaloneShortcut(key.vkCode,modifiers):-1;
            if(shortcut>=0){winKeys.event(key.vkCode,true,modifiers);ownedChordKey=key.vkCode;PostMessageW(keyOwner,WM_APP+32,shortcut,0);return 1;}
            if(ownedChordKey&&key.vkCode==ownedChordKey&&!down)return 1;
            bool ownedRelease=ownedChordKey&&!down&&(key.vkCode==VK_LWIN||key.vkCode==VK_RWIN);
            bool solo=winKeys.event(key.vkCode,down,modifiers);
            if(ownedRelease&&!winKeys.held)ownedChordKey=0;
            if(solo||ownedRelease){
                // Release Win with a harmless Ctrl mask, suppressing Explorer's Start menu.
                INPUT inputs[3]={};for(auto& input:inputs)input.type=INPUT_KEYBOARD;
                inputs[0].ki.wVk=VK_LCONTROL;
                inputs[1].ki.wVk=(WORD)key.vkCode;inputs[1].ki.dwFlags=KEYEVENTF_KEYUP|KEYEVENTF_EXTENDEDKEY;
                inputs[2].ki.wVk=VK_LCONTROL;inputs[2].ki.dwFlags=KEYEVENTF_KEYUP;
                if(SendInput(3,inputs,sizeof(INPUT))==3){if(solo)PostMessageW(keyOwner,WM_APP+6,0,0);return 1;}
            }
        }
    }
    return CallNextHookEx(nullptr,code,msg,data);
}
std::wstring stateFile(){return stateDirectory()+L"\\shell-restore.ini";}
struct RestoreLock {
    HANDLE handle=CreateMutexW(nullptr,FALSE,L"Local\\OwnIsland.ShellRestoreLock");
    RestoreLock(){if(handle)WaitForSingleObject(handle,INFINITE);}
    ~RestoreLock(){if(handle){ReleaseMutex(handle);CloseHandle(handle);}}
};
int read(const std::wstring& file,const wchar_t* key,int fallback=0){return GetPrivateProfileIntW(L"shell",key,fallback,file.c_str());}
bool write(const std::wstring& file,const wchar_t* key,int value){return WritePrivateProfileStringW(L"shell",key,std::to_wstring(value).c_str(),file.c_str())!=FALSE;}
HWND nativeTaskbar(){HWND found=nullptr;EnumWindows([](HWND w,LPARAM p)->BOOL{wchar_t cls[64]={};GetClassNameW(w,cls,64);if(!wcscmp(cls,L"Shell_TrayWnd")&&!isIslandTrayEndpoint(w)){*(HWND*)p=w;return FALSE;}return TRUE;},(LPARAM)&found);return found;}
void bars(bool visible){
    EnumWindows([](HWND hwnd,LPARAM show)->BOOL{wchar_t cls[100]={};GetClassNameW(hwnd,cls,100);if((!wcscmp(cls,L"Shell_TrayWnd")||!wcscmp(cls,L"Shell_SecondaryTrayWnd"))&&!isIslandTrayEndpoint(hwnd)&&((IsWindowVisible(hwnd)!=FALSE)!=(show!=0)))ShowWindow(hwnd,show?SW_SHOWNOACTIVATE:SW_HIDE);return TRUE;},visible);
}
void restore(const std::wstring& file){
    nativeDesktopRecover(file);
    if(!read(file,L"armed"))return;
    APPBARDATA data={sizeof(data)};data.hWnd=nativeTaskbar();data.lParam=read(file,L"state",ABS_ALWAYSONTOP);
    if(read(file,L"had-taskbar",1)){SHAppBarMessage(ABM_SETSTATE,&data);bars(read(file,L"visible",1)!=0);}
    write(file,L"armed",0);
}
void recoveryLog(const wchar_t* stage,DWORD pid){
    auto file=stateDirectory()+L"\\recovery.log";WIN32_FILE_ATTRIBUTE_DATA a{};
    if(GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&a)&&(a.nFileSizeHigh||a.nFileSizeLow>262144))MoveFileExW(file.c_str(),(stateDirectory()+L"\\recovery.previous.log").c_str(),MOVEFILE_REPLACE_EXISTING);
    HANDLE h=CreateFileW(file.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return;
    SYSTEMTIME t{};GetLocalTime(&t);char line[160];int count=snprintf(line,sizeof(line),"%04u-%02u-%02u %02u:%02u:%02u %ls pid=%lu\r\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,stage,pid);DWORD written;if(count>0)WriteFile(h,line,std::min(count,159),&written,nullptr);CloseHandle(h);
}
bool processAlive(DWORD pid){HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);if(!process)return false;bool alive=WaitForSingleObject(process,0)==WAIT_TIMEOUT;CloseHandle(process);return alive;}
bool sizedOverflow(HWND popup,RECT& bounds){return popup&&GetWindowRect(popup,&bounds)&&bounds.right-bounds.left>=20&&bounds.bottom-bounds.top>=20;}
HWND initializeOverflow(){
    // A fresh Explorer session can leave the overflow at 0x0 until its first
    // chevron command. Initialize once with a bounded synchronous notification;
    // normal opens never invoke the taskbar's competing placement logic.
    HWND bar=nativeTaskbar();
    HWND notify=bar?FindWindowExW(bar,nullptr,L"TrayNotifyWnd",nullptr):nullptr;
    HWND button=notify?GetDlgItem(notify,1502):nullptr;
    wchar_t cls[32]={};if(button)GetClassNameW(button,cls,32);
    if(!button||wcscmp(cls,L"Button")||!IsWindowEnabled(button))return nullptr;
    HWND before=FindWindowW(L"NotifyIconOverflowWindow",nullptr);
    DWORD cloaked=0;BOOL cloak=TRUE;
    bool concealed=before&&SUCCEEDED(DwmGetWindowAttribute(before,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&!(cloaked&DWM_CLOAKED_APP)&&SUCCEEDED(DwmSetWindowAttribute(before,DWMWA_CLOAK,&cloak,sizeof(cloak)));
    DWORD_PTR result=0;
    SendMessageTimeoutW(notify,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(button),BN_CLICKED),(LPARAM)button,SMTO_ABORTIFHUNG|SMTO_BLOCK,500,&result);
    HWND popup=FindWindowW(L"NotifyIconOverflowWindow",nullptr);
    if(popup)ShowWindow(popup,SW_HIDE);
    if(concealed&&IsWindow(before)){cloak=FALSE;DwmSetWindowAttribute(before,DWMWA_CLOAK,&cloak,sizeof(cloak));}
    RECT bounds{};return sizedOverflow(popup,bounds)?popup:nullptr;
}
}
bool desktopLanguageTarget(HWND window){wchar_t cls[100]={};if(!window)return false;GetClassNameW(window,cls,100);return !wcscmp(cls,L"Progman")||!wcscmp(cls,L"WorkerW")||!wcscmp(cls,L"OwnIsland.Desktop")||!wcscmp(cls,L"OwnIsland.DesktopHost")||!wcscmp(cls,L"OwnIsland.DesktopController");}
void synchronizeDesktopLanguage(HWND root,HKL before){
    if(!IsWindow(root)||desktopInputRoot()!=root||!desktopLanguageTarget(root))return;
    HKL current=GetKeyboardLayout(GetWindowThreadProcessId(root,nullptr)),desired=current;
    if(current==before){int count=GetKeyboardLayoutList(0,nullptr);if(count<2)return;std::vector<HKL> layouts(count);GetKeyboardLayoutList(count,layouts.data());auto found=std::find(layouts.begin(),layouts.end(),current);desired=layouts[(found==layouts.end()?0:(found-layouts.begin()+1))%count];PostMessageW(root,WM_INPUTLANGCHANGEREQUEST,0,(LPARAM)desired);}
    ActivateKeyboardLayout(desired,0);
}
int shellWatch(DWORD parent){
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,parent);if(!process)return 1;
    auto eventName=L"Local\\OwnIsland.ShellReady."+std::to_wstring(parent);
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,eventName.c_str());if(!ready){CloseHandle(process);return 2;}
    SetEvent(ready);CloseHandle(ready);
    HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,health::name(parent).c_str());
    auto observed=mapping?(health::Shared*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(health::Shared)):nullptr;
    FILETIME created{},exit{},kernel{},user{};GetProcessTimes(process,&created,&exit,&kernel,&user);
    bool valid=observed&&observed->magic==health::Magic&&observed->pid==parent&&CompareFileTime(&created,&observed->created)==0;
    ULONGLONG failedSince=0,last=GetTickCount64();
    while(WaitForSingleObject(process,1000)==WAIT_TIMEOUT){
        auto now=GetTickCount64();bool gap=now-last>5000;last=now;
        if(!valid||gap||!health::read(observed->ready)||!health::read(observed->awake)||health::read(observed->stopping)||health::read(observed->gate)!=0){failedSince=0;continue;}
        HWND root=(HWND)(UINT_PTR)(DWORD)health::read(observed->window);DWORD pid=0;DWORD thread=GetWindowThreadProcessId(root,&pid);GUITHREADINFO gui{sizeof(gui)};
        if(pid!=parent||!thread||!IsWindowEnabled(root)||!GetGUIThreadInfo(thread,&gui)||(gui.flags&(GUI_INMENUMODE|GUI_INMOVESIZE|GUI_POPUPMENUMODE|GUI_SYSTEMMENUMODE))||gui.hwndCapture){failedSince=0;continue;}
        DWORD_PTR answer=0;
        if(SendMessageTimeoutW(root,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,600,&answer)){failedSince=0;continue;}
        if(!failedSince)failedSince=now;
        if(now-failedSince<30000)continue;
        if(InterlockedCompareExchange(&observed->gate,-1,0)!=0){failedSince=0;continue;}
        bool eligible=health::read(observed->awake)&&!health::read(observed->stopping)&&IsWindowEnabled(root);
        // Claiming gate closes the race with a file operation starting after the last probe.
        if(eligible&&!SendMessageTimeoutW(root,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,300,&answer)){
            if(TerminateProcess(process,ERROR_TIMEOUT)){recoveryLog(L"unresponsive-ui",parent);WaitForSingleObject(process,5000);break;}
        }
        InterlockedExchange(&observed->gate,0);failedSince=0;
    }
    if(observed)UnmapViewOfFile(observed);if(mapping)CloseHandle(mapping);CloseHandle(process);
    RestoreLock lock;auto file=stateFile();if((DWORD)read(file,L"pid")!=parent)return 0;
    bool recover=read(file,L"standalone")&&!read(file,L"shutdown");restore(file);
    if(recover){
        wchar_t value[64]={};GetPrivateProfileStringW(L"shell",L"crash-window",L"0",value,64,file.c_str());auto previous=_wcstoui64(value,nullptr,10),now=GetTickCount64();int crashes=now-previous<60000?read(file,L"crashes")+1:1;
        recoveryLog(crashes<=3?L"restart":L"restart-limit",parent);
        write(file,L"crashes",crashes);if(crashes==1)WritePrivateProfileStringW(L"shell",L"crash-window",std::to_wstring(now).c_str(),file.c_str());
        wchar_t executable[32768]={};GetModuleFileNameW(nullptr,executable,32768);std::wstring command=L"\""+std::wstring(executable)+L"\""+runtimeArguments();
        STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION child{};
        if(crashes<=3&&CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&child)){CloseHandle(child.hThread);CloseHandle(child.hProcess);}
        else if(!nativeTaskbar()&&!read(file,L"no-explorer-fallback"))ShellExecuteW(nullptr,L"open",L"explorer.exe",nullptr,nullptr,SW_SHOWNORMAL);
    }
    return 0;
}
bool startShellProtection(){
    if(protectionStarted)return true;
    if(!health::initialize())return false;
    auto state=stateFile();
    RestoreLock lock;
    if(read(state,L"armed")&&!processAlive((DWORD)read(state,L"pid")))restore(state);
    APPBARDATA data={sizeof(data)};data.hWnd=nativeTaskbar();
    if(!data.hWnd&&!StandaloneMode)return false;
    if(!write(state,L"pid",(int)GetCurrentProcessId())||!write(state,L"state",(int)SHAppBarMessage(ABM_GETSTATE,&data))||!write(state,L"visible",IsWindowVisible(data.hWnd))||!write(state,L"had-taskbar",data.hWnd?1:0)||!write(state,L"standalone",StandaloneMode?1:0)||!write(state,L"shutdown",0))return false;
    auto eventName=L"Local\\OwnIsland.ShellReady."+std::to_wstring(GetCurrentProcessId());HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str());
    wchar_t path[32768]={};GetModuleFileNameW(nullptr,path,32768);std::wstring command=L"\""+std::wstring(path)+L"\" --shell-watch "+std::to_wstring(GetCurrentProcessId())+runtimeArguments();
    STARTUPINFOW start={sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION child={};
    bool launched=ready&&CreateProcessW(path,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&child);
    bool guarded=launched&&WaitForSingleObject(ready,4000)==WAIT_OBJECT_0;
    if(launched){CloseHandle(child.hThread);CloseHandle(child.hProcess);}if(ready)CloseHandle(ready);
    if(!guarded)return false;
    if(!write(state,L"armed",1))return false;protectionStarted=true;return true;
}
void shellProtectionReady(){protectionReady=true;}
void stopShellProtection(){health::stopping(true);if(!protectionStarted||!protectionReady)return;RestoreLock lock;auto file=stateFile();if((DWORD)read(file,L"pid")==GetCurrentProcessId()){write(file,L"shutdown",1);write(file,L"crashes",0);restore(file);}protectionStarted=false;}
bool ShellPanel::start(HWND target){
    if(active)return true;owner=target;state=stateFile();
    keyReadyEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!keyReadyEvent)return false;keyStopping=false;
    keyThread=std::thread([this]{
        keyOwner=owner;winKeys={};ownedChordKey=0;trayContextUntil=0;trayContextEnding=false;MSG msg;PeekMessageW(&msg,nullptr,WM_USER,WM_USER,PM_NOREMOVE);keyThreadId=GetCurrentThreadId();
        HHOOK hook=keyStopping?nullptr:SetWindowsHookExW(WH_KEYBOARD_LL,keyboard,GetModuleHandleW(nullptr),0);
        HHOOK mouse=nullptr;
        keyInstalled=hook!=nullptr;SetEvent(keyReadyEvent);
        if(hook){while(!keyStopping&&GetMessageW(&msg,nullptr,0,0)>0){if(msg.message==WM_APP+30){if(msg.wParam&&!mouse)mouse=SetWindowsHookExW(WH_MOUSE_LL,trayMouse,GetModuleHandleW(nullptr),0);else if(!msg.wParam&&mouse){UnhookWindowsHookEx(mouse);mouse=nullptr;}}}if(mouse)UnhookWindowsHookEx(mouse);UnhookWindowsHookEx(hook);}keyOwner=nullptr;
    });
    bool keyReady=WaitForSingleObject(keyReadyEvent,3000)==WAIT_OBJECT_0;
    if(!keyReady||!keyInstalled){stop();return false;}
    if(!startShellProtection()){stop();return false;}
    APPBARDATA data={sizeof(data)};data.hWnd=nativeTaskbar();
    write(state,L"armed",1);active=true;if(data.hWnd){data.lParam=read(state,L"state")|ABS_AUTOHIDE;SHAppBarMessage(ABM_SETSTATE,&data);}
    shellOwner=owner;showHook=SetWinEventHook(EVENT_OBJECT_SHOW,EVENT_OBJECT_SHOW,nullptr,shellShown,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    overflowObserved=FindWindowW(L"NotifyIconOverflowWindow",nullptr);overflowPending=false;
    overflowHook=SetWinEventHook(EVENT_OBJECT_DESTROY,EVENT_OBJECT_LOCATIONCHANGE,nullptr,overflowChanged,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    maintain();return true;
}
void ShellPanel::stop(){
    ownTray.stop();
    if(overflowHook){UnhookWinEvent(overflowHook);overflowHook=nullptr;}overflowPending=false;overflowObserved=nullptr;
    if(showHook){UnhookWinEvent(showHook);showHook=nullptr;}if(active)shellOwner=nullptr;
    keyStopping=true;if(keyThread.joinable()){if(keyThreadId)PostThreadMessageW(keyThreadId,WM_QUIT,0,0);keyThread.join();}keyInstalled=false;keyThreadId=0;
    if(keyReadyEvent){CloseHandle(keyReadyEvent);keyReadyEvent=nullptr;}
    if(active){RestoreLock lock;if((DWORD)read(state,L"pid")==GetCurrentProcessId()){if(protectionReady){write(state,L"shutdown",1);write(state,L"crashes",0);}restore(state);}active=false;}
}
void ShellPanel::maintain(){if(active){
    if(StandaloneMode){if(!nativeTaskbar()){if(!ownTray.hosted())ownTray.start(owner);ownTray.colors(trayColors);ownTray.maintain();}else if(ownTray.hosted()){ownTray.stop();PostMessageW(HWND_BROADCAST,RegisterWindowMessageW(L"TaskbarCreated"),0,0);}}
    SetPropW(owner,L"OwnIsland.TrayHosted",(HANDLE)(UINT_PTR)(ownTray.hosted()?1:0));SetPropW(owner,L"OwnIsland.TrayCount",(HANDLE)(UINT_PTR)ownTray.count());APPBARDATA data={sizeof(data)};data.hWnd=nativeTaskbar();auto current=data.hWnd?SHAppBarMessage(ABM_GETSTATE,&data):0;if(data.hWnd&&!(current&ABS_AUTOHIDE)){data.lParam=current|ABS_AUTOHIDE;SHAppBarMessage(ABM_SETSTATE,&data);}bars(false);}}
bool ShellPanel::overflowVisible()const{if(ownTray.hosted())return ownTray.visible();HWND popup=FindWindowW(L"NotifyIconOverflowWindow",nullptr);return popup&&IsWindowVisible(popup);}
void ShellPanel::monitorOverflowMouse(bool enabled){if(keyThreadId)PostThreadMessageW(keyThreadId,WM_APP+30,enabled,0);}
bool ShellPanel::contextInteraction()const{return ownTray.contextInteraction()||trayContextUntil>GetTickCount64();}
void ShellPanel::closeOverflow(){ownTray.close();trayContextUntil=0;trayContextEnding=false;overflowAnchored=false;monitorOverflowMouse(false);HWND popup=FindWindowW(L"NotifyIconOverflowWindow",nullptr);if(popup&&IsWindowVisible(popup))ShowWindow(popup,SW_HIDE);}
bool ShellPanel::dismissInactiveOverflow(){
    if(ownTray.hosted()){bool before=ownTray.visible();ownTray.maintain();return before&&!ownTray.visible();}
    if(!overflowAnchored||contextInteraction())return false;
    HWND fg=GetForegroundWindow();wchar_t cls[100]={};if(fg)GetClassNameW(fg,cls,100);
    if(isTaskWindow(fg)||!wcscmp(cls,L"WorkerW")||!wcscmp(cls,L"Progman")){closeOverflow();return true;}return false;
}
void ShellPanel::updateOverflow(){
    if(ownTray.hosted()){overflowPending=false;ownTray.maintain();return;}
    overflowPending=false;HWND popup=FindWindowW(L"NotifyIconOverflowWindow",nullptr);
    if(!popup||!IsWindowVisible(popup)){
        if(popup&&overflowAnchored&&contextInteraction()&&!trayContextEnding){SetWindowPos(popup,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);return;}
        overflowAnchored=false;monitorOverflowMouse(false);return;
    }
    if(!overflowAnchored)return;
    RECT bounds{};GetWindowRect(popup,&bounds);int w=bounds.right-bounds.left,h=bounds.bottom-bounds.top;
    MONITORINFO monitor={sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(overflowAnchor,MONITOR_DEFAULTTONEAREST),&monitor);
    int x=std::clamp(overflowAnchor.x-w,monitor.rcWork.left,std::max(monitor.rcWork.left,monitor.rcWork.right-w));
    int y=std::clamp(overflowAnchor.y,monitor.rcWork.top,std::max(monitor.rcWork.top,monitor.rcWork.bottom-h));
    if(bounds.left!=x||bounds.top!=y)SetWindowPos(popup,HWND_TOPMOST,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
}
bool ShellPanel::hiddenIcons(POINT anchor){
    if(ownTray.hosted())return ownTray.toggle(anchor);
    HWND popup=FindWindowW(L"NotifyIconOverflowWindow",nullptr);
    if(popup&&IsWindowVisible(popup)){closeOverflow();return true;}
    RECT bounds{};
    if(!sizedOverflow(popup,bounds)){popup=initializeOverflow();if(!sizedOverflow(popup,bounds))return false;}
    int w=bounds.right-bounds.left,h=bounds.bottom-bounds.top;
    MONITORINFO monitor={sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST),&monitor);
    int x=std::clamp(anchor.x-w,monitor.rcWork.left,std::max(monitor.rcWork.left,monitor.rcWork.right-w)),y=std::clamp(anchor.y,monitor.rcWork.top,std::max(monitor.rcWork.top,monitor.rcWork.bottom-h));
    overflowAnchor=anchor;overflowAnchored=true;
    monitorOverflowMouse(true);
    SetWindowPos(popup,HWND_TOPMOST,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
    SetWindowPos(popup,HWND_TOPMOST,x,y,0,0,SWP_NOSIZE|SWP_SHOWWINDOW|SWP_NOACTIVATE);
    SetForegroundWindow(popup);updateOverflow();maintain();return true;
}
bool launchCommand(HWND owner,const std::wstring& text,bool elevated){
    health::Hold protection;if(!protection)return false;
    auto begin=text.find_first_not_of(L" \t\r\n");if(begin==std::wstring::npos)return false;auto end=text.find_last_not_of(L" \t\r\n");std::wstring input=text.substr(begin,end-begin+1);
    wchar_t expanded[32768]={};if(!ExpandEnvironmentStringsW(input.c_str(),expanded,32768))return false;input=expanded;
    std::wstring file,args;
    if(GetFileAttributesW(input.c_str())!=INVALID_FILE_ATTRIBUTES)file=input;
    else if(!input.empty()&&input.front()==L'"'){auto quote=input.find(L'"',1);if(quote==std::wstring::npos)return false;file=input.substr(1,quote-1);args=input.substr(quote+1);}
    else {auto split=input.find_first_of(L" \t");file=input.substr(0,split);if(split!=std::wstring::npos)args=input.substr(split+1);}
    if(!elevated){DWORD attrs=GetFileAttributesW(file.c_str());if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY))return openFileBrowser(file);if(!_wcsicmp(std::filesystem::path(file).filename().c_str(),L"explorer.exe")){auto at=args.find_first_not_of(L" \t");auto folder=at==std::wstring::npos?L"":args.substr(at);if(folder.size()>1&&folder.front()==L'"'&&folder.back()==L'"')folder=folder.substr(1,folder.size()-2);if(GetFileAttributesW(folder.c_str())==INVALID_FILE_ATTRIBUTES)folder.clear();return openFileBrowser(folder);}}
    SHELLEXECUTEINFOW execute={sizeof(execute)};execute.hwnd=owner;execute.fMask=SEE_MASK_FLAG_NO_UI;execute.lpVerb=elevated?L"runas":L"open";execute.lpFile=file.c_str();execute.lpParameters=args.empty()?nullptr:args.c_str();execute.nShow=SW_SHOWNORMAL;
    return ShellExecuteExW(&execute)!=FALSE;
}
