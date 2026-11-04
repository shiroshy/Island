// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "runtime.h"
#include "file_browser.h"
#include "tasks.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <chrono>
#include <cwctype>

namespace {
std::wstring executable(DWORD pid){
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)return {};
    std::wstring path(32768,L'\0');DWORD size=(DWORD)path.size();
    bool ok=QueryFullProcessImageNameW(process,0,path.data(),&size);CloseHandle(process);
    if(!ok)return {};path.resize(size);return path;
}
std::wstring basename(const std::wstring& path){auto p=path.find_last_of(L"\\/");auto s=path.substr(p==std::wstring::npos?0:p+1);p=s.find_last_of(L'.');if(p!=std::wstring::npos)s.resize(p);return s;}
std::wstring lower(std::wstring s){std::transform(s.begin(),s.end(),s.begin(),towlower);return s;}
std::wstring applicationId(HWND hwnd){
    IPropertyStore* properties=nullptr;std::wstring id;
    if(SUCCEEDED(SHGetPropertyStoreForWindow(hwnd,IID_PPV_ARGS(&properties)))){
        PROPVARIANT value;PropVariantInit(&value);
        if(SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID,&value))&&value.vt==VT_LPWSTR&&value.pwszVal)id=value.pwszVal;
        PropVariantClear(&value);properties->Release();
    }
    return id;
}
std::shared_ptr<const PixelImage> iconPixels(HICON icon,uint64_t id){
    if(!icon)return {};
    int size=32;ICONINFO native{};
    if(GetIconInfo(icon,&native)){BITMAP bitmap{};if(native.hbmColor&&GetObjectW(native.hbmColor,sizeof(bitmap),&bitmap))size=std::clamp((int)bitmap.bmWidth,16,256);if(native.hbmColor)DeleteObject(native.hbmColor);if(native.hbmMask)DeleteObject(native.hbmMask);}
    BITMAPINFO info={};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=size;info.bmiHeader.biHeight=-size;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(nullptr);void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dc||!bitmap){if(dc)DeleteDC(dc);if(bitmap)DeleteObject(bitmap);return {};}
    auto old=SelectObject(dc,bitmap);ZeroMemory(bits,size*size*4);DrawIconEx(dc,0,0,icon,size,size,0,nullptr,DI_NORMAL);GdiFlush();
    auto result=std::make_shared<PixelImage>();result->width=size;result->height=size;result->id=id;
    result->bgra.assign((unsigned char*)bits,(unsigned char*)bits+size*size*4);
    bool alpha=false;for(size_t i=3;i<result->bgra.size();i+=4)if(result->bgra[i]){alpha=true;break;}
    if(!alpha){
        // Legacy masked icons have no alpha channel. Recover coverage using two backgrounds.
        memset(bits,255,size*size*4);DrawIconEx(dc,0,0,icon,size,size,0,nullptr,DI_NORMAL);GdiFlush();
        auto white=(unsigned char*)bits;
        for(size_t i=0;i<result->bgra.size();i+=4){int delta=std::max({(int)white[i]-result->bgra[i],(int)white[i+1]-result->bgra[i+1],(int)white[i+2]-result->bgra[i+2]});result->bgra[i+3]=(unsigned char)std::clamp(255-delta,0,255);}
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return result;
}
std::shared_ptr<const PixelImage> shellPixels(const std::wstring& path,uint64_t id){
    if(path.empty())return {};
    IShellItemImageFactory* factory=nullptr;
    if(FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&factory))))return {};
    HBITMAP bitmap=nullptr;HRESULT hr=factory->GetImage({128,128},(SIIGBF)(SIIGBF_ICONONLY|SIIGBF_BIGGERSIZEOK),&bitmap);factory->Release();
    if(FAILED(hr)||!bitmap)return {};
    BITMAP source{};GetObjectW(bitmap,sizeof(source),&source);
    int width=source.bmWidth,height=std::abs(source.bmHeight);
    if(width<1||height<1||width>1024||height>1024){DeleteObject(bitmap);return {};}
    auto result=std::make_shared<PixelImage>();result->id=id;result->width=width;result->height=height;result->bgra.resize((size_t)width*height*4);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(nullptr);int rows=GetDIBits(dc,bitmap,0,height,result->bgra.data(),&info,DIB_RGB_COLORS);DeleteDC(dc);DeleteObject(bitmap);
    if(rows!=height)return {};
    bool alpha=false,straight=false;
    for(size_t i=0;i<result->bgra.size();i+=4){unsigned a=result->bgra[i+3];alpha|=a!=0;if(a&&a<255&&(result->bgra[i]>a+2||result->bgra[i+1]>a+2||result->bgra[i+2]>a+2))straight=true;}
    if(!alpha)return {};
    for(size_t i=0;i<result->bgra.size();i+=4){unsigned a=result->bgra[i+3];for(int c=0;c<3;c++){if(!a)result->bgra[i+c]=0;else if(straight)result->bgra[i+c]=(unsigned char)((result->bgra[i+c]*a+127)/255);}}
    return result;
}
std::shared_ptr<const PixelImage> readIcon(const std::wstring& path,HWND hwnd,uint64_t id){
    if(auto image=shellPixels(path,id))return image;
    HICON owned=nullptr;
    if(hwnd){DWORD_PTR value=0;SendMessageTimeoutW(hwnd,WM_GETICON,ICON_BIG,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,35,&value);HICON icon=(HICON)value;if(!icon)icon=(HICON)GetClassLongPtrW(hwnd,GCLP_HICON);if(icon)owned=CopyIcon(icon);}
    if(!owned){if(auto image=shellPixels(path,id))return image;}
    if(!owned&&!path.empty()){SHFILEINFOW info={};if(SHGetFileInfoW(path.c_str(),0,&info,sizeof(info),SHGFI_ICON|SHGFI_LARGEICON))owned=info.hIcon;}
    if(!owned)owned=CopyIcon(LoadIconW(nullptr,IDI_APPLICATION));
    auto result=iconPixels(owned,id);if(owned)DestroyIcon(owned);return result;
}
struct RawWindow {TaskWindow window;std::wstring path,id,cls;};
BOOL CALLBACK collect(HWND hwnd,LPARAM param){
    auto& rows=*(std::vector<RawWindow>*)param;
    if(!IsWindowVisible(hwnd)||hwnd==GetShellWindow())return TRUE;
    DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);if(!pid)return TRUE;if(pid==GetCurrentProcessId()){wchar_t cls[100]={};GetClassNameW(hwnd,cls,100);if(wcscmp(cls,L"OwnIsland.Files"))return TRUE;}
    LONG_PTR ex=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    if(!(ex&WS_EX_APPWINDOW)&&((ex&WS_EX_TOOLWINDOW)||GetWindow(hwnd,GW_OWNER)))return TRUE;
    DWORD cloaked=0;if(SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return TRUE;
    wchar_t cls[256]={},title[1024]={};GetClassNameW(hwnd,cls,256);GetWindowTextW(hwnd,title,1024);
    if(!*title||!wcscmp(cls,L"Progman")||!wcscmp(cls,L"WorkerW")||!wcscmp(cls,L"Shell_TrayWnd")||!wcscmp(cls,L"Shell_SecondaryTrayWnd"))return TRUE;
    DWORD appPid=pid;
    if(!wcscmp(cls,L"ApplicationFrameWindow"))EnumChildWindows(hwnd,[](HWND child,LPARAM data)->BOOL{wchar_t c[128]={};GetClassNameW(child,c,128);if(!wcscmp(c,L"Windows.UI.Core.CoreWindow"))GetWindowThreadProcessId(child,(DWORD*)data);return TRUE;},(LPARAM)&appPid);
    rows.push_back({{hwnd,pid,title},executable(appPid),applicationId(hwnd),cls});return TRUE;
}
bool same(const TaskSnapshot& a,const TaskSnapshot& b){
    if(a.groups.size()!=b.groups.size()||a.shortcuts!=b.shortcuts)return false;
    for(size_t i=0;i<a.groups.size();i++){auto& x=a.groups[i];auto& y=b.groups[i];if(x.key!=y.key||x.active!=y.active||x.windows.size()!=y.windows.size())return false;for(size_t j=0;j<x.windows.size();j++)if(x.windows[j].hwnd!=y.windows[j].hwnd||x.windows[j].pid!=y.windows[j].pid||x.windows[j].title!=y.windows[j].title)return false;}
    return true;
}
}
std::shared_ptr<const PixelImage> loadShellIcon(const std::wstring& path,uint64_t id){return readIcon(path,nullptr,id);}
std::shared_ptr<const PixelImage> copyIconPixels(HICON icon,uint64_t id){return iconPixels(icon,id);}
void TaskTracker::start(HWND window){target=window;stopping=false;worker=std::thread([this]{run();});}
void TaskTracker::stop(){{std::lock_guard lock(mutex);stopping=true;dirty=true;}wake.notify_one();if(worker.joinable())worker.join();}
void TaskTracker::refresh(){{std::lock_guard lock(mutex);dirty=true;}wake.notify_one();}
void TaskTracker::setPinned(const std::vector<std::wstring>& paths){{std::lock_guard lock(mutex);pinned=paths;dirty=true;}wake.notify_one();}
std::shared_ptr<const TaskSnapshot> TaskTracker::snapshot(){std::lock_guard lock(mutex);return current;}
void TaskTracker::run(){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    std::map<std::wstring,std::shared_ptr<const PixelImage>> icons;uint64_t iconId=1;
    std::vector<std::wstring> order;HWND lastForeground=nullptr;
    while(true){
        std::vector<std::wstring> pins;
        {std::unique_lock lock(mutex);if(stopping)break;pins=pinned;dirty=false;}
        std::vector<RawWindow> rows;EnumWindows(collect,(LPARAM)&rows);
        HWND fg=GetForegroundWindow();DWORD fgPid=0;if(fg)GetWindowThreadProcessId(fg,&fgPid);
        if(fg&&fgPid!=GetCurrentProcessId())lastForeground=fg;
        auto next=std::make_shared<TaskSnapshot>();
        for(auto& row:rows){
            auto key=!row.id.empty()?L"id:"+row.id:!row.path.empty()?L"exe:"+lower(row.path):L"pid:"+std::to_wstring(row.window.pid)+row.cls;
            auto group=std::find_if(next->groups.begin(),next->groups.end(),[&](auto& g){return g.key==key;});
            if(group==next->groups.end()){
                auto cache=L"task:"+key;auto& icon=icons[cache];if(!icon)icon=readIcon(row.path,row.window.hwnd,iconId++);
                auto name=basename(row.path);if(name.empty()||lower(name)==L"applicationframehost")name=row.window.title;
                next->groups.push_back({key,name,row.path,icon,{},false});group=std::prev(next->groups.end());
            }
            group->windows.push_back(row.window);group->active|=row.window.hwnd==lastForeground&&!IsIconic(row.window.hwnd);
        }
        order.erase(std::remove_if(order.begin(),order.end(),[&](auto& key){return std::none_of(next->groups.begin(),next->groups.end(),[&](auto& g){return g.key==key;});}),order.end());
        for(auto& g:next->groups){if(std::find(order.begin(),order.end(),g.key)==order.end())order.push_back(g.key);std::sort(g.windows.begin(),g.windows.end(),[](auto& a,auto& b){return (uintptr_t)a.hwnd<(uintptr_t)b.hwnd;});}
        std::sort(next->groups.begin(),next->groups.end(),[&](auto& a,auto& b){return std::find(order.begin(),order.end(),a.key)<std::find(order.begin(),order.end(),b.key);});
        for(auto& path:pins){auto& icon=icons[L"pin:"+path];if(!icon)icon=readIcon(path,nullptr,iconId++);next->shortcuts[path]=icon;}
        // Keep only icons used by live groups and pinned entries.
        for(auto it=icons.begin();it!=icons.end();){bool keep=std::any_of(next->groups.begin(),next->groups.end(),[&](auto& g){return it->first==L"task:"+g.key;})||std::any_of(pins.begin(),pins.end(),[&](auto& p){return it->first==L"pin:"+p;});if(!keep)it=icons.erase(it);else ++it;}
        bool changed=false;{std::lock_guard lock(mutex);changed=!same(*current,*next);if(changed)current=next;}
        if(changed)PostMessageW(target,WM_APP+4,0,0);
        std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(750),[this]{return stopping||dirty;});if(stopping)break;
    }
    CoUninitialize();
}
bool validTaskWindow(const TaskWindow& window){DWORD pid=0;return IsWindow(window.hwnd)&&GetWindowThreadProcessId(window.hwnd,&pid)&&pid==window.pid;}
bool isTaskWindow(HWND hwnd){
    if(!hwnd||!IsWindowVisible(hwnd)||hwnd==GetShellWindow())return false;
    DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);if(!pid)return false;if(pid==GetCurrentProcessId()){wchar_t cls[100]={};GetClassNameW(hwnd,cls,100);if(wcscmp(cls,L"OwnIsland.Files"))return false;}
    LONG_PTR ex=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);if(!(ex&WS_EX_APPWINDOW)&&((ex&WS_EX_TOOLWINDOW)||GetWindow(hwnd,GW_OWNER)))return false;
    DWORD cloaked=0;if(SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return false;
    wchar_t cls[128]={},title[2]={};GetClassNameW(hwnd,cls,128);GetWindowTextW(hwnd,title,2);
    return *title&&wcscmp(cls,L"Progman")&&wcscmp(cls,L"WorkerW")&&wcscmp(cls,L"Shell_TrayWnd")&&wcscmp(cls,L"Shell_SecondaryTrayWnd");
}
std::vector<TaskWindow> taskWindows(){std::vector<TaskWindow> result;EnumWindows([](HWND hwnd,LPARAM data)->BOOL{if(isTaskWindow(hwnd)){DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);wchar_t title[1024]={};GetWindowTextW(hwnd,title,1024);((std::vector<TaskWindow>*)data)->push_back({hwnd,pid,title});}return TRUE;},(LPARAM)&result);return result;}
bool activateTaskWindow(const TaskWindow& window,bool toggle){
    if(!validTaskWindow(window))return false;
    // Tool windows can temporarily clear the foreground during a mouse click.
    // The caller supplies the last active group, as shown in the task strip.
    if(toggle&&!IsIconic(window.hwnd)){ShowWindowAsync(window.hwnd,SW_MINIMIZE);return true;}
    if(IsIconic(window.hwnd))ShowWindowAsync(window.hwnd,SW_RESTORE);
    return SetForegroundWindow(window.hwnd)!=FALSE;
}

