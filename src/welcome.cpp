#include "welcome.h"
#include "welcome_visual.h"
#include "runtime.h"
#include "platform.h"
#include <wtsapi32.h>
#include <powrprof.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr wchar_t ClassName[]=L"OwnIsland.Welcome";
struct Visibility {bool screen=true,session=true,suspended=false,displayChanged=false;};
LRESULT CALLBACK welcomeProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_NCCREATE)SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto state=(Visibility*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(state){
        if(msg==WM_WTSSESSION_CHANGE){if(wp==WTS_SESSION_LOCK||wp==WTS_CONSOLE_DISCONNECT||wp==WTS_REMOTE_DISCONNECT)state->session=false;else if(wp==WTS_SESSION_UNLOCK||wp==WTS_CONSOLE_CONNECT||wp==WTS_REMOTE_CONNECT)state->session=true;return 0;}
        if(msg==WM_POWERBROADCAST){if(wp==PBT_APMSUSPEND)state->suspended=true;if(wp==PBT_APMRESUMEAUTOMATIC||wp==PBT_APMRESUMESUSPEND)state->suspended=false;if(wp==PBT_POWERSETTINGCHANGE){auto change=(POWERBROADCAST_SETTING*)lp;if(change&&IsEqualGUID(change->PowerSetting,GUID_CONSOLE_DISPLAY_STATE)&&change->DataLength==sizeof(DWORD)){DWORD value=0;memcpy(&value,change->Data,sizeof(value));state->screen=value!=0;}}return TRUE;}
        if(msg==WM_DISPLAYCHANGE||msg==WM_DPICHANGED){state->displayChanged=true;return 0;}
    }
    if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_ARROW));return TRUE;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_CLOSE)return 0;
    if(msg==WM_PAINT){PAINTSTRUCT ps;BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void trace(const wchar_t* stage){
    auto path=stateDirectory()+L"\\welcome-startup.log";
    WIN32_FILE_ATTRIBUTE_DATA attributes{};if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes)&&attributes.nFileSizeLow>32768)MoveFileExW(path.c_str(),(path+L".previous").c_str(),MOVEFILE_REPLACE_EXISTING);
    HANDLE file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return;
    FILETIME created{},a{},b{},c{},now{};GetProcessTimes(GetCurrentProcess(),&created,&a,&b,&c);GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER start{},current{};start.LowPart=created.dwLowDateTime;start.HighPart=created.dwHighDateTime;current.LowPart=now.dwLowDateTime;current.HighPart=now.dwHighDateTime;
    wchar_t wide[180];int length=swprintf(wide,180,L"pid=%lu tick=%llu process-ms=%llu %ls\r\n",GetCurrentProcessId(),GetTickCount64(),(current.QuadPart-start.QuadPart)/10000,stage);
    char text[360];int bytes=WideCharToMultiByte(CP_UTF8,0,wide,length,text,sizeof(text),nullptr,nullptr);DWORD written=0;if(bytes>0)WriteFile(file,text,bytes,&written,nullptr);CloseHandle(file);
}
struct Surface {
    HWND hwnd=nullptr;RECT bounds{};Graphics gfx;bool primary=false;
    ~Surface(){if(IsWindow(hwnd))DestroyWindow(hwnd);}
};
struct Scene {
    Visibility visibility;std::vector<std::unique_ptr<Surface>> surfaces;HPOWERNOTIFY power=nullptr;
    std::wstring logonKey;ULONGLONG previous=0,elapsed=0,sweep=0;bool revealing=false,shown=false;
    ~Scene(){if(power)UnregisterPowerSettingNotification(power);if(!surfaces.empty())WTSUnRegisterSessionNotification(surfaces.front()->hwnd);}
    bool eligible(bool preview){
        if(preview)return true;WTSINFOW* info=nullptr;DWORD size=0;bool duplicate=false;
        if(WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,WTS_CURRENT_SESSION,WTSSessionInfo,(LPWSTR*)&info,&size)&&size>=sizeof(WTSINFOW)&&info->LogonTime.QuadPart){
            logonKey=std::to_wstring(info->LogonTime.QuadPart);wchar_t last[100]={};GetPrivateProfileStringW(L"welcome",L"logon",L"",last,100,(stateDirectory()+L"\\login-visual.ini").c_str());duplicate=logonKey==last;
        }if(info)WTSFreeMemory(info);if(duplicate)trace(L"skip-logon-already-shown");else if(logonKey.empty())trace(L"logon-time-unavailable");return !duplicate;
    }
    static std::vector<RECT> monitors(){std::vector<RECT> result;EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT r,LPARAM p)->BOOL{((std::vector<RECT>*)p)->push_back(*r);return TRUE;},(LPARAM)&result);return result;}
    bool topology(){auto current=monitors();if(current.size()!=surfaces.size())return false;for(size_t i=0;i<current.size();i++)if(!EqualRect(&current[i],&surfaces[i]->bounds))return false;return true;}
    bool create(){
        WNDCLASSEXW cls{sizeof(cls)};cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=ClassName;cls.lpfnWndProc=welcomeProc;RegisterClassExW(&cls);
        for(auto r:monitors()){
            auto surface=std::make_unique<Surface>();surface->bounds=r;surface->primary=r.left<=0&&r.right>0&&r.top<=0&&r.bottom>0;
            surface->hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT,ClassName,L"",WS_POPUP,r.left,r.top,r.right-r.left,r.bottom-r.top,nullptr,nullptr,GetModuleHandleW(nullptr),&visibility);
            if(!surface->hwnd||!surface->gfx.init(surface->hwnd,r.right-r.left,r.bottom-r.top,1,false))return false;
            surfaces.push_back(std::move(surface));
        }
        if(surfaces.empty())return false;
        WTSRegisterSessionNotification(surfaces.front()->hwnd,NOTIFY_FOR_THIS_SESSION);power=RegisterPowerSettingNotification(surfaces.front()->hwnd,&GUID_CONSOLE_DISPLAY_STATE,DEVICE_NOTIFY_WINDOW_HANDLE);
        previous=GetTickCount64();return draw();
    }
    bool draw(){for(auto& surface:surfaces){auto& gfx=surface->gfx;gfx.begin();welcome_visual::draw(gfx,(float)(surface->bounds.right-surface->bounds.left),(float)(surface->bounds.bottom-surface->bounds.top),surface->primary?(float)elapsed:0,(float)sweep,revealing);if(!gfx.end())return false;}return true;}
    bool tick(bool ready,bool awake){
        auto now=GetTickCount64();auto delta=now-previous;previous=now;
        if(visibility.displayChanged){visibility.displayChanged=false;if(!topology()){trace(L"display-topology-changed");return false;}}
        if(!awake||!visibility.screen||!visibility.session||visibility.suspended||!runtimeDesktopVisible()){
            if(shown)for(auto& surface:surfaces)ShowWindow(surface->hwnd,SW_HIDE);shown=false;return true;
        }
        for(auto& surface:surfaces)if(!IsWindow(surface->hwnd))return false;
        if(!shown){delta=0;for(auto& surface:surfaces)if(!SetWindowPos(surface->hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW)||!IsWindowVisible(surface->hwnd)){trace(L"show-failed");return false;}shown=true;trace(L"visible");if(!logonKey.empty()){WritePrivateProfileStringW(L"welcome",L"logon",logonKey.c_str(),(stateDirectory()+L"\\login-visual.ini").c_str());logonKey.clear();}}
        elapsed+=delta;
        if(!revealing&&elapsed>=(ULONGLONG)welcome_visual::GreetingMilliseconds&&ready){revealing=true;trace(L"sweep");}
        if(revealing)sweep+=delta;
        if(elapsed>=12000){trace(L"ready-timeout");return false;}if(sweep>=(ULONGLONG)welcome_visual::SweepMilliseconds){trace(L"completed");return false;}
        return draw();
    }
};
}
struct WelcomeAnimation::Impl {
    std::atomic<bool> quit=false,ready=false,awake=true,alive=true,displayDirty=false;
    std::mutex properties;HWND owner=nullptr;std::thread worker;HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);bool preview=false;
    ~Impl(){quit=true;if(wake)SetEvent(wake);if(worker.joinable())worker.join();publish(0);if(wake)CloseHandle(wake);}
    void attach(HWND value){std::lock_guard lock(properties);if(value)owner=value;if(owner&&alive){SetPropW(owner,L"OwnIsland.WelcomeActive",(HANDLE)1);SetPropW(owner,L"OwnIsland.WelcomePreview",(HANDLE)(UINT_PTR)preview);}}
    void publish(int phase){std::lock_guard lock(properties);if(!owner)return;if(phase){SetPropW(owner,L"OwnIsland.WelcomePhase",(HANDLE)(UINT_PTR)phase);}else{RemovePropW(owner,L"OwnIsland.WelcomeActive");RemovePropW(owner,L"OwnIsland.WelcomePhase");RemovePropW(owner,L"OwnIsland.WelcomePreview");}}
    void run(){
        CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        try{
            Scene scene;
            if(scene.eligible(preview)){
                trace(L"begin");
                if(scene.create())while(!quit){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}if(displayDirty.exchange(false))scene.visibility.displayChanged=true;if(!scene.tick(ready,awake))break;if(scene.shown)publish(scene.revealing?2:1);else publishHidden();MsgWaitForMultipleObjects(wake?1:0,wake?&wake:nullptr,FALSE,16,QS_ALLINPUT);}else trace(L"create-failed");
                trace(L"closed");
            }
        }catch(...){trace(L"graphics-error");}
        alive=false;publish(0);CoUninitialize();
    }
    void publishHidden(){std::lock_guard lock(properties);if(owner)RemovePropW(owner,L"OwnIsland.WelcomePhase");}
};
WelcomeAnimation::WelcomeAnimation()=default;
WelcomeAnimation::~WelcomeAnimation()=default;
void WelcomeAnimation::start(HWND owner,bool preview){
    if(impl){if(impl->alive||!preview){impl->attach(owner);return;}stop();}
    auto next=std::make_unique<Impl>();next->preview=preview;next->attach(owner);auto target=next.get();impl=std::move(next);
    try{target->worker=std::thread([target]{target->run();});}catch(...){impl.reset();}
}
void WelcomeAnimation::update(bool ready,bool awake){if(!impl)return;impl->ready=ready;impl->awake=awake;if(impl->wake)SetEvent(impl->wake);}
void WelcomeAnimation::displayChanged(){if(!impl)return;impl->displayDirty=true;if(impl->wake)SetEvent(impl->wake);}
void WelcomeAnimation::stop(){impl.reset();}
bool WelcomeAnimation::active()const{return impl&&impl->alive;}
