// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "health.h"
#include "file_browser.h"
#include "file_ops.h"
#include "graphics.h"
#include "runtime.h"
#include "platform.h"
#include "frames.h"
#include "audio.h"
#include "menu.h"
#include "shell.h"
#include "overlay.h"
#include "search.h"
#include "desktop.h"
#include "components.h"
#include "native_desktop.h"
#include "wallpaper.h"
#include "dock.h"
#include <wtsapi32.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <powrprof.h>
#include <dwmapi.h>
#include <shcore.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cwctype>
#include <psapi.h>

std::wstring pickScreenColor();

namespace {
constexpr wchar_t WindowClass[] = L"OwnIsland.Surface";
constexpr float SurfaceW=736,SurfaceH=476,PanelW=696,PanelH=342;
constexpr UINT TrayMessage=WM_APP+10, ExternalCommand=WM_APP+11;
unsigned Outline=0x77717f;float BorderWidth=0;bool BorderSmooth=true;
unsigned Ink=0xeae9ed,Muted=0x8a8790,Card=0x252328,Hover=0x322f37,Accent=0xb8d8c9,Background=0x09090b;
unsigned contrast(unsigned c){return (((c>>16)&255)*.2126f+((c>>8)&255)*.7152f+(c&255)*.0722f)>145?0x202126:0xf5f4f7;}
unsigned mix(unsigned a,unsigned b,float t){unsigned out=0;for(int shift:{0,8,16})out|=(unsigned)std::lround(((a>>shift)&255)*(1-t)+((b>>shift)&255)*t)<<shift;return out;}
double moment(){static LARGE_INTEGER frequency=[](){LARGE_INTEGER f;QueryPerformanceFrequency(&f);return f;}();LARGE_INTEGER n;QueryPerformanceCounter(&n);return (double)n.QuadPart/frequency.QuadPart;}
std::wstring hexColor(unsigned color){wchar_t s[8];swprintf(s,8,L"#%06X",color&0xffffff);return s;}
bool parseColor(std::wstring text,unsigned& value){if(!text.empty()&&text[0]==L'#')text.erase(0,1);if(text.size()!=6||text.find_first_not_of(L"0123456789abcdefABCDEF")!=std::wstring::npos)return false;value=(unsigned)wcstoul(text.c_str(),nullptr,16);return true;}
bool parseTimer(std::wstring text,int& seconds){
    auto begin=text.find_first_not_of(L" \t\r\n"),end=text.find_last_not_of(L" \t\r\n");if(begin==std::wstring::npos)return false;text=text.substr(begin,end-begin+1);
    if(text.size()>12||text.find_first_not_of(L"0123456789:")!=std::wstring::npos)return false;
    std::vector<int> parts;size_t pos=0;
    while(true){auto p=text.find(L':',pos);auto part=text.substr(pos,p==std::wstring::npos?p:p-pos);if(part.empty()||part.size()>5)return false;parts.push_back(_wtoi(part.c_str()));if(p==std::wstring::npos)break;pos=p+1;}
    if(parts.size()==1)seconds=parts[0]*60;
    else if(parts.size()==2&&parts[1]<60)seconds=parts[0]*60+parts[1];
    else if(parts.size()==3&&parts[1]<60&&parts[2]<60)seconds=parts[0]*3600+parts[1]*60+parts[2];else return false;
    return seconds>0&&seconds<=86400;
}
struct Hit { Box box; std::function<void(float)> action; bool header=false; };
constexpr int TabOrder[]={0,11,1,2,3,4,5,6,7};
constexpr const wchar_t* TabNames[]={L"Главная",L"Приложения",L"Буфер",L"Файлы",L"Текст",L"Чат",L"Заметки",L"Погода",L"Управление"};
int tabRank(int tab){for(int i=0;i<9;i++)if(TabOrder[i]==tab)return i;return tab==12?9:tab==9?10:tab==8?11:12;}
float tabWidth(int i,int selected){return TabOrder[i]==selected?std::max(82.f,(float)wcslen(TabNames[i])*6.5f+43):35.f;}
HWND taskHookTarget=nullptr;
void CALLBACK foregroundEvent(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD){if(taskHookTarget)PostMessageW(taskHookTarget,WM_APP+5,0,0);}
struct Appearance {std::array<unsigned,7> colors{};float width=0;bool smooth=true;};
std::wstring decimal(float value){wchar_t b[24];swprintf(b,24,L"%.1f",value);return b;}
struct Settings { bool hover=true, seconds=false; int size=100; };
std::wstring timeString(bool seconds=false) {
    SYSTEMTIME now; GetLocalTime(&now);
    wchar_t s[32]; swprintf(s,32,seconds?L"%02d:%02d:%02d":L"%02d:%02d",now.wHour,now.wMinute,now.wSecond); return s;
}
std::wstring duration(double sec) {
    int n=std::max(0,(int)sec); wchar_t s[32];
    if(n>=3600)swprintf(s,32,L"%d:%02d:%02d",n/3600,n/60%60,n%60);else swprintf(s,32,L"%d:%02d",n/60,n%60);
    return s;
}
std::wstring roundNumber(double n) { return std::to_wstring((int)std::round(n)); }
std::wstring weatherLabel(int code) {
    if(code==0)return L"Ясно";if(code<=3)return L"Облачно";if(code<=48)return L"Туман";
    if(code<=67)return L"Дождь";if(code<=77)return L"Снег";if(code<=82)return L"Ливень";if(code<=86)return L"Снегопад";return L"Гроза";
}
std::wstring editText(HWND edit) {
    int n=GetWindowTextLengthW(edit);std::wstring s(n+1,L'\0');GetWindowTextW(edit,s.data(),n+1);s.resize(n);return s;
}
class Island {
public:
    HWND hwnd=nullptr,edit=nullptr,timerEdit=nullptr;
    HFONT editFont=nullptr,timerFont=nullptr;
    HBRUSH editBrush=CreateSolidBrush(RGB(37,35,40));
    WNDPROC oldEditProc=nullptr;
    WNDPROC oldTimerProc=nullptr;
    std::unique_ptr<Graphics> gfx;
    std::vector<Hit> hits;
    MediaBridge media;
    AudioMeter audio;
    Frames frames;
    TaskTracker tasks;
    ShellPanel shell;
    Overlay overlay;
    WindowsSearch search;
    DesktopProcess desktop;
    WallpaperMedia panelWallpaper;
    int wallOpacity=65,wallShade=35,wallBlur=8,wallDrag=-1;
    bool desktopEnabled=true;
    std::shared_ptr<const SearchState> searchState=std::make_shared<SearchState>();
    std::wstring searchValue;
    int searchSelected=0,searchOffset=0;
    bool searchFocus=false;
    struct SearchSlot {Box box;size_t index;};
    std::vector<SearchSlot> searchSlots;
    unsigned long long searchClickAt=0;
    size_t searchClickIndex=~size_t(0);
    HPOWERNOTIFY displayNotification=nullptr;
    bool displayActive=true,sessionActive=true,systemSuspended=false,renderPaused=false;
    unsigned long long renderedFrames=0;
    std::shared_ptr<const TaskSnapshot> taskState=std::make_shared<TaskSnapshot>();
    HWINEVENTHOOK foregroundHook=nullptr;
    MediaState now;
    Settings settings;
    Appearance desktopLook;bool appearanceDesktop=false;int styleDrag=0;float pickerHue=0,pickerSat=0,pickerValue=0;
    std::wstring ini=stateDirectory()+L"\\settings.ini";
    std::wstring textValue,noteValue,cityValue,commandValue;
    std::vector<std::wstring> clips,files,shortcuts;
    int tab=0,editOwner=-1,hoverIndex=-1,selectedClip=-1;
    int clipOffset=0,fileOffset=0,appOffset=0,taskOffset=0;
    float scale=1,currentW=240,currentH=36,fromW=240,fromH=36,targetW=240,targetH=36;
    bool expanded=false,pinned=false,tracking=false,clipboardOn=false,manualHidden=false,dialog=false,closing=false,weatherEditing=false;
    bool hoverBlocked=false;
    bool morphing=false;
    double morphStart=0,tabStart=0,lastFrame=0;
    int tabDirection=1;
    std::array<float,9> headerFrom{};
    double headerStart=0;
    int refreshRate=60,customTimerSeconds=300,selectedColor=0;
    std::wstring colorValue;
    std::array<float,3> meter{};
    struct ShortcutSlot {Box box;size_t index;};
    std::vector<ShortcutSlot> shortcutSlots;
    struct TaskSlot {Box box;TaskGroup group;};
    std::vector<TaskSlot> taskSlots;
    struct DesktopWindow{TaskWindow window;bool maximized=false;};
    std::vector<DesktopWindow> desktopWindows;
    TaskWindow desktopFocus;
    bool desktopShown=false;
    unsigned long long desktopSettleUntil=0;
    unsigned long long animationStart=0,outsideSince=0,toastUntil=0,confirmUntil=0,timerEnd=0,timerPaused=0;
    std::wstring toastTitle,toastDetail,confirmAction;
    int previousCaps=0,previousLanguage=0;
    bool draggingVolume=false,draggingSeek=false,musicOver=false;float seekRatio=0;
    NetworkMonitor network;PerformanceSettings performance;bool dockLocked=false;HWND languageRoot=nullptr;HKL languageBefore=nullptr;
    int dockEdge=0,dockDragEdge=0;float dockRatio=.5f,dockGrabOffset=0;POINT dockMonitor{0,0},dockDown{};bool dockPending=false,dockMoving=false;
    float volume=0;
    bool muted=false;
    int micState=-1;
    HWND priorForeground=nullptr;
    WeatherState weather;
    std::wstring weatherRequestedCity;
    std::thread weatherWorker;
    std::atomic<bool> weatherBusy=false;
    NOTIFYICONDATAW tray={};
    UINT taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    ~Island() {
        closing=true;
        desktop.stop();
        panelWallpaper.stop();
        shell.stop();
        search.stop();
        tasks.stop();
        if(foregroundHook)UnhookWinEvent(foregroundHook);
        taskHookTarget=nullptr;
        if(weatherWorker.joinable())weatherWorker.join();
        media.stop();
        if(editFont)DeleteObject(editFont);
        if(timerFont)DeleteObject(timerFont);
        DeleteObject(editBrush);
    }
    void load() {
        performance.load(ini);
        dockLocked=GetPrivateProfileIntW(L"dock",L"locked",0,ini.c_str())!=0;dockEdge=std::clamp((int)GetPrivateProfileIntW(L"dock",L"edge",0,ini.c_str()),0,3);wchar_t dockValue[32]={};GetPrivateProfileStringW(L"dock",L"ratio",L"0.5",dockValue,32,ini.c_str());dockRatio=std::clamp(wcstof(dockValue,nullptr),0.f,1.f);dockMonitor.x=(LONG)(int)GetPrivateProfileIntW(L"dock",L"monitor-x",0,ini.c_str());dockMonitor.y=(LONG)(int)GetPrivateProfileIntW(L"dock",L"monitor-y",0,ini.c_str());
        settings.hover=GetPrivateProfileIntW(L"island",L"hover",1,ini.c_str())!=0;
        settings.seconds=GetPrivateProfileIntW(L"island",L"seconds",0,ini.c_str())!=0;
        settings.size=std::clamp((int)GetPrivateProfileIntW(L"island",L"size",100,ini.c_str()),80,150);
        desktopEnabled=GetPrivateProfileIntW(L"desktop",L"enabled",1,ini.c_str())!=0;
        customTimerSeconds=std::clamp((int)GetPrivateProfileIntW(L"timer",L"seconds",300,ini.c_str()),1,86400);
        const wchar_t* keys[]={L"background",L"card",L"accent",L"text",L"secondary",L"hover",L"outline"};
        unsigned* colors[]={&Background,&Card,&Accent,&Ink,&Muted,&Hover,&Outline};
        for(int i=0;i<7;i++){wchar_t value[16]={};GetPrivateProfileStringW(L"colors",keys[i],L"",value,16,ini.c_str());unsigned v;if(parseColor(value,v))*colors[i]=v;}
        wchar_t width[24]={};GetPrivateProfileStringW(L"colors",L"outline-width",L"0",width,24,ini.c_str());BorderWidth=std::clamp(std::round(wcstof(width,nullptr)*2)/2,0.f,6.f);BorderSmooth=GetPrivateProfileIntW(L"colors",L"outline-smooth",1,ini.c_str())!=0;
        for(int i=0;i<7;i++){desktopLook.colors[i]=*colors[i];wchar_t value[16]={};GetPrivateProfileStringW(L"desktop-colors",keys[i],L"",value,16,ini.c_str());unsigned v;if(parseColor(value,v))desktopLook.colors[i]=v;}GetPrivateProfileStringW(L"desktop-colors",L"outline-width",decimal(BorderWidth).c_str(),width,24,ini.c_str());desktopLook.width=std::clamp(std::round(wcstof(width,nullptr)*2)/2,0.f,6.f);desktopLook.smooth=GetPrivateProfileIntW(L"desktop-colors",L"outline-smooth",BorderSmooth,ini.c_str())!=0;
        wallOpacity=std::clamp((int)GetPrivateProfileIntW(L"panel",L"wallpaper-opacity",65,ini.c_str()),0,100);wallShade=std::clamp((int)GetPrivateProfileIntW(L"panel",L"wallpaper-shade",35,ini.c_str()),0,100);wallBlur=std::clamp((int)GetPrivateProfileIntW(L"panel",L"wallpaper-blur",8,ini.c_str()),0,30);
        panelWallpaper.start(L"panel",GetPrivateProfileIntW(L"panel",L"wallpaper-photo",0,ini.c_str())!=0);
        applyTheme();
        noteValue=readNote();
        for(int i=0;i<64;i++) {wchar_t p[32768]={};auto key=std::to_wstring(i);GetPrivateProfileStringW(L"launch",key.c_str(),L"",p,32768,ini.c_str());if(*p)shortcuts.push_back(p);}
        wchar_t city[256]={};GetPrivateProfileStringW(L"weather",L"city",L"",city,256,ini.c_str());cityValue=city;
        loadForecast();
    }
    void saveDock(){WritePrivateProfileStringW(L"dock",L"locked",dockLocked?L"1":L"0",ini.c_str());wchar_t ratio[32];swprintf(ratio,32,L"%.6f",dockRatio);WritePrivateProfileStringW(L"dock",L"edge",std::to_wstring(dockEdge).c_str(),ini.c_str());WritePrivateProfileStringW(L"dock",L"ratio",ratio,ini.c_str());WritePrivateProfileStringW(L"dock",L"monitor-x",std::to_wstring(dockMonitor.x).c_str(),ini.c_str());WritePrivateProfileStringW(L"dock",L"monitor-y",std::to_wstring(dockMonitor.y).c_str(),ini.c_str());}
    void save() {
        saveDock();
        WritePrivateProfileStringW(L"island",L"hover",settings.hover?L"1":L"0",ini.c_str());
        WritePrivateProfileStringW(L"island",L"seconds",settings.seconds?L"1":L"0",ini.c_str());
        WritePrivateProfileStringW(L"island",L"size",std::to_wstring(settings.size).c_str(),ini.c_str());
        WritePrivateProfileStringW(L"desktop",L"enabled",desktopEnabled?L"1":L"0",ini.c_str());
        WritePrivateProfileStringW(L"timer",L"seconds",std::to_wstring(customTimerSeconds).c_str(),ini.c_str());
        const wchar_t* keys[]={L"background",L"card",L"accent",L"text",L"secondary",L"hover",L"outline"};
        unsigned colors[]={Background,Card,Accent,Ink,Muted,Hover,Outline};
        for(int i=0;i<7;i++)WritePrivateProfileStringW(L"colors",keys[i],hexColor(colors[i]).c_str(),ini.c_str());
        WritePrivateProfileStringW(L"colors",L"outline-width",decimal(BorderWidth).c_str(),ini.c_str());
        WritePrivateProfileStringW(L"colors",L"outline-smooth",BorderSmooth?L"1":L"0",ini.c_str());
        for(int i=0;i<7;i++)WritePrivateProfileStringW(L"desktop-colors",keys[i],hexColor(desktopLook.colors[i]).c_str(),ini.c_str());WritePrivateProfileStringW(L"desktop-colors",L"outline-width",decimal(desktopLook.width).c_str(),ini.c_str());WritePrivateProfileStringW(L"desktop-colors",L"outline-smooth",desktopLook.smooth?L"1":L"0",ini.c_str());
        WritePrivateProfileStringW(L"launch",nullptr,nullptr,ini.c_str());
        for(size_t i=0;i<shortcuts.size();i++)WritePrivateProfileStringW(L"launch",std::to_wstring(i).c_str(),shortcuts[i].c_str(),ini.c_str());
        tasks.setPinned(shortcuts);
        search.setSources(shortcuts);
    }
    void position() {
        MONITORINFO monitor={sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(dockMonitor,MONITOR_DEFAULTTONEAREST),&monitor);
        UINT dx=96,dy=96;GetDpiForMonitor(MonitorFromPoint(dockMonitor,MONITOR_DEFAULTTONEAREST),MDT_EFFECTIVE_DPI,&dx,&dy);
        scale=dx/96.f*settings.size/100.f;
        // Keep the panel usable at large Windows text scaling on small displays.
        scale=std::min({scale,(monitor.rcMonitor.right-monitor.rcMonitor.left-20)/SurfaceW,(monitor.rcMonitor.bottom-monitor.rcMonitor.top-20)/SurfaceH});
        hideEditors();tabStart=0;
        bool ok=false;
        if(gfx)ok=gfx->resize((UINT)std::ceil(SurfaceW*scale),(UINT)std::ceil(SurfaceH*scale),scale);
        if(!ok){gfx.reset();gfx=std::make_unique<Graphics>();ok=gfx->init(hwnd,(UINT)std::ceil(SurfaceW*scale),(UINT)std::ceil(SurfaceH*scale),scale);}
        if(!ok) {
            MessageBoxW(nullptr,L"Не удалось создать графическую поверхность DirectComposition.",L"Остров",MB_OK|MB_ICONERROR);PostMessageW(hwnd,WM_CLOSE,0,0);return;
        }
        placeDock();
        if(editFont)DeleteObject(editFont);
        editFont=CreateFontW(-(int)(14*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if(timerFont)DeleteObject(timerFont);
        timerFont=CreateFontW(-(int)(32*scale),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if(edit)SendMessageW(edit,WM_SETFONT,(WPARAM)editFont,TRUE);
        if(timerEdit)SendMessageW(timerEdit,WM_SETFONT,(WPARAM)timerFont,TRUE);
        DEVMODEW mode={};mode.dmSize=sizeof(mode);if(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode)&&mode.dmDisplayFrequency>1)refreshRate=std::clamp((int)mode.dmDisplayFrequency,60,120);
        region();syncEdit();draw();
    }
    float center()const{return std::ceil(SurfaceW*scale)*.5f/scale;}
    Box body()const{return dockBody(dockEdge,currentW,currentH,center()*2,SurfaceH);}
    DockPoint uiOrigin()const{if(dockEdge==1)return {20,(SurfaceH-PanelH)/2};if(dockEdge==3)return {-20,(SurfaceH-PanelH)/2};if(dockEdge==2)return {0,SurfaceH-currentH};return {};}
    DockPoint uiPoint(float x,float y)const{auto o=uiOrigin();return {x-o.x,y-o.y};}
    void placeDock(){
        if(!hwnd)return;MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromPoint(dockMonitor,MONITOR_DEFAULTTONEAREST),&info);auto r=info.rcMonitor;auto b=body();bool side=dockEdge==1||dockEdge==3;float length=(float)(side?r.bottom-r.top:r.right-r.left),along=(side?b.h:b.w)*scale;float ratio=dockMoving?snapDockRatio(dockRatio):dockRatio;float anchor=std::clamp(ratio*length,along/2+12*scale,length-along/2-12*scale);int w=(int)std::ceil(SurfaceW*scale),h=(int)std::ceil(SurfaceH*scale);int x=side?(dockEdge==3?r.left:r.right-w):r.left+(int)std::lround(anchor-w/2.f);int y=side?r.top+(int)std::lround(anchor-h/2.f):(dockEdge==2?r.bottom-h:r.top);RECT old{};GetWindowRect(hwnd,&old);if(old.left!=x||old.top!=y||old.right-old.left!=w||old.bottom-old.top!=h)SetWindowPos(hwnd,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE|SWP_NOOWNERZORDER);
    }
    bool gripAt(POINT p)const{if(dockLocked)return false;MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromPoint(dockMonitor,MONITOR_DEFAULTTONEAREST),&info);auto r=info.rcMonitor;int d=dockEdge==0?p.y-r.top:dockEdge==1?r.right-1-p.x:dockEdge==2?r.bottom-1-p.y:p.x-r.left;return d>=0&&d<=std::max(7,(int)(8*scale));}
    void moveDock(POINT p){
        MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST),&info);auto r=info.rcMonitor;int distances[]={p.y-r.top,r.right-1-p.x,r.bottom-1-p.y,p.x-r.left},edge=(int)(std::min_element(distances,distances+4)-distances);if(edge!=dockEdge&&distances[dockEdge]-distances[edge]<28)edge=dockEdge;
        if(edge!=dockDragEdge){dockGrabOffset=0;dockDragEdge=edge;}dockEdge=edge;dockMonitor={(r.left+r.right)/2,(r.top+r.bottom)/2};bool side=edge==1||edge==3;dockRatio=std::clamp(((side?p.y-r.top:p.x-r.left)-dockGrabOffset)/(side?r.bottom-r.top:r.right-r.left),0.f,1.f);hideEditors();tabStart=headerStart=0;if(gfx)gfx->releaseSnapshot();placeDock();region();draw();
    }
    void region(){
        auto p=islandOutline(body(),dockEdge);HDC dc=CreateCompatibleDC(nullptr);BeginPath(dc);auto point=[&](int i){return POINT{(LONG)std::lround(p[i].x*scale),(LONG)std::lround(p[i].y*scale)};};auto start=point(0);MoveToEx(dc,start.x,start.y,nullptr);for(int k:{1,5,9,13}){POINT curve[]={point(k),point(k+1),point(k+2)};PolyBezierTo(dc,curve,3);if(k<13){auto next=point(k+3);LineTo(dc,next.x,next.y);}}CloseFigure(dc);EndPath(dc);HRGN shape=PathToRegion(dc);DeleteDC(dc);if(!shape)return;HRGN padded=CreateRectRgn(0,0,0,0);int margin=(int)std::ceil(BorderWidth*scale/2)+2;for(int dy=-margin;dy<=margin;dy++)for(int dx=-margin;dx<=margin;dx++){OffsetRgn(shape,dx,dy);CombineRgn(padded,padded,shape,RGN_OR);OffsetRgn(shape,-dx,-dy);}DeleteObject(shape);SetWindowRgn(hwnd,padded,FALSE);
    }
    void animate(bool open) {
        if(open==expanded && targetW==(open?PanelW:240) && (!toastUntil||open))return;
        if(open)priorForeground=GetForegroundWindow();
        if(!open){storeEdit();hoverBlocked=true;}
        expanded=open;outsideSince=0;hoverIndex=-1;
        hideEditors();tabStart=0;
        fromW=currentW;fromH=currentH;
        targetW=open?PanelW:(toastUntil>GetTickCount64()?390.f:240.f);
        targetH=open?PanelH:(toastUntil>GetTickCount64()?62.f:36.f);
        morphStart=moment();morphing=true;
        if(renderPaused){settleAnimations();syncEdit();}
        updateFrames();draw();
    }
    void showToast(std::wstring title,std::wstring detail=L"") {
        toastTitle=std::move(title);toastDetail=std::move(detail);toastUntil=GetTickCount64()+3000;
        if(!expanded){fromW=currentW;fromH=currentH;targetW=390;targetH=62;morphStart=moment();morphing=true;updateFrames();}
        draw();
    }
    void changeTab(int index) {
        if(index==tab)return;
        storeEdit();storeTimer();hideEditors();draw();
        for(int i=0;i<9;i++)headerFrom[i]=headerWidth(i);
        headerStart=moment();tabDirection=tabRank(index)>tabRank(tab)?1:-1;
        if(gfx&&expanded&&!morphing&&!renderPaused){gfx->snapshot();tabStart=moment();}else tabStart=0;
        tab=index;hoverIndex=-1;if(tab==10||tab==14)colorValue=hexColor(colorAt(selectedColor));
        syncEdit();updateFrames();draw();
    }
    void storeEdit() {
        storeTimer();
        if(!edit)return;
        auto s=editText(edit);
        if(editOwner==3)textValue=s;
        if(editOwner==5 && s!=noteValue){noteValue=s;writeNote(s);}
        if(editOwner==6)cityValue=s;
        if(editOwner==9)commandValue=s;
        if(editOwner==10||editOwner==14)colorValue=s;
        if(editOwner==12)searchValue=s;
    }
    unsigned& colorAt(int index){unsigned* colors[]={&Background,&Card,&Accent,&Ink,&Muted,&Hover,&Outline};return appearanceDesktop?desktopLook.colors[std::clamp(index,0,6)]:*colors[std::clamp(index,0,6)];}
    void applyTheme(){
        fileBrowserColors(menuColors());
        if(editBrush)DeleteObject(editBrush);editBrush=CreateSolidBrush(RGB((Card>>16)&255,(Card>>8)&255,Card&255));
        if(edit)InvalidateRect(edit,nullptr,TRUE);if(timerEdit)InvalidateRect(timerEdit,nullptr,TRUE);
    }
    void hideEditors(){if(edit)ShowWindow(edit,SW_HIDE);if(timerEdit)ShowWindow(timerEdit,SW_HIDE);}
    float headerWidth(int i)const{
        float t=headerStart>0?std::clamp((float)((moment()-headerStart)/.26),0.f,1.f):1;
        float ease=t*t*(3-2*t);return headerStart>0?headerFrom[i]+(tabWidth(i,tab)-headerFrom[i])*ease:tabWidth(i,tab);
    }
    void updateFrames(){auto wall=panelWallpaper.snapshot();bool motion=performance.video&&wall.enabled&&wall.video&&wallOpacity>0&&wallShade<100;panelWallpaper.setActive(motion&&!manualHidden&&!renderPaused&&displayActive&&sessionActive);bool full=desktop.needsAudio()&&!systemSuspended&&displayActive&&sessionActive;audio.setAnalysisRate(std::min(performance.analysisFPS,desktop.analysisRate()));audio.fullSpectrum(full);audio.enable(full||(!manualHidden&&!renderPaused&&now.available&&now.playing&&(!expanded||morphing)));frames.update(!manualHidden&&!renderPaused&&(morphing||tabStart>0||headerStart>0||motion||(!expanded&&now.available&&now.playing)),motion&&!morphing&&!tabStart&&!headerStart?performance.videoFPS:refreshRate);}
    DesktopData desktopData(){
        DesktopData d;d.performance=performance;d.network=network.state();d.media=now;d.weather=weather;d.tasks=taskState;d.pinned=shortcuts;d.note=noteValue;
        d.background=desktopLook.colors[0];d.card=desktopLook.colors[1];d.accent=desktopLook.colors[2];d.ink=desktopLook.colors[3];d.muted=desktopLook.colors[4];d.hover=desktopLook.colors[5];d.outline=desktopLook.colors[6];d.outlineWidth=desktopLook.width;d.outlineSmooth=desktopLook.smooth;
        auto stamp=GetTickCount64();d.timerSeconds=timerEnd?(int)((timerEnd>stamp?timerEnd-stamp:0)+999)/1000:timerPaused?(int)((timerPaused+999)/1000):customTimerSeconds;
        d.timerRunning=timerEnd!=0;d.timerPaused=timerPaused!=0;d.volume=volume;d.mutedAudio=muted;d.awake=!systemSuspended&&displayActive&&sessionActive&&runtimeDesktopVisible();return d;
    }
    void desktopAction(int code,const std::wstring& value){
        if(code==DeskSaveNote){if(value!=noteValue){noteValue=value;writeNote(value);if(edit&&editOwner==5&&editText(edit)!=value)SetWindowTextW(edit,value.c_str());}return;}
        if(closing)return;
        if(code==DeskAppearance){openAppearance(true);return;}
        if(code==DeskSearch){storeEdit();openSearch();return;}
        if(code==DeskApps||code==DeskSettings||code==DeskWeather||code==DeskNote){storeEdit();pinned=true;animate(true);changeTab(code==DeskApps?11:code==DeskSettings?8:code==DeskWeather?6:5);SetForegroundWindow(hwnd);return;}
        if(code==DeskAddApp){addShortcut();return;}
        if(code==DeskTimerToggle){if(!timerEnd&&!timerPaused&&!value.empty()){int seconds;if(!parseTimer(value,seconds)){showToast(L"Некорректное время");return;}customTimerSeconds=seconds;}timerAction();return;}
        if(code==DeskSetTimer){int seconds;if(parseTimer(value,seconds)){customTimerSeconds=seconds;timerEnd=timerPaused=0;save();syncEdit();}return;}
        if(code==DeskTimerReset){timerEnd=timerPaused=0;syncEdit();draw();return;}
        if(code==DeskPinApp){if(shortcuts.size()<64&&std::find(shortcuts.begin(),shortcuts.end(),value)==shortcuts.end())shortcuts.push_back(value);save();return;}
        if(code==DeskUnpinApp){std::erase(shortcuts,value);save();return;}
        if(code==DeskReorderApp){auto comma=value.find(L',');if(comma!=std::wstring::npos){int a=_wtoi(value.substr(0,comma).c_str()),b=_wtoi(value.substr(comma+1).c_str());if(a>=0&&b>=0&&a<(int)shortcuts.size()&&b<(int)shortcuts.size()&&a!=b){auto path=shortcuts[a];shortcuts.erase(shortcuts.begin()+a);shortcuts.insert(shortcuts.begin()+b,path);save();}}return;}
        if(code==DeskVolume){volume=std::clamp(volume+_wtoi(value.c_str())/100.f,0.f,1.f);setVolume(volume);return;}
        if(code==DeskToggleMute){toggleMute();muted=getMuted();return;}
        if(code==100)media.control(_wtoi(value.c_str()));
        if(code==101)media.seek(_wtof(value.c_str()));
    }
    void startDesktop(){if(desktopEnabled)desktop.start(hwnd,&audio,[this]{return desktopData();},[this](int code,const std::wstring& value){desktopAction(code,value);});}
    void settleAnimations(){
        if(morphing){currentW=targetW;currentH=targetH;morphing=false;placeDock();region();}
        tabStart=headerStart=0;if(gfx)gfx->releaseSnapshot();lastFrame=moment();
    }
    void refreshVisibility(bool promote=false){
        health::awake(!systemSuspended&&displayActive&&sessionActive);
        shell.setAwake(!systemSuspended&&displayActive&&sessionActive);
        SetPropW(hwnd,L"OwnIsland.Awake",(HANDLE)(UINT_PTR)(!systemSuspended&&displayActive&&sessionActive));
        if(promote&&!systemSuspended&&displayActive&&sessionActive)overlay.raise(hwnd);
        bool paused=systemSuspended||!displayActive||!sessionActive||!runtimeDesktopVisible()||!overlay.visible(hwnd);
        if(paused==renderPaused)return;
        if(paused){
            // Freeze a complete frame, never an intermediate opening/closing frame.
            if(expanded&&!pinned&&!dialog){storeEdit();expanded=false;hoverBlocked=true;targetW=240;targetH=36;morphing=true;}
            settleAnimations();syncEdit();draw();renderPaused=true;meter.fill(0);
        }else{renderPaused=false;settleAnimations();syncEdit();draw();}
        SetPropW(hwnd,L"OwnIsland.RenderPaused",(HANDLE)(UINT_PTR)(renderPaused?1:0));updateFrames();
    }
    void invokeIsland(){
        manualHidden=false;
        if(expanded){pinned=false;animate(false);if(IsWindow(priorForeground))SetForegroundWindow(priorForeground);}
        else{
            if(IsIconic(hwnd))ShowWindow(hwnd,SW_RESTORE);
            pinned=true;animate(true);SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW|SWP_NOACTIVATE);
            SetForegroundWindow(hwnd);refreshVisibility();
        }
    }
    void frame(){
        frames.received();if(renderPaused){updateFrames();return;}double stamp=moment();float dt=(float)std::clamp(stamp-lastFrame,0.,.1);lastFrame=stamp;
        if(morphing){
            float t=std::clamp((float)((stamp-morphStart)/.36),0.f,1.f);
            float ease=t<.5f?4*t*t*t:1-std::pow(-2*t+2,3.f)/2;
            currentW=fromW+(targetW-fromW)*ease;currentH=fromH+(targetH-fromH)*ease;placeDock();region();
            if(t>=1){morphing=false;currentW=targetW;currentH=targetH;syncEdit();}
        }
        if(tabStart>0&&stamp-tabStart>=.23){tabStart=0;gfx->releaseSnapshot();syncEdit();}
        if(headerStart>0&&stamp-headerStart>=.26)headerStart=0;
        auto levels=audio.levels();for(int i=0;i<3;i++){float rate=levels[i]>meter[i]?25.f:7.f;meter[i]+=(levels[i]-meter[i])*(1-std::exp(-dt*rate));}
        draw();updateFrames();
    }
    void storeTimer(){if(timerEdit&&!timerEnd&&!timerPaused){int seconds;if(parseTimer(editText(timerEdit),seconds)&&customTimerSeconds!=seconds){customTimerSeconds=seconds;WritePrivateProfileStringW(L"timer",L"seconds",std::to_wstring(seconds).c_str(),ini.c_str());}}}
    void timerAction(){
        if(timerEnd){timerPaused=timerEnd>GetTickCount64()?timerEnd-GetTickCount64():0;timerEnd=0;}
        else if(timerPaused){timerEnd=GetTickCount64()+timerPaused;timerPaused=0;}
        else {int seconds=customTimerSeconds;if(timerEdit&&IsWindowVisible(timerEdit)&&!parseTimer(editText(timerEdit),seconds)){showToast(L"Некорректное время",L"Введите минуты или время в формате ММ:СС");return;}customTimerSeconds=seconds;timerEnd=GetTickCount64()+(unsigned long long)seconds*1000;save();}
        syncEdit();draw();
    }
    static LRESULT CALLBACK editProc(HWND w,UINT m,WPARAM wp,LPARAM lp) {
        auto a=(Island*)GetWindowLongPtrW(w,GWLP_USERDATA);
        if(m==WM_KEYDOWN&&wp=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){SendMessageW(w,EM_SETSEL,0,-1);return 0;}
        if(m==WM_CHAR&&wp==1)return 0;
        if(m==WM_CHAR&&wp==VK_RETURN&&a->editOwner==12)return 0;
        if(m==WM_CONTEXTMENU){a->editMenu(w);return 0;}
        if(m==WM_KEYDOWN&&a->editOwner==12){
            if(wp==VK_UP||wp==VK_DOWN){a->moveSearch(wp==VK_UP?-1:1);return 0;}
            if(wp==VK_RETURN){a->openSearchResult(a->searchSelected,(GetKeyState(VK_CONTROL)&0x8000)&&(GetKeyState(VK_SHIFT)&0x8000));return 0;}
        }
        if(m==WM_KEYDOWN && wp==VK_ESCAPE){a->pinned=false;a->animate(false);if(a->priorForeground)SetForegroundWindow(a->priorForeground);return 0;}
        if(w==a->timerEdit){if(m==WM_KEYDOWN&&wp==VK_RETURN){a->timerAction();return 0;}return CallWindowProcW(a->oldTimerProc,w,m,wp,lp);}
        if(m==WM_KEYDOWN && wp==VK_RETURN && (a->editOwner==6||a->editOwner==9)){a->storeEdit();if(a->editOwner==6)a->requestWeather();else a->executeCommand();return 0;}
        if(m==WM_KEYDOWN&&wp==VK_RETURN&&(a->editOwner==10||a->editOwner==14)){a->storeEdit();a->applyColor();return 0;}
        return CallWindowProcW(a->oldEditProc,w,m,wp,lp);
    }
    void placeInput(HWND input,Box box,HFONT font,bool multiline){
        auto origin=uiOrigin();int x=(int)std::lround((box.x+origin.x)*scale),y=(int)std::lround((box.y+origin.y)*scale),w=(int)std::lround(box.w*scale),h=(int)std::lround(box.h*scale);
        if(!multiline){HDC dc=GetDC(input);auto old=SelectObject(dc,font);TEXTMETRICW metrics;GetTextMetricsW(dc,&metrics);SelectObject(dc,old);ReleaseDC(input,dc);int inner=metrics.tmHeight+2;y+=(h-inner)/2;h=inner;}
        SetWindowPos(input,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(input,SW_SHOWNOACTIVATE);
    }
    void syncEdit() {
        int owner=tab==3?3:tab==5?5:tab==6&&(weatherEditing||cityValue.empty())?6:tab==9?9:tab==10?10:tab==14?14:tab==12?12:-1;
        if(!expanded || morphing || tabStart>0 || std::abs(currentH-PanelH)>1 || owner<0){hideEditors();return;}
        if(owner!=editOwner) {
            if(edit)DestroyWindow(edit);
            editOwner=owner;
            bool multiline=owner==3||owner==5;
            edit=CreateWindowExW(WS_EX_LAYERED,L"EDIT",L"",WS_CHILD|ES_LEFT|ES_AUTOVSCROLL|(multiline?(ES_MULTILINE|ES_WANTRETURN|WS_VSCROLL):ES_AUTOHSCROLL),0,0,0,0,hwnd,(HMENU)100,GetModuleHandleW(nullptr),nullptr);
            SetLayeredWindowAttributes(edit,0,255,LWA_ALPHA);
            SetWindowLongPtrW(edit,GWLP_USERDATA,(LONG_PTR)this);
            oldEditProc=(WNDPROC)SetWindowLongPtrW(edit,GWLP_WNDPROC,(LONG_PTR)editProc);
            SendMessageW(edit,EM_SETLIMITTEXT,owner==6?200:(owner==10||owner==14)?7:owner==12?512:100000,0);
            SendMessageW(edit,WM_SETFONT,(WPARAM)editFont,TRUE);
            ShowScrollBar(edit,SB_VERT,FALSE);
            auto value=owner==3?textValue:owner==5?noteValue:owner==6?cityValue:(owner==10||owner==14)?colorValue:owner==12?searchValue:commandValue;
            SetWindowTextW(edit,value.c_str());
        }
        Box r=owner==3?Box{40,91,408,176}:owner==5?Box{44,95,410,211}:owner==6?Box{118,169,500,43}:owner==10?Box{476,140,206,42}:owner==14?Box{476,159,206,42}:owner==12?Box{48,86,640,42}:Box{46,96,644,50};
        bool editEnabled=(owner!=10&&owner!=14)||selectedColorEnabled();if((IsWindowEnabled(edit)!=FALSE)!=editEnabled)EnableWindow(edit,editEnabled);
        placeInput(edit,r,editFont,owner==3||owner==5);
        if(owner==12&&searchFocus){searchFocus=false;SetForegroundWindow(hwnd);SetFocus(edit);SendMessageW(edit,EM_SETSEL,(WPARAM)searchValue.size(),(LPARAM)searchValue.size());}
        if(tab==5&&!timerEnd&&!timerPaused){
            if(!timerEdit){
                timerEdit=CreateWindowExW(WS_EX_LAYERED,L"EDIT",L"",WS_CHILD|ES_CENTER|ES_AUTOHSCROLL,0,0,0,0,hwnd,(HMENU)101,GetModuleHandleW(nullptr),nullptr);
                SetLayeredWindowAttributes(timerEdit,0,255,LWA_ALPHA);SetWindowLongPtrW(timerEdit,GWLP_USERDATA,(LONG_PTR)this);
                oldTimerProc=(WNDPROC)SetWindowLongPtrW(timerEdit,GWLP_WNDPROC,(LONG_PTR)editProc);
                SendMessageW(timerEdit,WM_SETFONT,(WPARAM)timerFont,TRUE);SendMessageW(timerEdit,EM_SETLIMITTEXT,12,0);
                SetWindowTextW(timerEdit,duration(customTimerSeconds).c_str());
            }
            placeInput(timerEdit,{488,121,201,57},timerFont,false);
        }else if(timerEdit)ShowWindow(timerEdit,SW_HIDE);
    }
    void ghost(const std::wstring& value,Box box,bool multiline,int owner){
        if(!edit||!IsWindowVisible(edit)||editOwner!=owner)gfx->text(value,box,14,Ink,false,DWRITE_TEXT_ALIGNMENT_LEADING,multiline,L"Segoe UI",multiline?DWRITE_PARAGRAPH_ALIGNMENT_NEAR:DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    void addHit(Box box,std::function<void(float)> action,bool header=false) {hits.push_back({box,std::move(action),header});}
    HCURSOR pointerCursor(){
        POINT p{};GetCursorPos(&p);if(dockPending||gripAt(p))return LoadCursorW(nullptr,IDC_SIZEALL);ScreenToClient(hwnd,&p);auto q=uiPoint(p.x/scale,p.y/scale);bool hand=!expanded;
        if(expanded&&!morphing&&!renderPaused)for(auto& hit:hits)if((!tabStart||hit.header)&&hit.box.contains(q.x,q.y)){hand=true;break;}
        return LoadCursorW(nullptr,hand?IDC_HAND:IDC_ARROW);
    }
    void button(Box r,const std::wstring& label,const wchar_t* glyph,std::function<void()> action,bool active=false,bool enabled=true) {
        int index=(int)hits.size();
        gfx->fill(r,!enabled?mix(Background,Card,.55f):active?mix(Card,Accent,.2f):hoverIndex==index?Hover:Card,8);
        if(glyph)gfx->icon(glyph,{r.x+4,r.y,28,r.h},15,!enabled?mix(Card,Muted,.6f):active?Accent:Muted);
        gfx->text(label,{r.x+(glyph?35:10),r.y,r.w-(glyph?43:20),r.h},12,enabled?Ink:mix(Card,Muted,.7f),false,glyph?DWRITE_TEXT_ALIGNMENT_LEADING:DWRITE_TEXT_ALIGNMENT_CENTER,true);
        if(enabled)addHit(r,[action=std::move(action)](float){action();});
    }
    void header() {
        const wchar_t* icons[]={L"\uE80F",L"\uE71D",L"\uE77F",L"\uE8B7",L"\uE8D2",L"\uE8F2",L"\uE70F",L"\uE706",L"\uE713"};
        float x=34;
        for(int i=0;i<9;i++){
            bool selected=tab==TabOrder[i];float w=headerWidth(i);
            float amount=std::clamp((w-35)/(tabWidth(i,TabOrder[i])-35),0.f,1.f);
            Box r{x,12,w,27};int n=(int)hits.size();
            if(amount>.01f||hoverIndex==n)gfx->fill(r,hoverIndex==n?mix(Background,Hover,.8f):Hover,13,std::max(amount,hoverIndex==n?.65f:0.f));
            gfx->icon(icons[i],{x+4+3*amount,12,27,27},13,mix(Muted,Ink,amount));
            if(amount>.01f){gfx->clip({x+35,12,std::max(0.f,w-43),27});gfx->text(TabNames[i],{x+35,12,tabWidth(i,TabOrder[i])-43,27},10.5f,mix(Background,Ink,amount),true);gfx->unclip();}
            addHit({r.x-1.5f,r.y,r.w+3,r.h},[this,i](float){changeTab(TabOrder[i]);},true);x+=w+3;
        }
        shell.colors(menuColors());
        gfx->text(timeString(),{508,12,45,27},10.5f,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        auto iconButton=[&](float px,const wchar_t* glyph,std::function<void()> fn,bool on=false){Box r{px,12,27,27};if(on||hoverIndex==(int)hits.size())gfx->fill(r,Card,8);gfx->icon(glyph,r,13,on?Accent:Muted);addHit(r,[fn](float){fn();},true);};
        iconButton(472,L"\uE721",[this]{openSearch();},tab==12);
        iconButton(555,shell.overflowVisible()?L"\uE70D":L"\uE70E",[this]{auto origin=uiOrigin();POINT p{(LONG)((582+origin.x)*scale),(LONG)((43+origin.y)*scale)};ClientToScreen(hwnd,&p);if(!shell.hiddenIcons(p))showToast(L"Скрытые значки недоступны");draw();});
        iconButton(583,muted||volume<.001f?L"\uE74F":L"\uE767",[this]{toggleMute();muted=getMuted();draw();});
        iconButton(612,L"\uE720",[this]{ShellExecuteW(hwnd,L"open",L"control.exe",L"mmsys.cpl,,1",nullptr,SW_SHOWNORMAL);});
        if(micState!=0){gfx->line(619,31,632,19,Background,3.8f);gfx->line(619,31,632,19,Muted,1.3f);}
        iconButton(641,L"\uE718",[this]{pinned=!pinned;draw();},pinned);
        iconButton(670,L"\uE713",[this]{changeTab(8);},tab==8||tab==10||tab==13||tab==14||tab==15);
    }
    void mediaCard() {
        Box r{34,51,421,151};gfx->fill(r,Card,12);
        if(now.available && now.cover&&!now.cover->empty())gfx->image(*now.cover,now.revision,{48,65,96,96});
        else {gfx->fill({48,65,96,96},mix(Card,Muted,.15f),10);gfx->icon(L"\uE8D6",{48,65,96,96},31,Muted);}
        std::wstring source=now.source;std::transform(source.begin(),source.end(),source.begin(),towlower);
        gfx->text(now.available?(source.find(L"spotify")!=std::wstring::npos?L"SPOTIFY":L"СЕЙЧАС ИГРАЕТ"):L"МУЗЫКА",{159,65,280,16},9.5f,Muted,true);
        gfx->text(now.available?now.title:L"Ничего не воспроизводится",{159,85,280,25},15,Ink,true);
        gfx->text(now.available?now.artist:L"",{159,112,280,20},11.5f,Muted);
        gfx->text(duration(draggingSeek?seekRatio*now.duration:now.position),{159,139,36,16},9,Muted);
        gfx->text(duration(now.duration),{399,139,40,16},9,Muted,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
        float progress=draggingSeek?seekRatio:now.duration>0?std::clamp((float)(now.position/now.duration),0.f,1.f):0;
        gfx->fill({198,147,194,2},mix(Card,Muted,.5f),1);if(progress>0)gfx->fill({198,147,194*progress,2},Muted,1);if((musicOver||draggingSeek)&&now.duration>0)gfx->fill({194+194*progress,144,8,8},Ink,4);
        addHit({196,139,198,16},[this](float f){if(now.duration>0){draggingSeek=true;seekRatio=f;SetCapture(hwnd);draw();}});
        auto control=[&](float x,const wchar_t* glyph,int action,bool big=false){Box b{x,161,big?34.f:30.f,34};if(big)gfx->fill(b,Ink,17,1,false);gfx->icon(glyph,b,big?15:14,big?Card:Muted);addHit(b,[this,action](float){media.control(action);});};
        control(249,L"\uE892",2);control(285,now.playing?L"\uE769":L"\uE768",1,true);control(325,L"\uE893",3);
        gfx->fill({34,210,421,37},Card,9);
        gfx->icon(muted?L"\uE74F":L"\uE767",{44,210,27,37},14);
        addHit({40,210,36,37},[this](float){openTarget(L"sndvol.exe");});
        gfx->fill({83,227,257,3},mix(Card,Muted,.5f),1.5f);gfx->fill({83,227,257*volume,3},Muted,1.5f);gfx->fill({79+257*volume,224,8,8},Ink,4);
        gfx->text(std::to_wstring((int)std::round(volume*100)),{352,210,35,37},10,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        addHit({77,214,269,30},[this](float f){volume=f;setVolume(f);draggingVolume=true;SetCapture(hwnd);draw();});
        gfx->icon(L"\uE7F5",{397,210,45,37},14);addHit({394,210,50,37},[](float){openTarget(L"sndvol.exe");});
    }
    void home() {
        mediaCard();
        struct Tool {const wchar_t* name;const wchar_t* icon;};
        Tool tools[]={{L"Область",L"\uE7C5"},{L"Экран",L"\uE91B"},{L"Текст",L"\uE8D2"},{L"Буфер",L"\uE77F"},{L"Пипетка",L"\uE790"},{L"Безформ",L"\uE8A7"},{L"Файлы",L"\uE8B7"},{L"Команда",L"\uE721"}};
        for(int i=0;i<8;i++)button({466.f+(i%2)*123.f,51.f+(i/2)*50.f,113,46},tools[i].name,tools[i].icon,[this,i]{
            if(i==0){pinned=false;animate(false);openTarget(L"ms-screenclip:");}
            if(i==1){captureScreen(hwnd);showToast(L"Скриншот скопирован");}
            if(i==2)changeTab(3);
            if(i==3)changeTab(1);
            if(i==4)startColorPicker();
            if(i==5){auto s=clipboardText(hwnd);if(!s.empty()){setClipboard(hwnd,s);showToast(L"Форматирование удалено");}else showToast(L"В буфере нет текста");}
            if(i==6)changeTab(2);
            if(i==7)changeTab(9);
        });
        taskPanel();
    }
    void taskPanel(){
        gfx->fill({34,260,668,57},Card,10);taskSlots.clear();
        auto& groups=taskState->groups;
        Box desktop{650,267,44,43};if(hoverIndex==(int)hits.size()||desktopShown)gfx->fill(desktop,Hover,7);gfx->icon(L"\uE7F4",desktop,18,desktopShown?Accent:Muted);addHit(desktop,[this](float){toggleDesktop();});
        gfx->line(644,273,644,304,mix(Card,Muted,.35f));
        if(groups.empty())return;
        bool paging=groups.size()>10;int count=paging?10:(int)groups.size();
        taskOffset=std::clamp(taskOffset,0,std::max(0,(int)groups.size()-count));
        float left=paging?65.f:43.f;
        if(paging){
            Box back{38,270,23,35},next{617,270,23,35};
            gfx->icon(L"\uE76B",back,11,taskOffset?Ink:Muted);gfx->icon(L"\uE76C",next,11,taskOffset+count<(int)groups.size()?Ink:Muted);
            addHit(back,[this](float){taskOffset=std::max(0,taskOffset-10);draw();});
            addHit(next,[this,count](float){taskOffset=std::min(std::max(0,(int)taskState->groups.size()-count),taskOffset+10);draw();});
        }
        for(int slot=0;slot<count;slot++){
            auto group=groups[taskOffset+slot];float x=left+slot*(paging?55.f:59.f);
            Box r{x,264,54,49};bool hovered=hoverIndex==(int)hits.size();
            if(group.active||hovered)gfx->fill(r,group.active?mix(Card,Accent,.10f):Hover,8);
            if(group.icon)gfx->pixels(group.icon,{x+13,270,28,28});else gfx->icon(L"\uE737",{x+10,270,34,30},22,Muted);
            gfx->fill({x+(group.active?16.f:23.f),305,group.active?22.f:8.f,2},group.active?Accent:Muted,1);
            if(group.windows.size()>1){gfx->fill({x+34,267,16,14},Hover,5);gfx->text(std::to_wstring(group.windows.size()),{x+34,267,16,14},8,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);}
            taskSlots.push_back({r,group});addHit(r,[this,group](float){chooseTask(group,true);});
        }
    }
    void appsPage(){
        shortcutSlots.clear();gfx->text(L"Приложения",{36,56,430,30},18,Ink,true);
        button({555,54,147,32},L"Добавить",L"\uE710",[this]{addShortcut();});
        if(shortcuts.empty()){empty(L"\uE71D",L"Нет закреплённых приложений",L"");return;}
        int rows=((int)shortcuts.size()+7)/8;appOffset=std::clamp(appOffset,0,std::max(0,rows-3));
        for(int slot=0;slot<24;slot++){
            size_t i=(size_t)(appOffset*8+slot);if(i>=shortcuts.size())break;
            float x=34.f+(slot%8)*84.f,y=98.f+(slot/8)*74.f;Box r{x,y,76,64};
            gfx->fill(r,hoverIndex==(int)hits.size()?Hover:Card,10);
            auto icon=taskState->shortcuts.find(shortcuts[i]);
            if(icon!=taskState->shortcuts.end())gfx->pixels(icon->second,{x+20,y+14,36,36});else gfx->icon(L"\uE8A5",{x+20,y+14,36,36},25,Accent);
            shortcutSlots.push_back({r,i});addHit(r,[this,i](float){if(i<shortcuts.size())openPinnedTarget(shortcuts[i],taskState);});
        }
        if(rows>3){float track=218,thumb=std::max(20.f,track*3/rows);gfx->fill({707,98,2,track},Card,1);gfx->fill({707,98+(track-thumb)*appOffset/(rows-3),2,thumb},Muted,1);}
    }
    void forgetDesktop(){desktopShown=false;desktopWindows.clear();desktopFocus={};desktopSettleUntil=0;SetPropW(hwnd,L"OwnIsland.DesktopShown",nullptr);if(expanded&&tab==0)draw();}
    void desktopWindowOpened(){
        if(!desktopShown||GetTickCount64()<desktopSettleUntil)return;
        for(auto& group:taskState->groups)for(auto& window:group.windows)if(validTaskWindow(window)&&!IsIconic(window.hwnd)&&isTaskWindow(window.hwnd)){forgetDesktop();return;}
    }
    void toggleDesktop(){
        pinned=false;animate(false);
        auto windows=taskWindows();
        // A manually restored/new window starts a new hide cycle immediately.
        if(desktopShown&&std::any_of(windows.begin(),windows.end(),[](auto& w){return !IsIconic(w.hwnd);}))forgetDesktop();
        if(desktopShown){
            for(auto& entry:desktopWindows)if(validTaskWindow(entry.window)&&IsIconic(entry.window.hwnd))ShowWindowAsync(entry.window.hwnd,entry.maximized?SW_SHOWMAXIMIZED:SW_RESTORE);
            if(validTaskWindow(desktopFocus))SetForegroundWindow(desktopFocus.hwnd);
            forgetDesktop();
        }else{
            HWND foreground=GetForegroundWindow();desktopWindows.clear();desktopFocus={};desktopSettleUntil=GetTickCount64()+600;
            for(auto& window:windows)if(!IsIconic(window.hwnd)&&validTaskWindow(window)){
                desktopWindows.push_back({window,IsZoomed(window.hwnd)!=FALSE});
                if(window.hwnd==foreground)desktopFocus=window;
                ShowWindowAsync(window.hwnd,SW_MINIMIZE);
            }
            if(!desktopFocus.hwnd&&!desktopWindows.empty())desktopFocus=desktopWindows.front().window;
            desktopShown=!desktopWindows.empty();
        }
        SetPropW(hwnd,L"OwnIsland.DesktopShown",(HANDLE)(UINT_PTR)(desktopShown?1:0));tasks.refresh();draw();
        if(desktopShown||desktopWindows.empty())desktop.reveal();
    }
    void chooseTask(const TaskGroup& group,bool toggle){
        if(group.windows.empty())return;
        if(group.windows.size()==1){activateTaskWindow(group.windows.front(),toggle&&group.active);tasks.refresh();return;}
        HWND foreground=GetForegroundWindow();std::vector<MenuItem> items;
        int limit=std::min(14,(int)group.windows.size());
        for(int i=0;i<limit;i++)items.push_back({group.windows[i].title,L"\uE737",100+i,foreground==group.windows[i].hwnd});
        if(group.windows.size()>14)items.push_back({L"Следующее окно",L"\uE76C",1});
        POINT p;GetCursorPos(&p);dialog=true;int result=customMenu(hwnd,p,items,menuColors());dialog=false;
        if(result>=100&&result<100+limit)activateTaskWindow(group.windows[result-100]);
        if(result==1){auto it=std::find_if(group.windows.begin(),group.windows.end(),[&](auto& w){return w.hwnd==foreground;});size_t index=it==group.windows.end()?0:((size_t)(it-group.windows.begin())+1)%group.windows.size();activateTaskWindow(group.windows[index]);}
        tasks.refresh();
    }
    void taskMenu(const TaskGroup& group){
        if(group.windows.empty())return;
        std::vector<MenuItem> items={{group.windows.size()>1?L"Выбрать окно":L"Переключиться",L"\uE737",1},{L"Свернуть",L"\uE921",2}};
        if(!group.path.empty()){
            bool exists=std::any_of(shortcuts.begin(),shortcuts.end(),[&](auto& p){return _wcsicmp(p.c_str(),group.path.c_str())==0;});
            items.push_back({exists?L"Открепить из приложений":L"Закрепить в приложениях",L"\uE718",3,exists});
        }
        items.push_back({group.windows.size()>1?L"Закрыть окна":L"Закрыть окно",L"\uE711",4});
        POINT p;GetCursorPos(&p);dialog=true;int result=customMenu(hwnd,p,items,menuColors());dialog=false;
        if(result==1)chooseTask(group,false);
        if(result==2)for(auto& w:group.windows)if(validTaskWindow(w))ShowWindowAsync(w.hwnd,SW_MINIMIZE);
        if(result==3){auto it=std::find_if(shortcuts.begin(),shortcuts.end(),[&](auto& path){return _wcsicmp(path.c_str(),group.path.c_str())==0;});if(it!=shortcuts.end())shortcuts.erase(it);else if(shortcuts.size()<64)shortcuts.push_back(group.path);else showToast(L"Достигнут лимит приложений");save();draw();}
        if(result==4)for(auto& w:group.windows)if(validTaskWindow(w))PostMessageW(w.hwnd,WM_CLOSE,0,0);
        tasks.refresh();
    }
    void empty(const wchar_t* glyph,const std::wstring& title,const std::wstring& detail) {
        gfx->icon(glyph,{310,88,116,66},37,0xa6a0ae);
        gfx->text(title,{75,165,586,31},18,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);
        gfx->text(detail,{75,201,586,45},12,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER,true);
    }
    void clipboardPage() {
        gfx->text(L"Буфер обмена",{36,56,450,30},18,Ink,true);
        if(!clipboardOn){empty(L"\uE77F",L"Буфер недоступен",L"");return;}
        button({548,55,153,30},L"Очистить историю",L"\uE74D",[this]{clips.clear();draw();});
        if(clips.empty()){empty(L"\uE77F",L"Пока пусто",L"");return;}
        clipOffset=std::clamp(clipOffset,0,std::max(0,(int)clips.size()-4));
        for(size_t row=0;row<4&&row+clipOffset<clips.size();row++){
            size_t i=row+clipOffset;
            Box r{35,96.f+(float)row*50,666,44};gfx->fill(r,hoverIndex==(int)hits.size()?Hover:Card,8);
            gfx->icon(L"\uE8A5",{43,r.y,30,44},14);gfx->text(clips[i],{84,r.y,573,44},12,Ink);
            gfx->icon(L"\uE8C8",{660,r.y,30,44},13,Muted);
            addHit(r,[this,i](float){if(setClipboard(hwnd,clips[i]))showToast(L"Текст скопирован");});
        }
    }
    void filesPage() {
        gfx->text(L"Полка для файлов",{36,56,450,30},18,Ink,true);
        button({553,55,147,30},L"Добавить файл",L"\uE710",[this]{pickFile();});
        if(files.empty()){empty(L"\uE8B7",L"Нет файлов",L"");return;}
        fileOffset=std::clamp(fileOffset,0,std::max(0,(int)files.size()-4));
        for(size_t row=0;row<4&&row+fileOffset<files.size();row++) {
            size_t i=row+fileOffset;
            Box r{35,96.f+(float)row*50,666,44};gfx->fill(r,Card,8);
            gfx->icon(L"\uE8A5",{43,r.y,32,44},15,Accent);gfx->text(fileName(files[i]),{84,r.y+3,562,20},12,Ink);
            gfx->text(files[i],{84,r.y+23,562,16},9,Muted);
            addHit({r.x,r.y,620,r.h},[this,i](float){openTarget(files[i]);});
            gfx->icon(L"\uE711",{660,r.y+4,30,36},11,Muted);addHit({659,r.y,36,r.h},[this,i](float){files.erase(files.begin()+i);draw();});
        }
    }
    void textPage() {
        gfx->text(L"Инструменты для текста",{36,53,600,28},16,Ink,true);
        gfx->fill({34,85,426,193},Card,10);
        ghost(textValue,{43,92,405,175},true,3);
        const wchar_t* labels[]={L"ВЕРХНИЙ РЕГИСТР",L"нижний регистр",L"Исправить раскладку",L"Каждое Слово",L"Убрать лишние пробелы"};
        for(int i=0;i<5;i++)button({471,85.f+i*39,230,33},labels[i],nullptr,[this,i]{storeEdit();textValue=transformText(textValue,i);SetWindowTextW(edit,textValue.c_str());draw();});
        button({34,289,205,30},L"Вставить из буфера",L"\uE77F",[this]{auto s=clipboardText(hwnd);if(!s.empty())SetWindowTextW(edit,s.c_str());});
        button({250,289,210,30},L"Копировать результат",L"\uE8C8",[this]{storeEdit();setClipboard(hwnd,textValue);showToast(L"Результат скопирован");});
    }
    void chatPage() {empty(L"\uE8F2",L"Чат не подключён",L"");}
    void notesPage() {
        gfx->text(L"Заметка",{36,53,430,28},16,Ink,true);
        gfx->fill({34,85,430,231},Card,10);
        ghost(noteValue,{44,95,410,211},true,5);
        Box r{476,85,226,231};gfx->fill(r,Card,10);
        gfx->text(L"ТАЙМЕР",{490,99,195,20},10,Muted,true);
        unsigned long long remaining=timerPaused?timerPaused:timerEnd>GetTickCount64()?timerEnd-GetTickCount64():0;
        if(timerEnd||timerPaused||!timerEdit||!IsWindowVisible(timerEdit))gfx->text(duration(remaining?remaining/1000.0:customTimerSeconds),{488,121,201,57},32,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);
        for(int i=0;i<3;i++){int minutes=i==0?5:i==1?15:25;button({488.f+i*68,187,62,27},std::to_wstring(minutes)+L" мин",nullptr,[this,minutes]{customTimerSeconds=minutes*60;timerEnd=GetTickCount64()+minutes*60000;timerPaused=0;if(timerEdit)SetWindowTextW(timerEdit,duration(customTimerSeconds).c_str());save();syncEdit();draw();});}
        button({488,225,201,34},timerEnd?L"Пауза":timerPaused?L"Продолжить":L"Начать",timerEnd?L"\uE769":L"\uE768",[this]{timerAction();});
        button({488,267,201,34},L"Сбросить",L"\uE72C",[this]{timerEnd=timerPaused=0;if(timerEdit)SetWindowTextW(timerEdit,duration(customTimerSeconds).c_str());syncEdit();draw();});
    }
    void loadForecast(){
        if(cityValue.empty()||!GetPrivateProfileIntW(L"forecast",L"available",0,ini.c_str()))return;
        auto field=[this](const wchar_t* key){wchar_t text[512]={};GetPrivateProfileStringW(L"forecast",key,L"",text,512,ini.c_str());return std::wstring(text);};
        if(field(L"request")!=cityValue)return;
        weather.city=field(L"city");weather.temperature=wcstod(field(L"temperature").c_str(),nullptr);weather.wind=wcstod(field(L"wind").c_str(),nullptr);weather.humidity=_wtoi(field(L"humidity").c_str());weather.code=_wtoi(field(L"code").c_str());
        for(int i=0;i<7;i++){auto prefix=L"day"+std::to_wstring(i);auto date=field((prefix+L"date").c_str());if(date.size()!=10)break;weather.days.push_back({date,wcstod(field((prefix+L"low").c_str()).c_str(),nullptr),wcstod(field((prefix+L"high").c_str()).c_str(),nullptr),_wtoi(field((prefix+L"code").c_str()).c_str())});}
        weather.available=!weather.city.empty();
    }
    void saveForecast(){
        if(!weather.available)return;
        auto field=[this](const wchar_t* key,const std::wstring& value){WritePrivateProfileStringW(L"forecast",key,value.c_str(),ini.c_str());};
        field(L"available",L"1");field(L"request",cityValue);field(L"city",weather.city);field(L"temperature",std::to_wstring(weather.temperature));field(L"wind",std::to_wstring(weather.wind));field(L"humidity",std::to_wstring(weather.humidity));field(L"code",std::to_wstring(weather.code));
        for(size_t i=0;i<weather.days.size();i++){auto prefix=L"day"+std::to_wstring(i);auto& d=weather.days[i];field((prefix+L"date").c_str(),d.date);field((prefix+L"low").c_str(),std::to_wstring(d.low));field((prefix+L"high").c_str(),std::to_wstring(d.high));field((prefix+L"code").c_str(),std::to_wstring(d.code));}
    }
    void weatherPage() {
        if(weatherEditing||cityValue.empty()){
            gfx->icon(L"\uE706",{310,67,116,57},36,Accent);
            gfx->text(L"Погода в вашем городе",{75,129,586,30},19,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);
            gfx->fill({104,169,528,43},Card,9);
            ghost(cityValue,{118,169,500,43},false,6);
            button({248,231,240,36},weatherBusy?L"Загружаю прогноз…":L"Показать прогноз",L"\uE72C",[this]{storeEdit();requestWeather();});
            if(!weather.error.empty())gfx->text(weather.error,{75,278,586,44},11,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER,true);
            return;
        }
        if(!weather.available){
            gfx->icon(L"\uE706",{310,80,116,66},37,Accent);gfx->text(cityValue,{75,158,586,34},19,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);
            gfx->text(weatherBusy?L"Загружаю прогноз…":weather.error,{75,200,586,44},12,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER,true);
            button({34,280,214,34},L"Другой город",L"\uE70F",[this]{weatherEditing=true;editOwner=-1;syncEdit();draw();});
            button({488,280,214,34},L"Обновить",L"\uE72C",[this]{requestWeather();});return;
        }
        gfx->fill({34,52,222,264},0x243743,12);
        gfx->text(weather.city,{49,63,193,43},13,0xe4edf4,true,false?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING,true);
        gfx->icon(weather.code<=3?L"\uE706":L"\uE9C4",{44,117,50,50},31,0xddd7ad);
        gfx->text(roundNumber(weather.temperature)+L"°",{96,110,144,71},49,0xf1f3f7,true);
        gfx->text(weatherLabel(weather.code),{49,185,190,27},14,0xd3e0e9);
        gfx->text(L"Влажность     "+std::to_wstring(weather.humidity)+L" %",{49,234,192,24},11,0xa3bac9);
        gfx->text(L"Ветер              "+roundNumber(weather.wind)+L" км/ч",{49,262,192,24},11,0xa3bac9);
        gfx->text(L"ПРОГНОЗ НА НЕДЕЛЮ",{274,54,360,26},10,Muted,true);
        for(size_t i=0;i<weather.days.size();i++){
            auto& d=weather.days[i];float y=87.f+i*29;
            gfx->text(i==0?L"Сегодня":d.date.substr(8,2)+L"."+d.date.substr(5,2),{278,y,82,26},11,Ink);
            gfx->icon(d.code<=3?L"\uE706":L"\uE9C4",{360,y,28,26},14,0xa9baca);
            gfx->text(roundNumber(d.low)+L"°",{396,y,42,26},11,Muted,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
            gfx->fill({455,y+12,140,3},0x34363e,2);
            gfx->fill({455,y+12,140*std::clamp((float)((d.high-d.low+4)/24),.18f,1.f),3},0x9bb6c7,2);
            gfx->text(roundNumber(d.high)+L"°",{611,y,55,26},11,Ink,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        button({383,294,160,27},L"Обновить",L"\uE72C",[this]{requestWeather();});
        button({557,294,144,27},L"Другой город",L"\uE70F",[this]{weatherEditing=true;editOwner=-1;syncEdit();draw();});
    }
    void controlPage() {
        auto card=[&](float x,const wchar_t* title,const wchar_t* subtitle,const wchar_t* glyph,std::function<void()> fn){Box b{x,55,214,144};gfx->fill(b,hoverIndex==(int)hits.size()?Hover:Card,12);gfx->fill({x+14,69,34,34},0x38453e,17);if(!wcscmp(glyph,L"\uE701")){float cx=x+31,cy=95;for(float radius:{5.f,10.f,15.f})for(int i=0;i<18;i++){float a=(-135+i*5)*3.14159265f/180,b=a+5*3.14159265f/180;gfx->line(cx+radius*std::cos(a),cy+radius*std::sin(a),cx+radius*std::cos(b),cy+radius*std::sin(b),Accent,1.5f);}gfx->fill({cx-1.4f,cy-1.4f,2.8f,2.8f},Accent,1.4f);}else gfx->icon(glyph,{x+14,69,34,34},17,Accent);gfx->text(title,{x+15,131,186,24},14,Ink,true);gfx->text(subtitle,{x+15,160,185,22},10,Muted);addHit(b,[fn](float){fn();});};
        card(34,L"Сеть",network.state()==NetworkKind::Wifi?L"Wi-Fi":network.state()==NetworkKind::Ethernet?L"Ethernet":network.state()==NetworkKind::Disconnected?L"Нет подключения":L"Подключено",networkGlyph(network.state()),[]{openTarget(L"ms-settings:network");});
        card(261,L"Bluetooth",L"Устройства и подключения",L"\uE702",[]{openTarget(L"ms-settings:bluetooth");});
        card(488,L"Оформление",L"Тема и цвета Windows",L"\uE790",[]{openTarget(L"ms-settings:colors");});
        const wchar_t* names[]={L"Блокировка",L"Сон",L"Перезагрузка",L"Выключение"};
        const wchar_t* glyphs[]={L"\uE72E",L"\uE708",L"\uE777",L"\uE7E8"};
        for(int i=0;i<4;i++)button({34.f+i*170,213,158,57},names[i],glyphs[i],[this,i]{powerAction(i);});
        if(!confirmAction.empty() && GetTickCount64()<confirmUntil)gfx->text(L"Нажмите «"+confirmAction+L"» ещё раз для подтверждения",{36,283,660,34},11,0xe5cfa5,false,DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    bool isStartup() {
        HKEY key;bool on=false;
        if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS){on=RegQueryValueExW(key,L"OwnIsland",nullptr,nullptr,nullptr,nullptr)==ERROR_SUCCESS;RegCloseKey(key);}return on;
    }
    void refreshStartupLauncher(){if(!StateDirectoryOverride.empty())return;wchar_t module[32768];GetModuleFileNameW(nullptr,module,32768);auto launcher=std::filesystem::path(module).parent_path()/L"Island.Start.exe";if(!std::filesystem::exists(launcher))return;HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE|KEY_SET_VALUE,&key)!=ERROR_SUCCESS)return;wchar_t current[32768]={};DWORD size=sizeof(current),type=0;if(RegQueryValueExW(key,L"OwnIsland",nullptr,&type,(BYTE*)current,&size)==ERROR_SUCCESS&&type==REG_SZ){current[32767]=0;std::wstring expected=L"\""+std::wstring(module)+L"\"";if(!_wcsicmp(current,expected.c_str())||!_wcsicmp(current,module)){auto command=L"\""+launcher.wstring()+L"\"";RegSetValueExW(key,L"OwnIsland",0,REG_SZ,(const BYTE*)command.c_str(),(DWORD)((command.size()+1)*sizeof(wchar_t)));}}RegCloseKey(key);}
    void startupToggle() {
        bool enabled=isStartup();HKEY key;
        if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS){showToast(L"Не удалось изменить автозапуск");return;}
        if(enabled)RegDeleteValueW(key,L"OwnIsland");
        else {wchar_t path[32768];GetModuleFileNameW(nullptr,path,32768);auto starter=std::filesystem::path(path).parent_path()/L"Island.Start.exe";if(std::filesystem::exists(starter))wcscpy_s(path,starter.c_str());std::wstring command=L"\""+std::wstring(path)+L"\"";RegSetValueExW(key,L"OwnIsland",0,REG_SZ,(const BYTE*)command.c_str(),(DWORD)((command.size()+1)*sizeof(wchar_t)));}
        RegCloseKey(key);draw();
    }
    MenuColors menuColors()const{return {Background,Hover,Ink,Muted,Accent,Outline,BorderWidth,BorderSmooth};}
    void settingsPage() {
        auto toggle=[&](float y,const wchar_t* label,bool on,std::function<void()> fn){Box b{34,y,668,26};gfx->text(label,{47,y,580,26},12,Ink);gfx->fill({646,y+3,39,21},on?Accent:mix(Card,Muted,.5f),11);gfx->fill({on?666.f:649.f,y+6,15,15},on?Ink:Muted,8);addHit(b,[fn](float){fn();});};
        toggle(54,L"Запускать вместе с Windows",isStartup(),[this]{startupToggle();});
        toggle(82,L"Раскрывать при наведении",settings.hover,[this]{settings.hover=!settings.hover;save();draw();});
        toggle(110,L"Показывать секунды",settings.seconds,[this]{settings.seconds=!settings.seconds;save();draw();});
        toggle(138,L"Рабочий стол",desktopEnabled,[this]{desktopEnabled=!desktopEnabled;if(desktopEnabled)startDesktop();else desktop.stop();save();updateFrames();draw();});
        toggle(166,L"Зафиксировать положение",dockLocked,[this]{dockLocked=!dockLocked;saveDock();draw();});
        gfx->text(L"Файловый менеджер",{47,201,215,27},12,Ink);
        button({274,201,202,27},L"Island",nullptr,[this]{setWindowsFileManager(false);draw();});
        button({486,201,216,27},L"Проводник Windows",nullptr,[this]{setWindowsFileManager(true);draw();});
        gfx->stroke(windowsFileManager()?Box{486,201,216,27}:Box{274,201,202,27},Accent,8,1.5f);
        gfx->text(L"Размер",{47,233,140,27},12,Ink);
        button({503,233,40,27},L"−",nullptr,[this]{settings.size=std::max(80,settings.size-10);save();position();});
        gfx->text(std::to_wstring(settings.size)+L" %",{550,233,96,27},12,Ink,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        button({655,233,40,27},L"+",nullptr,[this]{settings.size=std::min(150,settings.size+10);save();position();});
        button({34,269,218,34},L"Оформление островка",L"\uE790",[this]{openAppearance(false);});
        button({264,269,218,34},L"Оформление стола",L"\uE8B9",[this]{openAppearance(true);},false,desktopEnabled);
        button({494,269,208,34},L"Производительность",L"\uE713",[this]{changeTab(15);});
        gfx->text(L"By Shizuka | Tg @idontluvyou",{34,307,668,20},10,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void performancePage(){
        gfx->text(L"Производительность",{36,53,370,32},18,Ink,true);
        auto commit=[this]{performance.save(ini);updateFrames();draw();};
        button({438,53,128,31},L"Экономичный",nullptr,[this,commit]{performance.desktopFPS=15;performance.videoFPS=15;performance.analysisFPS=10;commit();});button({578,53,124,31},L"Обычный",nullptr,[this,commit]{performance.desktopFPS=30;performance.videoFPS=30;performance.analysisFPS=30;commit();});
        auto choices=[&](float y,const wchar_t* name,int current,std::function<void(int)> set,bool enabled=true){gfx->text(name,{47,y,215,35},12,Ink);int choices[]={10,15,30,60};for(int i=0;i<4;i++){int n=choices[i];button({274+i*108.f,y,100,35},std::to_wstring(n)+L" Гц",nullptr,[set,n,commit]{set(n);commit();},false,enabled);if(current==n&&enabled)gfx->stroke({274+i*108.f,y,100,35},Accent,10,1.5f);}};
        choices(109,L"Рабочий стол",performance.desktopFPS,[this](int n){performance.desktopFPS=n;},desktopEnabled);choices(165,L"Видео мини-панели",performance.videoFPS,[this](int n){performance.videoFPS=n;},performance.video&&panelWallpaper.snapshot().enabled&&panelWallpaper.snapshot().video);choices(221,L"Предел анализа звука",performance.analysisFPS,[this](int n){performance.analysisFPS=n;});
        button({34,285,128,32},L"Назад",L"\uE72B",[this]{changeTab(8);});button({178,285,524,32},performance.video?L"Видео и GIF включены":L"Видео и GIF на паузе",performance.video?L"\uE73D":L"\uE739",[this,commit]{performance.video=!performance.video;commit();});
    }
    void wallValue(int index,float value){if(index==0)wallOpacity=(int)std::lround(value*100);if(index==1)wallShade=(int)std::lround(value*100);if(index==2)wallBlur=(int)std::lround(value*30);updateFrames();}
    void saveWallpaper(){WritePrivateProfileStringW(L"panel",L"wallpaper-opacity",std::to_wstring(wallOpacity).c_str(),ini.c_str());WritePrivateProfileStringW(L"panel",L"wallpaper-shade",std::to_wstring(wallShade).c_str(),ini.c_str());WritePrivateProfileStringW(L"panel",L"wallpaper-blur",std::to_wstring(wallBlur).c_str(),ini.c_str());}
    void wallpaperPage(){
        auto wall=panelWallpaper.snapshot();gfx->text(L"Обои",{36,53,250,32},18,Ink,true);
        button({438,53,128,31},L"Изображение",L"\uE8B9",[this]{dialog=true;panelWallpaper.choose(hwnd);dialog=false;updateFrames();draw();});
        button({578,53,124,31},L"Видео",L"\uE714",[this]{dialog=true;panelWallpaper.choose(hwnd,true);dialog=false;updateFrames();draw();},false,performance.video);
        gfx->text(wall.loading?L"Загрузка…":wall.error.empty()?wall.name:wall.error,{47,89,645,24},11,wall.error.empty()?Muted:0xe7a29a);
        const wchar_t* names[]={L"Непрозрачность",L"Затемнение",L"Размытие"};int values[]={wallOpacity,wallShade,wallBlur};
        for(int i=0;i<3;i++){bool enabled=wall.enabled;float y=121.f+i*51;gfx->text(names[i],{47,y,390,23},12,enabled?Ink:Muted);gfx->text(std::to_wstring(values[i])+(i==2?L" px":L" %"),{595,y,94,23},11,Muted,false,DWRITE_TEXT_ALIGNMENT_TRAILING);float amount=values[i]/(i==2?30.f:100.f);gfx->fill({48,y+32,640,3},Hover,1);gfx->fill({48,y+32,640*amount,3},enabled?Accent:Hover,1);gfx->fill({43+640*amount,y+28,10,11},enabled?Ink:mix(Card,Muted,.6f),5);if(enabled)addHit({42,y+23,652,23},[this,i](float ratio){wallDrag=i;wallValue(i,ratio);SetCapture(hwnd);draw();});}
        button({34,283,190,33},L"Настройки",L"\uE72B",[this]{changeTab(8);});
        button({512,283,190,33},L"Убрать обои",L"\uE711",[this]{panelWallpaper.clear();WritePrivateProfileStringW(L"panel",L"wallpaper-photo",L"0",ini.c_str());updateFrames();draw();},false,wall.enabled||wall.loading);
    }
    float& selectedWidth(){return appearanceDesktop?desktopLook.width:BorderWidth;}
    bool& selectedSmooth(){return appearanceDesktop?desktopLook.smooth:BorderSmooth;}
    void openAppearance(bool desktop){if(desktop&&!desktopEnabled)return;storeEdit();appearanceDesktop=desktop;pinned=true;animate(true);colorValue=hexColor(colorAt(selectedColor));changeTab(10);if(edit&&editOwner==10)SetWindowTextW(edit,colorValue.c_str());}
    bool selectedColorEnabled(){return selectedColor!=6||selectedWidth()>0;}
    void applyColor(){if(!selectedColorEnabled()||(appearanceDesktop&&!desktopEnabled))return;unsigned color;if(!parseColor(colorValue,color)){showToast(L"Некорректный цвет",L"Введите HEX-код из шести символов");return;}colorAt(selectedColor)=color;applyTheme();save();if(tab==14)changeTab(10);else draw();}
    static unsigned hsv(float h,float sat,float value){h=std::fmod(h+360,360.f)/60;int segment=(int)h;float f=h-segment,p=value*(1-sat),q=value*(1-sat*f),t=value*(1-sat*(1-f));float r=0,g=0,b=0;switch(segment){case 0:r=value;g=t;b=p;break;case 1:r=q;g=value;b=p;break;case 2:r=p;g=value;b=t;break;case 3:r=p;g=q;b=value;break;case 4:r=t;g=p;b=value;break;default:r=value;g=p;b=q;}return ((unsigned)std::lround(r*255)<<16)|((unsigned)std::lround(g*255)<<8)|(unsigned)std::lround(b*255);}
    void pickerFromColor(unsigned c){float r=((c>>16)&255)/255.f,g=((c>>8)&255)/255.f,b=(c&255)/255.f,high=std::max({r,g,b}),low=std::min({r,g,b}),d=high-low;pickerValue=high;pickerSat=high?d/high:0;pickerHue=d==0?0:high==r?60*std::fmod((g-b)/d+6,6.f):high==g?60*((b-r)/d+2):60*((r-g)/d+4);}
    void openColorPicker(){if(!selectedColorEnabled())return;storeEdit();unsigned c=colorAt(selectedColor);pickerFromColor(c);colorValue=hexColor(c);changeTab(14);}
    void stylePointer(float x,float y){if(styleDrag==1){selectedWidth()=std::clamp(std::round((x-183)/218*12)/2,0.f,6.f);if(!appearanceDesktop)region();syncEdit();}else{if(styleDrag==2){pickerSat=std::clamp((x-34)/350,0.f,1.f);pickerValue=1-std::clamp((y-98)/178,0.f,1.f);}if(styleDrag==3)pickerHue=std::clamp((y-98)/178,0.f,1.f)*359.99f;colorValue=hexColor(hsv(pickerHue,pickerSat,pickerValue));if(edit&&editOwner==14)SetWindowTextW(edit,colorValue.c_str());}draw();}
    void colorsPage(){
        button({34,54,85,30},L"Назад",L"\uE72B",[this]{changeTab(8);});gfx->text(appearanceDesktop?L"Цвета стола":L"Цвета островка",{137,53,430,31},18,Ink,true);
        button({608,54,94,30},L"Обои",L"\uE8B9",[this]{if(appearanceDesktop){pinned=false;animate(false);desktop.appearance();}else changeTab(13);});
        const wchar_t* names[]={L"Фон",L"Карточки",L"Акцент",L"Текст",L"Вторичный текст",L"Наведение",L"Обводка"};
        for(int i=0;i<7;i++){bool enabled=i!=6||selectedWidth()>0;Box r{34,91.f+i*27,408,25};if(enabled&&(i==selectedColor||hoverIndex==(int)hits.size()))gfx->fill(r,i==selectedColor?Hover:Card,7);gfx->fill({45,r.y+4,17,17},colorAt(i),5);gfx->stroke({45,r.y+4,17,17},Muted,5,.5f);gfx->text(names[i],{75,r.y,207,25},12,enabled?Ink:mix(Card,Muted,.7f));gfx->text(hexColor(colorAt(i)),{302,r.y,123,25},11,Muted,false,DWRITE_TEXT_ALIGNMENT_TRAILING);if(enabled)addHit(r,[this,i](float){storeEdit();selectedColor=i;openColorPicker();});}
        gfx->fill({464,94,238,35},colorAt(selectedColor),9);gfx->text(hexColor(colorAt(selectedColor)),{464,94,238,35},12,contrast(colorAt(selectedColor)),true,DWRITE_TEXT_ALIGNMENT_CENTER);if(selectedColorEnabled())addHit({464,94,238,35},[this](float){openColorPicker();});
        gfx->fill({464,140,238,42},Card,8);ghost(colorValue,{476,140,206,42},false,10);button({464,193,238,33},L"Применить",L"\uE73E",[this]{storeEdit();applyColor();},false,selectedColorEnabled());
        const wchar_t* presets[]={L"Тёмная",L"Светлая",L"Графит"};for(int i=0;i<3;i++)button({464.f+i*81,238,76,30},presets[i],nullptr,[this,i]{unsigned values[3][6]={{0x09090b,0x252328,0xb8d8c9,0xeae9ed,0x8a8790,0x322f37},{0xf0eff3,0xffffff,0x598d72,0x24212b,0x77717f,0xe0dee6},{0x151a21,0x242c36,0x9db9db,0xe9eef5,0x8c9caf,0x344254}};for(int j=0;j<6;j++)colorAt(j)=values[i][j];colorValue=hexColor(colorAt(selectedColor));if(edit&&editOwner==10)SetWindowTextW(edit,colorValue.c_str());applyTheme();save();draw();});
        gfx->text(L"Обводка",{34,286,140,24},12,Ink);gfx->text(decimal(selectedWidth())+L" px",{307,286,94,24},11,Muted,false,DWRITE_TEXT_ALIGNMENT_TRAILING);gfx->fill({183,316,218,3},Hover,1);gfx->fill({183,316,218*selectedWidth()/6,3},Accent,1);gfx->fill({178+218*selectedWidth()/6,312,10,11},Ink,5);addHit({177,306,230,21},[this](float f){styleDrag=1;selectedWidth()=std::clamp(std::round(f*12)/2,0.f,6.f);syncEdit();SetCapture(hwnd);draw();});
        Box check{464,282,238,35};bool outlined=selectedWidth()>0;gfx->stroke({469,291,16,16},outlined&&selectedSmooth()?Accent:Muted,4,1);if(selectedSmooth())gfx->icon(L"\uE73E",{469,291,16,16},11,outlined?Accent:Muted);gfx->text(L"Сглаживание",{495,282,207,35},12,outlined?Ink:Muted);if(outlined)addHit(check,[this](float){selectedSmooth()=!selectedSmooth();save();draw();});
    }
    void colorPickerPage(){
        button({34,54,85,30},L"Назад",L"\uE72B",[this]{colorValue=hexColor(colorAt(selectedColor));changeTab(10);});gfx->text(L"Цвет",{137,53,430,31},18,Ink,true);
        gfx->gradient({34,98,350,178},0xffffff,hsv(pickerHue,1,1),false);gfx->gradient({34,98,350,178},0,0,true,0,1);
        for(int i=0;i<178;i++)gfx->fill({400,98.f+i,22,1.1f},hsv(i/177.f*359.99f,1,1));
        float px=34+350*pickerSat,py=98+178*(1-pickerValue);gfx->stroke({px-5,py-5,10,10},0xffffff,5,1.5f);gfx->stroke({px-6.5f,py-6.5f,13,13},0,6.5f,1);gfx->line(397,98+178*pickerHue/360,425,98+178*pickerHue/360,Ink,2);
        addHit({34,98,350,178},[this](float){styleDrag=2;POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);auto q=uiPoint(p.x/scale,p.y/scale);stylePointer(q.x,q.y);SetCapture(hwnd);});addHit({396,98,30,178},[this](float){styleDrag=3;POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);auto q=uiPoint(p.x/scale,p.y/scale);stylePointer(q.x,q.y);SetCapture(hwnd);});
        unsigned c=0;parseColor(colorValue,c);gfx->fill({464,98,238,49},c,9);gfx->text(colorValue,{464,98,238,49},13,contrast(c),true,DWRITE_TEXT_ALIGNMENT_CENTER);gfx->fill({464,159,238,42},Card,8);ghost(colorValue,{476,159,206,42},false,14);button({464,212,238,33},L"Применить",L"\uE73E",[this]{storeEdit();applyColor();});button({464,257,238,33},L"Отмена",nullptr,[this]{colorValue=hexColor(colorAt(selectedColor));changeTab(10);});
    }
    int searchRows()const{return searchState->indexError?3:4;}
    void openSearch(){searchFocus=true;pinned=true;animate(true);changeTab(12);syncEdit();}
    void searchChanged(){
        auto value=editText(edit);if(value==searchValue)return;searchValue=value;searchSelected=searchOffset=0;searchClickAt=0;searchClickIndex=~size_t(0);
        search.request(searchValue);searchState=search.snapshot();draw();
    }
    void moveSearch(int delta){
        int count=(int)searchState->results.size();if(!count)return;
        searchSelected=std::clamp(searchSelected+delta,0,count-1);
        if(searchSelected<searchOffset)searchOffset=searchSelected;
        if(searchSelected>=searchOffset+searchRows())searchOffset=searchSelected-searchRows()+1;
        draw();
    }
    void performSearchAction(const SearchResult& item,int action){
        bool ok=action==3?searchLocation(item):searchOpen(hwnd,item,action==2);
        if(ok){pinned=false;animate(false);}
        else if(GetLastError()!=ERROR_CANCELLED)showToast(action==3?L"Не удалось открыть расположение":L"Не удалось открыть результат");
    }
    void openSearchResult(int index,bool administrator=false){
        if(index<0||index>=(int)searchState->results.size())return;
        auto item=searchState->results[index];if(administrator&&!item.administrator)return;performSearchAction(item,administrator?2:1);
    }
    void searchMenu(size_t index){
        if(index>=searchState->results.size())return;
        auto item=searchState->results[index];std::vector<MenuItem> items={{L"Открыть",L"\uE8A5",1}};
        if(item.administrator)items.push_back({L"От имени администратора",L"\uE7EF",2});
        if(!item.path.starts_with(L"shell:"))items.push_back({L"Расположение",L"\uE8B7",3});
        POINT p;GetCursorPos(&p);dialog=true;int result=customMenu(hwnd,p,items,menuColors());dialog=false;
        if(result)performSearchAction(item,result);
    }
    void searchPage(){
        searchSlots.clear();gfx->text(L"Поиск",{36,54,430,28},18,Ink,true);
        if(searchState->busy)gfx->text(L"Поиск…",{579,54,123,28},10.5f,Muted,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
        gfx->fill({34,86,668,42},Card,9);ghost(searchValue,{48,86,640,42},false,12);
        auto& results=searchState->results;int count=(int)results.size(),visible=searchRows();
        if(!count){
            if(!searchValue.empty()){gfx->icon(L"\uE721",{300,161,136,50},28,Muted);gfx->text(searchState->busy?L"Поиск…":searchState->indexError?L"Поиск файлов Windows недоступен":L"Ничего не найдено",{75,215,586,40},13,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER,true);}
            return;
        }
        searchSelected=std::clamp(searchSelected,0,count-1);searchOffset=std::clamp(searchOffset,0,std::max(0,count-visible));
        for(int row=0;row<visible&&searchOffset+row<count;row++){
            int index=searchOffset+row;auto& item=results[index];Box box{34,140.f+row*44,412,41};bool selected=index==searchSelected;
            gfx->fill(box,selected||hoverIndex==(int)hits.size()?Hover:Card,8);
            if(item.icon)gfx->pixels(item.icon,{44,box.y+8,25,25});else gfx->icon(item.directory?L"\uE8B7":item.application?L"\uE71D":L"\uE8A5",{42,box.y+6,29,29},18,Accent);
            gfx->text(item.name,{80,box.y+3,354,20},12,Ink,selected);
            gfx->text(item.path.starts_with(L"shell:")?L"Приложение Windows":item.path,{80,box.y+23,354,14},9,Muted);
            searchSlots.push_back({box,(size_t)index});addHit(box,[this,index](float){auto now=GetTickCount64();bool twice=searchClickIndex==(size_t)index&&now-searchClickAt<=GetDoubleClickTime();searchSelected=index;searchClickIndex=index;searchClickAt=now;if(twice)openSearchResult(index);else draw();});
        }
        if(count>visible){float height=visible*44.f-3,thumb=std::max(18.f,height*visible/count);gfx->fill({450,140,2,height},Card,1);gfx->fill({450,140+(height-thumb)*searchOffset/(count-visible),2,thumb},Muted,1);}
        if(searchState->indexError)gfx->text(L"Поиск файлов Windows недоступен",{34,291,412,25},10,Muted);
        auto item=results[searchSelected];gfx->fill({458,140,244,173},Card,10);gfx->text(item.name,{470,149,220,29},12.5f,Ink,true);
        float y=185;button({470,y,220,32},L"Открыть",L"\uE8A5",[this,item]{performSearchAction(item,1);});y+=39;
        if(item.administrator){button({470,y,220,32},L"От имени администратора",L"\uE7EF",[this,item]{performSearchAction(item,2);});y+=39;}
        if(!item.path.starts_with(L"shell:"))button({470,y,220,32},L"Расположение",L"\uE8B7",[this,item]{performSearchAction(item,3);});
    }
    void commandPage() {
        gfx->text(L"Команда",{36,57,600,30},19,Ink,true);
        gfx->fill({34,96,668,50},Card,10);
        ghost(commandValue,{46,96,644,50},false,9);
        button({34,250,200,39},L"Открыть / найти",L"\uE721",[this]{storeEdit();executeCommand();});
        button({245,250,200,39},L"Выбрать файл",L"\uE8B7",[this]{dialog=true;auto path=chooseFile(hwnd,L"Выберите файл или приложение");dialog=false;if(!path.empty()){commandValue=path;SetWindowTextW(edit,path.c_str());}});
        button({456,250,246,39},L"Выполнить",L"\uE768",[this]{storeEdit();if(!launchCommand(hwnd,commandValue))showToast(L"Не удалось выполнить команду");});
    }
    void collapsedSide(){
        auto b=body();float x=dockEdge==3?0:SurfaceW-36,cy=SurfaceH/2;for(int i=0;i<3;i++){float h=now.playing?3+15*meter[i]:3;gfx->fill({x+11+i*5,cy-82-h/2,2,h},Accent,1);}
        auto stamp=GetTickCount64();std::wstring clock=timerEnd>stamp?duration((timerEnd-stamp)/1000.):timeString(false);auto colon=clock.find_last_of(L':');auto upper=clock.substr(0,colon),lower=colon==std::wstring::npos?L"":clock.substr(colon+1);gfx->text(upper,{x,cy-29,36,27},upper.size()>2?12:17,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);gfx->text(lower,{x,cy+2,36,27},17,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);
        HWND foreground=GetForegroundWindow();auto language=LOWORD((UINT_PTR)GetKeyboardLayout(GetWindowThreadProcessId(foreground,nullptr)));const wchar_t* code=PRIMARYLANGID(language)==LANG_RUSSIAN?L"RU":L"EN";gfx->text(toastUntil>stamp?L"•":code,{x,cy+73,36,24},9,toastUntil>stamp?Accent:Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        if(toastUntil>stamp&&b.w>130){float textX=dockEdge==3?48:b.x+12;gfx->text(toastTitle,{textX,cy-47,b.w-62,52},12,Ink,true,DWRITE_TEXT_ALIGNMENT_LEADING,true);gfx->text(toastDetail,{textX,cy+14,b.w-62,72},10,Muted,false,DWRITE_TEXT_ALIGNMENT_LEADING,true);}
    }
    void collapsed() {
        float x=center()-currentW/2;
        if(toastUntil>GetTickCount64()) {
            gfx->icon(L"\uE73E",{x+15,10,32,39},18,Accent);
            gfx->text(toastTitle,{x+55,8,currentW-72,23},12,Ink,true);
            if(!toastDetail.empty())gfx->text(toastDetail,{x+55,30,currentW-72,22},10,Muted);
            return;
        }
        if(now.available) {
            for(int i=0;i<3;i++){float h=now.playing?3+15*meter[i]:3;gfx->fill({x+19+i*5,18-h/2,2,h},Accent,1);}
        } else {gfx->fill({x+21,15,6,6},Accent,3);gfx->fill({x+31,15,6,6},Accent,3);}
        std::wstring center=timerEnd>GetTickCount64()?duration((timerEnd-GetTickCount64())/1000.):timeString(settings.seconds);
        gfx->text(center,{x+48,0,currentW-96,36},12,Ink,true,DWRITE_TEXT_ALIGNMENT_CENTER);
        SYSTEM_POWER_STATUS power={};GetSystemPowerStatus(&power);
        if(power.BatteryFlag!=128 && power.BatteryLifePercent<=100)gfx->text(std::to_wstring(power.BatteryLifePercent)+L"%",{x+currentW-53,0,40,36},10,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        else {
            HWND fg=GetForegroundWindow();HKL lang=GetKeyboardLayout(GetWindowThreadProcessId(fg,nullptr));
            auto code=LOWORD((UINT_PTR)lang);gfx->text(PRIMARYLANGID(code)==LANG_RUSSIAN?L"RU":L"EN",{x+currentW-50,0,34,36},10,Muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    void draw() {
        if(!gfx||!hwnd||manualHidden||renderPaused)return;
        SetPropW(hwnd,L"OwnIsland.DockLocked",(HANDLE)(UINT_PTR)dockLocked);SetPropW(hwnd,L"OwnIsland.MediaPlaying",(HANDLE)(UINT_PTR)now.playing);hits.clear();gfx->outlineColor=Outline;gfx->outlineWidth=BorderWidth;gfx->outlineSmooth=BorderSmooth;gfx->begin();auto wall=panelWallpaper.snapshot();if(!wall.enabled)gfx->releaseWallpaper();gfx->island(body(),dockEdge,Background,wall.enabled?wall.image:nullptr,wallOpacity/100.f,wallShade/100.f,wallBlur);
        gfx->clipIsland();auto origin=uiOrigin();gfx->setOrigin(origin.x,origin.y);
        float opened=(expanded||(morphing&&fromH>100))?std::clamp((currentH-36)/(PanelH-36),0.f,1.f):0;
        if(opened>.02f){
            float opacity=std::clamp((opened-.08f)/.72f,0.f,1.f);
            gfx->layer(opacity);header();gfx->endLayer();
            gfx->clip({22,46,692,282});
            float progress=tabStart>0?std::clamp((float)((moment()-tabStart)/.23),0.f,1.f):1;
            float eased=1-std::pow(1-progress,3.f);
            if(tabStart>0)gfx->previousFrame((1-eased)*opacity,-18*tabDirection*eased);
            gfx->layer(eased*opacity,22*tabDirection*(1-eased),-6*(1-opened));
            bool content=gfx->beginContent();
            switch(tab){case 0:home();break;case 1:clipboardPage();break;case 2:filesPage();break;case 3:textPage();break;case 4:chatPage();break;case 5:notesPage();break;case 6:weatherPage();break;case 7:controlPage();break;case 8:settingsPage();break;case 9:commandPage();break;case 10:colorsPage();break;case 11:appsPage();break;case 12:searchPage();break;case 13:wallpaperPage();break;case 14:colorPickerPage();break;case 15:performancePage();break;}
            if(content)gfx->endContent();
            gfx->endLayer();gfx->unclip();
        }
        if(opened<.25f){if(dockEdge==1||dockEdge==3){gfx->setOrigin(0,0);gfx->layer(1-opened/.25f);collapsedSide();gfx->endLayer();}else{gfx->layer(1-opened/.25f);collapsed();gfx->endLayer();}}
        gfx->setOrigin(0,0);gfx->unclipIsland();if(!gfx->end())PostMessageW(hwnd,WM_APP+20,0,0);else {++renderedFrames;SetPropW(hwnd,L"OwnIsland.RenderedFrames",(HANDLE)(UINT_PTR)renderedFrames);}
    }
    void requestWeather() {
        if(weatherBusy||cityValue.empty())return;
        weatherBusy=true;if(weatherWorker.joinable())weatherWorker.join();
        auto city=cityValue;weatherRequestedCity=city;
        weatherWorker=std::thread([this,city]{auto result=std::make_unique<WeatherState>(fetchWeather(city));if(!PostMessageW(hwnd,WM_APP+2,0,(LPARAM)result.get()))return;result.release();});
        draw();
    }
    void executeCommand() {
        if(commandValue.empty())return;
        auto value=commandValue;
        DWORD attrs=GetFileAttributesW(value.c_str());
        if(attrs!=INVALID_FILE_ATTRIBUTES)openTarget(value);
        else {
            static const wchar_t hex[]=L"0123456789ABCDEF";
            int n=WideCharToMultiByte(CP_UTF8,0,value.c_str(),(int)value.size(),nullptr,0,nullptr,nullptr);
            std::string bytes(n,'\0');WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),bytes.data(),n,nullptr,nullptr);
            std::wstring encoded;
            for(unsigned char c:bytes){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')encoded+=(wchar_t)c;else {encoded+=L'%';encoded+=hex[c>>4];encoded+=hex[c&15];}}
            openTarget(L"https://www.google.com/search?q="+encoded);
        }
    }
    void pickFile() {dialog=true;auto path=chooseFile(hwnd,L"Добавить на полку");dialog=false;if(!path.empty()&&std::find(files.begin(),files.end(),path)==files.end())files.insert(files.begin(),path);draw();}
    void addShortcut() {if(shortcuts.size()>=64){showToast(L"Достигнут лимит приложений");return;}dialog=true;auto path=chooseFile(hwnd,L"Добавить приложение");dialog=false;if(!path.empty()&&std::find(shortcuts.begin(),shortcuts.end(),path)==shortcuts.end()){shortcuts.push_back(path);appOffset=std::max(0,((int)shortcuts.size()+7)/8-3);save();}draw();}
    void startColorPicker() {
        dialog=true;ShowWindow(hwnd,SW_HIDE);DwmFlush();auto color=pickScreenColor();ShowWindow(hwnd,SW_SHOWNOACTIVATE);dialog=false;
        if(!color.empty()){setClipboard(hwnd,color);showToast(color,L"Цвет скопирован в буфер");}
    }
    void powerAction(int action) {
        const wchar_t* names[]={L"Блокировка",L"Сон",L"Перезагрузка",L"Выключение"};
        if(confirmAction!=names[action]||GetTickCount64()>confirmUntil){confirmAction=names[action];confirmUntil=GetTickCount64()+4000;draw();return;}
        confirmAction.clear();
        if(action==0)LockWorkStation();
        if(action==1)SetSuspendState(FALSE,FALSE,FALSE);
        if(action>=2){HANDLE token;TOKEN_PRIVILEGES tp={};if(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token)){tp.PrivilegeCount=1;LookupPrivilegeValueW(nullptr,SE_SHUTDOWN_NAME,&tp.Privileges[0].Luid);tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;AdjustTokenPrivileges(token,FALSE,&tp,0,nullptr,nullptr);CloseHandle(token);}ExitWindowsEx(action==2?EWX_REBOOT:EWX_POWEROFF,SHTDN_REASON_MAJOR_APPLICATION|SHTDN_REASON_FLAG_PLANNED);}
    }
    void tick() {
        desktop.maintain();updateFrames();
        refreshVisibility(shell.dismissInactiveOverflow());
        desktopWindowOpened();
        auto tick=GetTickCount64();
        if(timerEnd && tick>=timerEnd){timerEnd=0;showToast(L"Таймер завершён",L"Время вышло");MessageBeep(MB_OK);}
        if(toastUntil && tick>=toastUntil){toastUntil=0;if(!expanded)animate(false);}
        POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);
        bool inside=body().contains(p.x/scale,p.y/scale);
        if(!inside)hoverBlocked=false;
        bool typing=((edit&&IsWindowVisible(edit))||(timerEdit&&IsWindowVisible(timerEdit)))&&GetForegroundWindow()==hwnd;
        if(expanded&&!inside&&!pinned&&!dialog&&!dockPending&&!typing&&!shell.overflowVisible()){if(!outsideSince)outsideSince=tick;else if(tick-outsideSince>550)animate(false);}else outsideSince=0;
        int caps=(GetKeyState(VK_CAPITAL)&1);HWND fg=GetForegroundWindow();int language=LOWORD((UINT_PTR)GetKeyboardLayout(GetWindowThreadProcessId(fg,nullptr)));
        if(caps!=previousCaps){previousCaps=caps;showToast(caps?L"Caps Lock включён":L"Caps Lock выключен");}
        if(previousLanguage && language!=previousLanguage){showToast(PRIMARYLANGID(language)==LANG_RUSSIAN?L"Русская раскладка":L"Английская раскладка");}
        previousLanguage=language;
    }
    void trayMenu() {
        std::vector<MenuItem> items={{expanded?L"Свернуть":L"Открыть",L"\uE8A7",1},{L"Закрепить панель",L"\uE718",2,pinned},{L"Диспетчер задач",L"\uE9D9",4},{L"Скрыть все",L"\uE7F4",5},{L"Настройки",L"\uE713",3}};
        POINT p;GetCursorPos(&p);dialog=true;int result=customMenu(hwnd,p,items,menuColors());dialog=false;
        if(result==1){manualHidden=false;ShowWindow(hwnd,SW_SHOWNOACTIVATE);pinned=!expanded;animate(!expanded);}
        if(result==2){pinned=!pinned;if(pinned)animate(true);}
        if(result==3){pinned=true;animate(true);changeTab(8);}
        if(result==4)openTarget(L"taskmgr.exe");
        if(result==5)toggleDesktop();
        PostMessageW(hwnd,WM_NULL,0,0);
    }
    void shortcutMenu(size_t index){
        if(index>=shortcuts.size())return;
        POINT p;GetCursorPos(&p);dialog=true;
        int result=customMenu(hwnd,p,{{L"Открыть",L"\uE8A5",1},{L"От имени администратора",L"\uE7EF",4},{L"Показать в папке",L"\uE8B7",2},{L"Открепить",L"\uE77A",3}},menuColors());dialog=false;
        if(result==1)openPinnedTarget(shortcuts[index],taskState);
        if(result==2)openFileLocation(shortcuts[index]);
        if(result==3){shortcuts.erase(shortcuts.begin()+index);save();draw();}
        if(result==4&&!launchCommand(hwnd,shortcuts[index],true))showToast(L"Запуск отменён или недоступен");
    }
    void editMenu(HWND input){
        POINT p;GetCursorPos(&p);dialog=true;
        int result=customMenu(hwnd,p,{{L"Вырезать",L"\uE8C6",1},{L"Копировать",L"\uE8C8",2},{L"Вставить",L"\uE77F",3},{L"Выделить всё",L"\uE8B3",4}},menuColors());dialog=false;
        if(!IsWindow(input))return;SetForegroundWindow(hwnd);SetFocus(input);
        if(result==1)SendMessageW(input,WM_CUT,0,0);if(result==2)SendMessageW(input,WM_COPY,0,0);if(result==3)SendMessageW(input,WM_PASTE,0,0);if(result==4)SendMessageW(input,EM_SETSEL,0,-1);
    }
    LRESULT message(UINT msg,WPARAM wp,LPARAM lp) {
        if(msg==taskbarCreated){Shell_NotifyIconW(NIM_ADD,&tray);shell.maintain();return 0;}
        switch(msg) {
        case WM_CREATE:{
            SetPropW(hwnd,L"OwnIsland.Standalone",(HANDLE)(UINT_PTR)(StandaloneMode?1:0));
            load();position();if(!gfx)return -1;
            tray.cbSize=sizeof(tray);tray.hWnd=hwnd;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;tray.uCallbackMessage=TrayMessage;
            tray.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));if(!tray.hIcon)tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
            wcscpy_s(tray.szTip,L"Win");Shell_NotifyIconW(NIM_ADD,&tray);
            SetTimer(hwnd,1,250,nullptr);SetTimer(hwnd,3,1000,nullptr);SetTimer(hwnd,8,16,nullptr);
            WTSRegisterSessionNotification(hwnd,NOTIFY_FOR_THIS_SESSION);
            displayNotification=RegisterPowerSettingNotification(hwnd,&GUID_CONSOLE_DISPLAY_STATE,DEVICE_NOTIFY_WINDOW_HANDLE);
            RegisterHotKey(hwnd,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'I');
            DragAcceptFiles(hwnd,TRUE);volume=getVolume();muted=getMuted();micState=getMicState();previousCaps=GetKeyState(VK_CAPITAL)&1;
            clipboardOn=AddClipboardFormatListener(hwnd)!=FALSE;PostMessageW(hwnd,WM_CLIPBOARDUPDATE,0,0);
            network.start();frames.start(hwnd);audio.start();media.start(hwnd);tasks.setPinned(shortcuts);tasks.start(hwnd);
            search.setSources(shortcuts);search.start(hwnd);
            taskHookTarget=hwnd;foregroundHook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,foregroundEvent,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
            if(!shell.start(hwnd))showToast(L"Не удалось включить замену панели Windows");else position();
            startDesktop();
            if(!cityValue.empty())requestWeather();lastFrame=moment();return 0;
        }
        case WM_PAINT:{PAINTSTRUCT ps;BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);draw();return 0;}
        case WM_ERASEBKGND:return 1;
        case WM_SETCURSOR:if((HWND)wp==hwnd&&LOWORD(lp)==HTCLIENT){HCURSOR cursor=pointerCursor();if(GetCursor()!=cursor)SetCursor(cursor);return TRUE;}break;
        case WM_MOUSEACTIVATE:{POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);auto child=ChildWindowFromPoint(hwnd,p);if((edit&&IsWindowVisible(edit)&&child==edit)||(timerEdit&&IsWindowVisible(timerEdit)&&child==timerEdit))return MA_ACTIVATE;return MA_NOACTIVATE;}
        case WM_MOUSEMOVE:{
            if(dockPending){POINT p;GetCursorPos(&p);if(dockMoving||std::abs(p.x-dockDown.x)>5||std::abs(p.y-dockDown.y)>5){dockMoving=true;moveDock(p);}return 0;}
            if(renderPaused)return 0;
            POINT currentPointer;GetCursorPos(&currentPointer);if(!expanded&&settings.hover&&!hoverBlocked&&!gripAt(currentPointer))animate(true);
            auto local=uiPoint(GET_X_LPARAM(lp)/scale,GET_Y_LPARAM(lp)/scale);float x=local.x,y=local.y;
            if(styleDrag){stylePointer(x,y);return 0;}
            if(draggingSeek){seekRatio=std::clamp((x-198)/194,0.f,1.f);draw();return 0;}
            if(wallDrag>=0){wallValue(wallDrag,std::clamp((x-48)/640,0.f,1.f));draw();return 0;}
            if(draggingVolume){volume=std::clamp((x-83)/257,0.f,1.f);setVolume(volume);draw();return 0;}
            int hovered=-1;for(size_t i=0;i<hits.size();i++)if(hits[i].box.contains(x,y)){hovered=(int)i;break;}
            bool overMusic=expanded&&tab==0&&Box{34,51,421,151}.contains(x,y);if(hovered!=hoverIndex||musicOver!=overMusic){hoverIndex=hovered;musicOver=overMusic;draw();}if(!tracking){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);tracking=true;}
            return 0;
        }
        case WM_MOUSELEAVE:tracking=false;musicOver=false;hoverIndex=-1;draw();return 0;
        case WM_LBUTTONDOWN:{
            POINT grip;GetCursorPos(&grip);if(gripAt(grip)){RECT native{};GetWindowRect(hwnd,&native);auto b=body();dockDragEdge=dockEdge;dockGrabOffset=(dockEdge==1||dockEdge==3)?grip.y-(native.top+(b.y+b.h/2)*scale):grip.x-(native.left+(b.x+b.w/2)*scale);dockDown=grip;dockPending=true;dockMoving=false;SetCapture(hwnd);return 0;}
            if(!expanded){pinned=true;animate(true);return 0;}
            if(morphing)return 0;
            auto local=uiPoint(GET_X_LPARAM(lp)/scale,GET_Y_LPARAM(lp)/scale);float x=local.x,y=local.y;
            if(shell.overflowVisible()&&!Box{555,12,27,27}.contains(x,y)){shell.closeOverflow();draw();}
            for(size_t i=0;i<hits.size();i++)if(hits[i].box.contains(x,y)){if(tabStart>0&&!hits[i].header)break;auto action=hits[i].action;auto box=hits[i].box;health::Hold protection;if(protection)action(std::clamp((x-box.x)/box.w,0.f,1.f));break;}
            return 0;
        }
        case WM_LBUTTONUP:if(dockPending){bool moved=dockMoving;dockPending=dockMoving=false;ReleaseCapture();if(moved){dockRatio=snapDockRatio(dockRatio);saveDock();position();}else{pinned=true;animate(true);}return 0;}if(styleDrag){styleDrag=0;ReleaseCapture();save();region();draw();}if(draggingSeek){draggingSeek=false;ReleaseCapture();if(now.duration>0)media.seek(seekRatio*now.duration);draw();}if(wallDrag>=0){wallDrag=-1;ReleaseCapture();saveWallpaper();}if(draggingVolume){draggingVolume=false;ReleaseCapture();}return 0;
        case WM_CAPTURECHANGED:if(dockPending){dockPending=dockMoving=false;saveDock();syncEdit();}styleDrag=0;draggingSeek=false;wallDrag=-1;draggingVolume=false;return 0;
        case WM_MOUSEWHEEL:{
            POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&point);auto origin=uiOrigin();point.x-=(LONG)std::lround(origin.x*scale);point.y-=(LONG)std::lround(origin.y*scale);
            if(expanded&&tab==1)clipOffset=std::clamp(clipOffset-GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA,0,std::max(0,(int)clips.size()-4));
            else if(expanded&&tab==2)fileOffset=std::clamp(fileOffset-GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA,0,std::max(0,(int)files.size()-4));
            else if(expanded&&tab==11)appOffset=std::clamp(appOffset-GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA,0,std::max(0,((int)shortcuts.size()+7)/8-3));
            else if(expanded&&tab==12){searchOffset=std::clamp(searchOffset-GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA,0,std::max(0,(int)searchState->results.size()-searchRows()));searchSelected=std::clamp(searchSelected,searchOffset,std::max(searchOffset,std::min((int)searchState->results.size()-1,searchOffset+searchRows()-1)));}
            else if(expanded&&tab==0&&Box{34,260,668,57}.contains(point.x/scale,point.y/scale))taskOffset=std::clamp(taskOffset-GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA,0,std::max(0,(int)taskState->groups.size()-10));
            else {volume=std::clamp(getVolume()+GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*.025f,0.f,1.f);setVolume(volume);}
            draw();return 0;}
        case WM_RBUTTONUP:{
            shell.closeOverflow();
            auto local=uiPoint(GET_X_LPARAM(lp)/scale,GET_Y_LPARAM(lp)/scale);float x=local.x,y=local.y;
            if(expanded&&tab==12&&!morphing&&!tabStart){for(auto slot:searchSlots)if(slot.box.contains(x,y)){searchSelected=(int)slot.index;searchMenu(slot.index);return 0;}}
            if(expanded&&tab==11&&!morphing&&!tabStart){for(auto slot:shortcutSlots)if(slot.box.contains(x,y)){shortcutMenu(slot.index);return 0;}}
            if(expanded&&tab==0&&!morphing&&!tabStart){for(auto slot:taskSlots)if(slot.box.contains(x,y)){taskMenu(slot.group);return 0;}}
            trayMenu();return 0;
        }
        case WM_CONTEXTMENU:trayMenu();return 0;
        case WM_HOTKEY:if(wp==1)invokeIsland();return 0;
        case WM_KEYDOWN:if(wp==VK_ESCAPE){pinned=false;animate(false);}return 0;
        case ComponentActionMessage:{std::unique_ptr<ComponentAction> action((ComponentAction*)lp);if(action)desktopAction(action->code,action->value);return 0;}
        case WM_TIMER:
            if(wp==8){desktop.audioFrame();return 0;}
            if(wp==5){KillTimer(hwnd,5);synchronizeDesktopLanguage(languageRoot,languageBefore);draw();return 0;}
            if(wp==6){KillTimer(hwnd,6);overlay.reset();desktop.displayChanged();desktop.maintain();fileBrowserDisplayChanged();position();tasks.refresh();audio.restart();refreshVisibility(true);return 0;}
            if(wp==1)tick();
            if(wp==3){shell.maintain();volume=getVolume();muted=getMuted();micState=getMicState();if(!clipboardOn){clipboardOn=AddClipboardFormatListener(hwnd)!=FALSE;if(clipboardOn)PostMessageW(hwnd,WM_CLIPBOARDUPDATE,0,0);}if(expanded&&tab==5)storeEdit();draw();}
            return 0;
        case WM_APP+1:now=media.snapshot();updateFrames();draw();return 0;
        case WM_APP+3:frame();return 0;
        case WM_APP+4:taskState=tasks.snapshot();SetPropW(hwnd,L"OwnIsland.TaskGroups",(HANDLE)(UINT_PTR)taskState->groups.size());desktopWindowOpened();if(expanded&&(tab==0||tab==11))draw();return 0;
        case WM_APP+5:{HWND fg=GetForegroundWindow();if(!shell.contextInteraction()&&isTaskWindow(fg))shell.closeOverflow();if(desktopShown&&GetTickCount64()>=desktopSettleUntil&&isTaskWindow(fg)&&!IsIconic(fg))forgetDesktop();refreshVisibility(!shell.contextInteraction());tasks.refresh();return 0;}
        case WM_APP+6:invokeIsland();return 0;
        case WM_APP+7:shell.maintain();return 0;
        case WM_APP+8:shell.updateOverflow();draw();return 0;
        case WM_APP+12:{auto state=search.snapshot();if(state->query!=searchValue)return 0;std::wstring selected;if(searchSelected>=0&&searchSelected<(int)searchState->results.size())selected=searchState->results[searchSelected].path;searchState=state;if(!selected.empty())for(size_t i=0;i<state->results.size();i++)if(state->results[i].path==selected){searchSelected=(int)i;break;}if(tab==12)draw();return 0;}
        case WM_APP+13:desktop.maintain();updateFrames();return 0;
        case WM_APP+2:{std::unique_ptr<WeatherState> result((WeatherState*)lp);weatherBusy=false;if(result->available){weather=std::move(*result);cityValue=weatherRequestedCity;weatherEditing=false;WritePrivateProfileStringW(L"weather",L"city",cityValue.c_str(),ini.c_str());saveForecast();}else if(weather.available)weather.error=result->error;else weather=std::move(*result);syncEdit();draw();return 0;}
        case WM_APP+32:if(wp==0)toggleDesktop();if(wp==1){pinned=true;animate(true);changeTab(9);SetForegroundWindow(hwnd);}if(wp==2)openFileBrowser();if(wp==3)openSearch();return 0;
        case WM_APP+31:languageRoot=(HWND)wp;languageBefore=(HKL)lp;SetTimer(hwnd,5,80,nullptr);return 0;
        case WM_APP+20:gfx.reset();position();return 0;
        case WM_DROPFILES:{bool apps=expanded&&tab==11;HDROP drop=(HDROP)wp;UINT count=DragQueryFileW(drop,0xffffffff,nullptr,0);for(UINT i=0;i<count;i++){UINT n=DragQueryFileW(drop,i,nullptr,0);std::wstring path(n+1,L'\0');DragQueryFileW(drop,i,path.data(),n+1);path.resize(n);auto& list=apps?shortcuts:files;if(std::find(list.begin(),list.end(),path)==list.end()){if(apps){if(list.size()<64)list.push_back(path);}else list.insert(list.begin(),path);}}DragFinish(drop);if(apps)save();pinned=true;animate(true);changeTab(apps?11:2);draw();return 0;}
        case WM_CLIPBOARDUPDATE:{auto s=clipboardText(hwnd);if(!s.empty()){auto p=std::find(clips.begin(),clips.end(),s);if(p!=clips.end())clips.erase(p);clips.insert(clips.begin(),s);if(clips.size()>30)clips.pop_back();if(tab==1)draw();}return 0;}
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:{unsigned editInk=IsWindowEnabled((HWND)lp)?Ink:Muted;SetTextColor((HDC)wp,RGB((editInk>>16)&255,(editInk>>8)&255,editInk&255));SetBkColor((HDC)wp,RGB((Card>>16)&255,(Card>>8)&255,Card&255));return (LRESULT)editBrush;}
        case WM_COMMAND:
            if(LOWORD(wp)==100&&HIWORD(wp)==EN_CHANGE&&(editOwner==10||editOwner==14)){colorValue=editText(edit);if(!styleDrag){unsigned c;if(editOwner==14&&parseColor(colorValue,c))pickerFromColor(c);draw();}}
            if(LOWORD(wp)==100&&HIWORD(wp)==EN_CHANGE&&editOwner==12)searchChanged();
            if(LOWORD(wp)==100&&HIWORD(wp)==EN_CHANGE&&editOwner==5)SetTimer(hwnd,4,600,nullptr);
            if(LOWORD(wp)==101&&HIWORD(wp)==EN_KILLFOCUS){storeTimer();save();}
            return 0;
        case WM_DISPLAYCHANGE:case WM_DPICHANGED:SetTimer(hwnd,6,300,nullptr);return 0;
        case WM_WTSSESSION_CHANGE:shell.closeOverflow();if(wp==WTS_SESSION_LOCK||wp==WTS_CONSOLE_DISCONNECT||wp==WTS_REMOTE_DISCONNECT)sessionActive=false;else if(wp==WTS_SESSION_UNLOCK||wp==WTS_CONSOLE_CONNECT||wp==WTS_REMOTE_CONNECT)sessionActive=true;refreshVisibility(true);desktop.maintain();updateFrames();return 0;
        case WM_POWERBROADCAST:
            if(wp==PBT_APMSUSPEND){systemSuspended=true;shell.closeOverflow();refreshVisibility();desktop.maintain();updateFrames();return TRUE;}
            if(wp==PBT_APMRESUMEAUTOMATIC||wp==PBT_APMRESUMESUSPEND){systemSuspended=false;audio.restart();tasks.refresh();refreshVisibility(true);desktop.maintain();updateFrames();SetTimer(hwnd,6,300,nullptr);return TRUE;}
            if(wp==PBT_POWERSETTINGCHANGE){auto change=(POWERBROADCAST_SETTING*)lp;if(change&&IsEqualGUID(change->PowerSetting,GUID_CONSOLE_DISPLAY_STATE)&&change->DataLength==sizeof(DWORD)){DWORD state;memcpy(&state,change->Data,sizeof(state));displayActive=state!=0;refreshVisibility(true);desktop.maintain();updateFrames();}}return TRUE;
        case WM_SETTINGCHANGE:SetTimer(hwnd,6,300,nullptr);draw();return 0;
        case TrayMessage:if(lp==WM_RBUTTONUP)trayMenu();if(lp==WM_LBUTTONUP){pinned=!expanded;animate(!expanded);}return 0;
        case ExternalCommand:if(wp==0){pinned=false;animate(false);}if(wp==1){pinned=true;manualHidden=false;ShowWindow(hwnd,SW_SHOWNOACTIVATE);animate(true);}if(wp==2){pinned=true;animate(true);changeTab((int)lp);}if(wp==3)toggleDesktop();return 0;
        case WM_COPYDATA:{auto cd=(COPYDATASTRUCT*)lp;
            std::unique_ptr<ComponentAction> action;if(receiveComponentAction((HWND)wp,cd,action)){if(action->code==DeskSaveNote){desktopAction(action->code,action->value);return TRUE;}auto raw=action.release();if(!PostMessageW(hwnd,ComponentActionMessage,0,(LPARAM)raw)){delete raw;return FALSE;}return TRUE;}
            if(cd&&cd->lpData&&(cd->dwData==0x4953464c||cd->dwData==0x4953524e)&&cd->cbData>=sizeof(wchar_t)&&cd->cbData<=65536&&cd->cbData%sizeof(wchar_t)==0){std::wstring s((const wchar_t*)cd->lpData,cd->cbData/sizeof(wchar_t));if(!s.empty()&&s.back()==L'\0')s.pop_back();return cd->dwData==0x4953464c?openFileBrowser(s):launchCommand(hwnd,s);}
            if(cd&&cd->dwData==0x49534c44&&cd->cbData>=sizeof(wchar_t)&&cd->cbData<=8192&&cd->cbData%sizeof(wchar_t)==0){std::wstring s((const wchar_t*)cd->lpData,cd->cbData/sizeof(wchar_t));if(!s.empty()&&s.back()==L'\0')s.pop_back();showToast(L"Уведомление",s);return TRUE;}return FALSE;}
        case WM_QUERYENDSESSION:
            if(fileOperationsActive()){ShutdownBlockReasonCreate(hwnd,L"Выполняется операция с файлами");return FALSE;}
            ShutdownBlockReasonDestroy(hwnd);storeEdit();health::stopping(true);return TRUE;
        case WM_ENDSESSION:
            if(wp){health::stopping(true);WritePrivateProfileStringW(L"shell",L"shutdown",L"1",(stateDirectory()+L"\\shell-restore.ini").c_str());storeEdit();}
            else{ShutdownBlockReasonDestroy(hwnd);health::stopping(false);}return 0;
        case WM_SYSCOMMAND:if((wp&0xfff0)==SC_CLOSE)return 0;break;
        case WM_CLOSE:if(fileOperationsActive()){showToast(L"Выполняется операция с файлами");return 0;}WritePrivateProfileStringW(L"shell",L"shutdown",L"1",(stateDirectory()+L"\\shell-restore.ini").c_str());storeEdit();DestroyWindow(hwnd);return 0;
        case WM_DESTROY:closing=true;network.stop();desktop.stop();if(displayNotification){UnregisterPowerSettingNotification(displayNotification);displayNotification=nullptr;}WTSUnRegisterSessionNotification(hwnd);shell.stop();search.stop();if(foregroundHook){UnhookWinEvent(foregroundHook);foregroundHook=nullptr;}taskHookTarget=nullptr;tasks.stop();frames.update(false);audio.enable(false);for(int i=1;i<=8;i++)KillTimer(hwnd,i);UnregisterHotKey(hwnd,1);if(clipboardOn)RemoveClipboardFormatListener(hwnd);Shell_NotifyIconW(NIM_DELETE,&tray);PostQuitMessage(0);return 0;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};
LRESULT CALLBACK wndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    Island* a=(Island*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(msg==WM_NCCREATE){a=(Island*)((CREATESTRUCTW*)lp)->lpCreateParams;a->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)a);}
    if(a) {
        if(msg==WM_TIMER&&wp==4){KillTimer(hwnd,4);a->storeEdit();return 0;}
        return a->message(msg,wp,lp);
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}


}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    OleInitialize(nullptr);
    int argc;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::vector<std::wstring> arguments;bool explicitMode=false;
    for(int i=1;i<argc;i++){
        std::wstring a=argv[i];
        if(a==L"--standalone"||a==L"--integrated"){StandaloneMode=a==L"--standalone";explicitMode=true;}
        else if(a==L"--state-dir"&&i+1<argc)StateDirectoryOverride=argv[++i];
        else arguments.push_back(a);
    }
    if(!explicitMode)StandaloneMode=GetPrivateProfileIntW(L"shell",L"standalone",0,(stateDirectory()+L"\\settings.ini").c_str())!=0;
    std::wstring arg=arguments.empty()?L"":arguments[0];
    auto value=arguments.size()>1?arguments[1]:L"";
    wchar_t modulePath[32768]={};GetModuleFileNameW(nullptr,modulePath,32768);auto moduleName=std::filesystem::path(modulePath).filename().wstring();
    if(!_wcsicmp(moduleName.c_str(),L"Island.Files.exe")&&arg!=L"--files-host"){
        int result=(arg.empty()||arg==L"--files"||arg.front()!=L'-')?runFilesComponent(arg==L"--files"?value:arg):2;LocalFree(argv);OleUninitialize();CoUninitialize();return result;
    }
    if(!_wcsicmp(moduleName.c_str(),L"Island.Desktop.exe")&&arg!=L"--desktop-host"){LocalFree(argv);OleUninitialize();CoUninitialize();return 2;}
    if(arg==L"--files"){bool ok=routeFileBrowser(value,false,false);LocalFree(argv);OleUninitialize();CoUninitialize();return ok?0:2;}
    if(arg==L"--desktop-host"){int result=runDesktopComponent(value);LocalFree(argv);OleUninitialize();CoUninitialize();return result;}
    if(arg==L"--files-host"){bool select=std::find(arguments.begin(),arguments.end(),L"--select")!=arguments.end(),fresh=std::find(arguments.begin(),arguments.end(),L"--new-window")!=arguments.end();int result=runFilesComponent(value,select,fresh);LocalFree(argv);OleUninitialize();CoUninitialize();return result;}
    if(arg==L"--shell-watch"&&arguments.size()>1){DWORD parent=wcstoul(value.c_str(),nullptr,10);LocalFree(argv);int code=shellWatch(parent);OleUninitialize();CoUninitialize();return code;}

    struct InstanceLock {HANDLE handle=nullptr;bool owned=false;~InstanceLock(){if(owned)ReleaseMutex(handle);if(handle)CloseHandle(handle);}} instanceLock;
    {
        wchar_t desktopName[256]={};DWORD needed=0;GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktopName,sizeof(desktopName),&needed);uint64_t hash=1469598103934665603ull;for(auto c:std::wstring(desktopName)){hash^=(uint16_t)c;hash*=1099511628211ull;}auto mutexName=L"Local\\OwnIsland.Instance."+std::to_wstring(hash);instanceLock.handle=CreateMutexW(nullptr,FALSE,mutexName.c_str());if(!instanceLock.handle){LocalFree(argv);OleUninitialize();CoUninitialize();return 4;}DWORD lock=WaitForSingleObject(instanceLock.handle,0);instanceLock.owned=lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED;
        if(!instanceLock.owned){auto deadline=GetTickCount64()+12000;while(!FindWindowW(WindowClass,nullptr)&&GetTickCount64()<deadline){Sleep(25);lock=WaitForSingleObject(instanceLock.handle,0);if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){instanceLock.owned=true;break;}}if(!instanceLock.owned&&!FindWindowW(WindowClass,nullptr)){LocalFree(argv);OleUninitialize();CoUninitialize();return 4;}}
    }
    HWND existing=FindWindowW(WindowClass,nullptr);

    if(existing) {
        if(arg==L"--quit")PostMessageW(existing,WM_CLOSE,0,0);
        else if(arg==L"--collapse")PostMessageW(existing,ExternalCommand,0,0);
        else if(arg==L"--desktop")PostMessageW(existing,ExternalCommand,3,0);
        else if(arg==L"--tab"&&arguments.size()>1)PostMessageW(existing,ExternalCommand,2,std::clamp(_wtoi(value.c_str()),0,15));
        else if(arg==L"--files"||arg==L"--run"){std::wstring s=value;COPYDATASTRUCT cd={arg==L"--files"?0x4953464cul:0x4953524eul,(DWORD)((s.size()+1)*sizeof(wchar_t)),s.data()};DWORD_PTR result;SendMessageTimeoutW(existing,WM_COPYDATA,0,(LPARAM)&cd,SMTO_ABORTIFHUNG,1500,&result);}
        else if(arg==L"--notify"&&arguments.size()>1){std::wstring s=value.c_str();COPYDATASTRUCT cd={0x49534c44,(DWORD)((s.size()+1)*sizeof(wchar_t)),s.data()};DWORD_PTR result;SendMessageTimeoutW(existing,WM_COPYDATA,0,(LPARAM)&cd,SMTO_ABORTIFHUNG,1000,&result);}
        else PostMessageW(existing,ExternalCommand,1,0);
        LocalFree(argv);OleUninitialize();CoUninitialize();return 0;
    }
    if(arg==L"--quit"||arg==L"--collapse"||arg==L"--notify"){LocalFree(argv);OleUninitialize();CoUninitialize();return 0;}
    if(explicitMode)WritePrivateProfileStringW(L"shell",L"standalone",StandaloneMode?L"1":L"0",(stateDirectory()+L"\\settings.ini").c_str());
    if(!startShellProtection()){LocalFree(argv);OleUninitialize();CoUninitialize();return 3;}
    Island app;
    WNDCLASSEXW cls={sizeof(cls)};cls.lpfnWndProc=wndProc;cls.hInstance=instance;cls.lpszClassName=WindowClass;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));RegisterClassExW(&cls);
    HWND hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOREDIRECTIONBITMAP,WindowClass,L"Остров",WS_POPUP|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,1,1,nullptr,nullptr,instance,&app);
    if(!hwnd){LocalFree(argv);OleUninitialize();CoUninitialize();return 2;}
    app.refreshStartupLauncher();
    shellProtectionReady();
    SetPropW(hwnd,L"OwnIsland.GuardedBeforeUI",(HANDLE)1);
    SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    app.desktop.maintain();
    health::ready(hwnd);SetPropW(hwnd,L"OwnIsland.StartupReady",(HANDLE)1);auto startEventName=L"Local\\OwnIsland.StartReady."+std::to_wstring(GetCurrentProcessId());if(HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,startEventName.c_str())){SetEvent(ready);CloseHandle(ready);}
    if(arg==L"--open"||arg==L"--tab"){app.pinned=true;app.animate(true);if(arg==L"--tab"&&arguments.size()>1)app.changeTab(std::clamp(_wtoi(value.c_str()),0,15));}
    if(arg==L"--files")openFileBrowser(value);if(arg==L"--run")launchCommand(hwnd,value);
    LocalFree(argv);
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    app.media.stop();
    stopShellProtection();
    OleUninitialize();CoUninitialize();return 0;
}