void openPinnedTarget(const std::wstring& path,const std::shared_ptr<const TaskSnapshot>& snapshot){
    std::wstring target=path,id,arguments;IShellLinkW* link=nullptr;
    if(path.size()>4&&!_wcsicmp(path.substr(path.size()-4).c_str(),L".lnk")&&SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))){
        IPersistFile* file=nullptr;if(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))){if(SUCCEEDED(file->Load(path.c_str(),STGM_READ))){wchar_t resolved[32768]={},args[32768]={};link->GetArguments(args,32768);arguments=args;if(SUCCEEDED(link->GetPath(resolved,32768,nullptr,SLGP_RAWPATH))&&*resolved){wchar_t expanded[32768]={};if(ExpandEnvironmentStringsW(resolved,expanded,32768)&&*expanded)target=expanded;else target=resolved;}IPropertyStore* properties=nullptr;if(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&properties)))){PROPVARIANT value;PropVariantInit(&value);if(SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID,&value))&&value.vt==VT_LPWSTR&&value.pwszVal)id=value.pwszVal;PropVariantClear(&value);properties->Release();}}file->Release();}link->Release();
    }
    {DWORD attrs=GetFileAttributesW(target.c_str());auto slash=target.find_last_of(L"\\/");auto name=target.substr(slash==std::wstring::npos?0:slash+1);if(!_wcsicmp(name.c_str(),L"explorer.exe")){std::wstring folder=arguments;auto quote=folder.find(L'"');if(quote!=std::wstring::npos){auto last=folder.find(L'"',quote+1);folder=folder.substr(quote+1,last==std::wstring::npos?last:last-quote-1);}if(GetFileAttributesW(folder.c_str())==INVALID_FILE_ATTRIBUTES)folder.clear();openFileBrowser(folder);return;}if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY)){openFileBrowser(target);return;}}
    const TaskWindow* chosen=nullptr;
    if(snapshot)for(auto& group:snapshot->groups)if((!id.empty()&&group.key==L"id:"+id)||(!target.empty()&&!_wcsicmp(group.path.c_str(),target.c_str()))){for(auto& window:group.windows)if(validTaskWindow(window)&&(!chosen||(!IsIconic(window.hwnd)&&IsIconic(chosen->hwnd))))chosen=&window;}
    if(chosen){activateTaskWindow(*chosen);return;}
    // Refresh live windows when the tracker has not observed a newly launched app yet.
    for(auto& window:taskWindows())if(!_wcsicmp(executable(window.pid).c_str(),target.c_str())||(!id.empty()&&applicationId(window.hwnd)==id)){activateTaskWindow(window);return;}
    ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}
