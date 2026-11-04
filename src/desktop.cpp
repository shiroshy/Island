// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "file_browser.h"
#include "desktop.h"
#include "health.h"
#include "runtime.h"
#include "native_desktop.h"
#include "menu.h"
#include "search.h"
#include "wallpaper.h"
#include "widget_image.h"
#include "widget_geometry.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <propkey.h>
#include <dwmapi.h>
#include <filesystem>
#include <cmath>
#include <set>
#include <chrono>
#include <fstream>
#include <numeric>

namespace {
constexpr wchar_t DeskClass[]=L"OwnIsland.Desktop";
constexpr wchar_t BackdropClass[]=L"OwnIsland.DesktopBackdrop";
constexpr wchar_t HostClass[]=L"OwnIsland.DesktopHost";
LRESULT CALLBACK independentHostProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(msg==WM_CLOSE)return 0;
    if(msg==WM_ERASEBKGND){RECT r{};GetClientRect(hwnd,&r);FillRect((HDC)wp,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));return 1;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
HWND createIndependentHost(){
    WNDCLASSEXW c{sizeof(c)};c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=HostClass;c.lpfnWndProc=independentHostProc;c.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassExW(&c);
    return CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,HostClass,L"",WS_POPUP|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),nullptr,nullptr,c.hInstance,nullptr);
}
LRESULT CALLBACK backdropProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_NCCREATE)SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    if(msg==WM_PAINT||msg==WM_ERASEBKGND){PAINTSTRUCT ps{};HDC dc=msg==WM_PAINT?BeginPaint(hwnd,&ps):(HDC)wp;RECT r{};GetClientRect(hwnd,&r);HBRUSH brush=CreateSolidBrush((COLORREF)GetWindowLongPtrW(hwnd,GWLP_USERDATA));FillRect(dc,&r,brush);DeleteObject(brush);if(msg==WM_PAINT)EndPaint(hwnd,&ps);return 1;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
constexpr float Pi=3.14159265359f;
unsigned blend(unsigned a,unsigned b,float t){unsigned c=0;for(int s:{0,8,16})c|=(unsigned)std::lround(((a>>s)&255)*(1-t)+((b>>s)&255)*t)<<s;return c;}
std::wstring knownFolder(REFKNOWNFOLDERID id){PWSTR p=nullptr;std::wstring result;if(SUCCEEDED(SHGetKnownFolderPath(id,0,nullptr,&p))){result=p;CoTaskMemFree(p);}return result;}
std::wstring prettyName(const std::wstring& path){SHFILEINFOW data{};if(SHGetFileInfoW(path.c_str(),0,&data,sizeof(data),SHGFI_DISPLAYNAME)&&*data.szDisplayName)return data.szDisplayName;return std::filesystem::path(path).filename().wstring();}
std::wstring textOf(HWND hwnd){int n=GetWindowTextLengthW(hwnd);std::wstring s(n+1,0);GetWindowTextW(hwnd,s.data(),n+1);s.resize(n);return s;}
std::wstring formattedTime(int n){wchar_t text[32];if(n>=3600)swprintf(text,32,L"%d:%02d:%02d",n/3600,n/60%60,n%60);else swprintf(text,32,L"%d:%02d",n/60,n%60);return text;}
std::wstring weatherText(int code){if(code==0)return L"Ясно";if(code<=3)return L"Облачно";if(code<=48)return L"Туман";if(code<=67)return L"Дождь";if(code<=77)return L"Снег";if(code<=82)return L"Ливень";if(code<=86)return L"Снегопад";return L"Гроза";}
const wchar_t* weatherIcon(int code){return code==0?L"\uE706":code<=3?L"\uE753":code<=48?L"\uE9BF":code<=67?L"\uE9C4":code<=86?L"\uE9C9":L"\uE9C6";}
HWND desktopHost(){HWND result=nullptr;EnumWindows([](HWND root,LPARAM p)->BOOL{wchar_t cls[100]={};GetClassNameW(root,cls,100);if(wcscmp(cls,L"WorkerW")&&wcscmp(cls,L"Progman"))return TRUE;if(FindWindowExW(root,nullptr,L"SHELLDLL_DefView",nullptr)){*(HWND*)p=root;return FALSE;}return TRUE;},(LPARAM)&result);return result;}
bool monitorBlocked(RECT bounds){
    struct Check{RECT monitor;bool blocked=false;} check{bounds};
    EnumWindows([](HWND w,LPARAM p)->BOOL{auto& c=*(Check*)p;if(!isTaskWindow(w)||IsIconic(w))return TRUE;MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&info);if(!EqualRect(&info.rcMonitor,&c.monitor))return TRUE;RECT r{};if(FAILED(DwmGetWindowAttribute(w,DWMWA_EXTENDED_FRAME_BOUNDS,&r,sizeof(r))))GetWindowRect(w,&r);if(IsZoomed(w)||(r.left<=c.monitor.left+2&&r.top<=c.monitor.top+2&&r.right>=c.monitor.right-2&&r.bottom>=c.monitor.bottom-2)){c.blocked=true;return FALSE;}return TRUE;},(LPARAM)&check);return check.blocked;
}
bool exposed(RECT bounds){
    HRGN remaining=CreateRectRgnIndirect(&bounds);
    EnumWindows([](HWND w,LPARAM p)->BOOL{
        if(!isTaskWindow(w)||IsIconic(w))return TRUE;auto style=GetWindowLongPtrW(w,GWL_EXSTYLE);if(style&WS_EX_TRANSPARENT)return TRUE;
        if(style&WS_EX_LAYERED){BYTE alpha=0;DWORD flags=0;COLORREF key;if(!GetLayeredWindowAttributes(w,&key,&alpha,&flags)||!(flags&LWA_ALPHA)||alpha<255)return TRUE;}
        RECT r{};if(FAILED(DwmGetWindowAttribute(w,DWMWA_EXTENDED_FRAME_BOUNDS,&r,sizeof(r))))GetWindowRect(w,&r);
        HRGN cover=CreateRectRgnIndirect(&r);int left=CombineRgn((HRGN)p,(HRGN)p,cover,RGN_DIFF);DeleteObject(cover);return left!=NULLREGION;
    },(LPARAM)remaining);
    RECT left{};bool visible=GetRgnBox(remaining,&left)!=NULLREGION;DeleteObject(remaining);return visible;
}
bool foregroundDesktop(){HWND w=GetForegroundWindow();wchar_t cls[100]={};GetClassNameW(w,cls,100);return !wcscmp(cls,L"Progman")||!wcscmp(cls,L"WorkerW")||!wcscmp(cls,DeskClass)||!wcscmp(cls,HostClass);}
void keepBelowApplications(HWND host){
    if(!IsWindow(host)||(!StandaloneMode&&foregroundDesktop()))return;
    struct Order{HWND host,last=nullptr;bool seen=false,above=false;} order{host};
    EnumWindows([](HWND w,LPARAM p)->BOOL{auto& o=*(Order*)p;if(w==o.host)o.seen=true;else if(isTaskWindow(w)&&!IsIconic(w)){o.last=w;if(o.seen)o.above=true;}return TRUE;},(LPARAM)&order);
    if(StandaloneMode){
        // Keep our root above Explorer's background, but below ordinary apps.
        HWND native=desktopHost();bool nativeAbove=false;
        if(native)for(HWND w=GetWindow(host,GW_HWNDPREV);w;w=GetWindow(w,GW_HWNDPREV))if(w==native){nativeAbove=true;break;}
        if(order.above||nativeAbove||!IsWindowVisible(host))SetWindowPos(host,order.last?order.last:HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_SHOWWINDOW);
    }else if(order.above&&order.last)SetWindowPos(host,order.last,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
}
int monthDays(int year,int month){static int days[]={31,28,31,30,31,30,31,31,30,31,30,31};return days[month-1]+(month==2&&year%4==0&&(year%100!=0||year%400==0));}
struct Entry {std::wstring path,name,target;bool directory=false;std::shared_ptr<const PixelImage> icon;};
struct Catalog {std::vector<Entry> files;std::map<std::wstring,Entry> apps;};
Entry readEntry(const std::wstring& path){Entry e;e.path=e.target=path;e.name=prettyName(path);auto attr=GetFileAttributesW(path.c_str());e.directory=attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY);auto ext=std::filesystem::path(path).extension().wstring();if(!_wcsicmp(ext.c_str(),L".lnk")){Com<IShellLinkW> link;Com<IPersistFile> file;if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put())))&&SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(file.put())))&&SUCCEEDED(file->Load(path.c_str(),STGM_READ))){wchar_t target[32768]={};if(SUCCEEDED(link->GetPath(target,32768,nullptr,0))&&*target)e.target=target;}if(e.name.ends_with(L".lnk"))e.name.resize(e.name.size()-4);}return e;}
void folderLocation(const std::wstring& path){openFileLocation(path);}
}

struct Desktop::Impl {
    struct View;
    HWND owner=nullptr,host=nullptr;
    AudioMeter* audio=nullptr;
    std::function<DesktopData()> snapshot;
    std::function<void(int,const std::wstring&)> action;
    std::vector<std::unique_ptr<View>> views;
    std::mutex mutex;std::condition_variable wake;std::thread worker;
    bool stopping=false,dirty=true,rebuilding=false,displayDirty=false;
    std::vector<std::wstring> pinned,extraFiles;
    std::shared_ptr<const Catalog> catalog=std::make_shared<Catalog>();
    std::wstring ini=stateDirectory()+L"\\settings.ini";
    bool activeAudio=false;int spectrumMode=0,eqFPS=30,eqBands=48;float gain=1;bool eqEnabled=true,eqSmooth=true,eqPeaks=true;
    ULONGLONG statsTick=0,previousIdle=0,previousKernel=0,previousUser=0;float cpuUsage=0;
    void stats(){auto now=GetTickCount64();if(now-statsTick<1000)return;statsTick=now;FILETIME idle{},kernel{},user{};if(GetSystemTimes(&idle,&kernel,&user)){auto number=[](FILETIME f){return ((ULONGLONG)f.dwHighDateTime<<32)|f.dwLowDateTime;};auto i=number(idle),k=number(kernel),u=number(user),total=k-previousKernel+u-previousUser;if(previousKernel&&total)cpuUsage=std::clamp(100.f*(1.f-(i-previousIdle)/(float)total),0.f,100.f);previousIdle=i;previousKernel=k;previousUser=u;}}
    void saveEQ(){for(auto pair:{std::pair{L"spectrum",spectrumMode},std::pair{L"eq-fps",eqFPS},std::pair{L"eq-bands",eqBands},std::pair{L"eq-enabled",eqEnabled?1:0},std::pair{L"eq-smooth",eqSmooth?1:0},std::pair{L"eq-peaks",eqPeaks?1:0}})WritePrivateProfileStringW(L"desktop",pair.first,std::to_wstring(pair.second).c_str(),ini.c_str());WritePrivateProfileStringW(L"desktop",L"gain",std::to_wstring(gain).c_str(),ini.c_str());}

    int preset=0,placement=0,shade=28,clockStyle=0;bool clockShadow=false;
    void saveShadow(){WritePrivateProfileStringW(L"desktop",L"clock-shadow",clockShadow?L"1":L"0",ini.c_str());}
    WallpaperMedia media;
    bool photo=false;
    void saveAppearance(){WritePrivateProfileStringW(L"desktop",L"wallpaper-preset",std::to_wstring(preset).c_str(),ini.c_str());WritePrivateProfileStringW(L"desktop",L"wallpaper-fit",std::to_wstring(placement).c_str(),ini.c_str());WritePrivateProfileStringW(L"desktop",L"wallpaper-shade",std::to_wstring(shade).c_str(),ini.c_str());}
    void selectPreset(int p){media.clear();preset=p;photo=false;saveAppearance();WritePrivateProfileStringW(L"desktop",L"wallpaper-photo",L"0",ini.c_str());}
    void selectWallpaper(bool video=false){media.choose(owner,video);}
    void run();void rebuild();void stop();void refresh(){std::lock_guard lock(mutex);dirty=true;wake.notify_one();}
    void invoke(int code,const std::wstring& value=L""){if(action)action(code,value);}
    void addFile(const std::wstring& path){bool added=false;{std::lock_guard lock(mutex);if(std::find(extraFiles.begin(),extraFiles.end(),path)==extraFiles.end()&&extraFiles.size()<64){extraFiles.push_back(path);added=true;}}if(added){saveFiles();refresh();}}
    void saveFiles(){WritePrivateProfileStringW(L"desktop-files",nullptr,nullptr,ini.c_str());for(size_t i=0;i<extraFiles.size();i++)WritePrivateProfileStringW(L"desktop-files",std::to_wstring(i).c_str(),extraFiles[i].c_str(),ini.c_str());}
    static LRESULT CALLBACK proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp);
};
struct Desktop::Impl::View {
    Impl* parent;HWND hwnd=nullptr,backdrop=nullptr,note=nullptr,timer=nullptr;WNDPROC noteProc=nullptr,timerProc=nullptr;
    std::unique_ptr<Graphics> gfx;HFONT font=nullptr,timerFont=nullptr;HBRUSH brush=nullptr;
    RECT monitor{};float scale=1,width=1728,height=972,left=64,top=82;
    DesktopData data;std::shared_ptr<const Catalog> catalog;
    struct Hit {Box box;std::function<void()> click;int kind=0,index=-1;std::wstring path;int widget=-1;};std::vector<Hit> hits;
    int appsPerPage=12;int hover=-1,appPage=0,filePage=0,taskPage=0,selectedFile=-1,pressed=-1;
    POINT down{};bool visible=false,calendar=false,editing=false,tracking=false,menu=false,noteDirty=false;
    int calendarMonth=0,timerInterval=33,page=0,toolTab=0;
    bool appearance=false;
    enum Widget {Navigation,Controls,Clock,Spectrum,Apps,Weather,Music,Tools,Files,Windows,Calendar,Timer,System,Folders,Image,WidgetCount};
    struct Placement {float x=0,y=0;bool shown=true;float w=0,h=0;bool vertical=false,shadow=false,background=true;float opacity=1;float angle=0;int captionAlign=-1,skin=0,mediaFit=1;};
    std::array<Placement,WidgetCount> layout{},savedLayout{};
    std::array<int,WidgetCount> order{},savedOrder{};
    std::wstring layoutSection;
    bool arranging=false,savedShadow=false;int grid=16,savedGrid=16,savedPage=0,dragWidget=-1,selectedWidget=-1,renderingWidget=-1;
    int eqMode=0,eqFPS=30,eqBands=48;float eqGain=1;bool eqEnabled=true,eqSmooth=true,eqPeaks=true,eqNumbers=true;
    WidgetImageMedia imageMedia;WidgetImageState imageState;
    void saveEQ(){for(auto pair:{std::pair{L"eq-mode",eqMode},std::pair{L"eq-fps",eqFPS},std::pair{L"eq-bands",eqBands},std::pair{L"eq-enabled",eqEnabled?1:0},std::pair{L"eq-smooth",eqSmooth?1:0},std::pair{L"eq-peaks",eqPeaks?1:0},std::pair{L"eq-numbers",eqNumbers?1:0}})WritePrivateProfileStringW(layoutSection.c_str(),pair.first,std::to_wstring(pair.second).c_str(),parent->ini.c_str());WritePrivateProfileStringW(layoutSection.c_str(),L"eq-gain",std::to_wstring(eqGain).c_str(),parent->ini.c_str());}
    float grabX=0,grabY=0;int dragHandle=-1;Box resizeStart{};float resizeX=0,resizeY=0;
    bool eqSettings=false,suppressPanel=false;float drawK=1,drawX=0,drawY=0;Box renderBox{};ULONGLONG eqTick=0;
    float noteFontScale=0,timerFontScale=0;HRGN noteMask=nullptr,timerMask=nullptr;
    Box contentBox(int id,Box value)const;Box titleBox(int id)const;Box closeBox(int id)const{auto t=titleBox(id);return {t.x+t.w+4,t.y,24,24};}void rotationMenu(int id);void clockAlignMenu();void musicStyleMenu();void imageFitMenu();void imageWidget();void clockCaption(const std::wstring& text,Box b,float size,unsigned color=0);void calendarWidget();void systemWidget();void foldersWidget();void eqControls();void widgetSettings(int id);int resizeHandleAt(float x,float y)const;void resizeWidget(float x,float y);void resetLayout();
    static Box minimum(int id,bool vertical){if(id==Navigation)return {0,0,vertical?110.f:250.f,vertical?130.f:42.f};if(id==Controls)return {0,0,vertical?44.f:210.f,vertical?210.f:44.f};if(id==Apps)return {0,0,vertical?64.f:180.f,vertical?180.f:64.f};if(id==Clock)return {0,0,240,100};if(id==Spectrum)return {0,0,180,100};return {0,0,180,120};}

    static const wchar_t* widgetName(int id){static const wchar_t* names[]={L"Разделы",L"Управление",L"Часы",L"Эквалайзер",L"Приложения",L"Погода",L"Музыка",L"Заметка",L"Файлы",L"Окна",L"Календарь",L"Таймер",L"Система",L"Папки",L"Изображение / GIF"};return names[id];}
    Box baseBounds(int id)const;
    Box bounds(int id)const;
    bool onPage(int id)const{return id==Clock||id==Spectrum||id==Calendar||id==Timer||id==System||id==Folders||id==Image?page==0:id==Files?page==1:id==Windows?page==2:true;}
    int widgetAt(float x,float y)const;
    int hitAt(float x,float y)const;
    void loadLayout();void saveLayout();void beginArrange();void finishArrange(bool save);void arrangeControls();void widgetMenu();void gridMenu();void renderWidget(int id);
    Box musicSeek()const{auto b=baseBounds(Music);return layout[Music].skin==1?Box{b.x+18,b.y+114,424,3}:layout[Music].skin==2?Box{b.x+18,b.y+340,284,3}:Box{b.x+24,b.y+112,312,3};}
    float musicRatio(float x,float y)const{auto point=rotateWidgetPoint({x,y},bounds(Music),-layout[Music].angle);auto a=contentBox(Music,musicSeek());return std::clamp((point.x-a.x)/a.w,0.f,1.f);}
    bool widgetContains(int id,float x,float y)const{auto b=bounds(id);auto point=rotateWidgetPoint({x,y},b,-layout[id].angle);return b.contains(point.x,point.y);}
    bool hitContains(const Hit& hit,float x,float y)const{if(hit.widget>=0){auto point=rotateWidgetPoint({x,y},bounds(hit.widget),-layout[hit.widget].angle);return hit.box.contains(point.x,point.y);}return hit.box.contains(x,y);}


    std::shared_ptr<const PixelImage> wallpaper;
    bool photo=false,wallLoading=false,motion=false,musicOver=false,seekDragging=false;float seekRatio=0;
    std::wstring wallName,wallError;
    ULONGLONG animation=0,lastFrame=0,lastDraw=0,clockTick=0,lastVisibility=0;
    std::array<float,48> levels{},peaks{};SpectrumFrame spectrum;
    View(Impl* p):parent(p){std::iota(order.begin(),order.end(),0);}
    ~View(){if(IsWindow(hwnd))DestroyWindow(hwnd);if(IsWindow(backdrop))DestroyWindow(backdrop);if(font)DeleteObject(font);if(timerFont)DeleteObject(timerFont);if(brush)DeleteObject(brush);if(noteMask)DeleteObject(noteMask);if(timerMask)DeleteObject(timerMask);}
    Box box(float x,float y,float w,float h)const{return {left+x,top+y,w,h};}
    void add(Box b,std::function<void()> click,int kind=0,int index=-1,std::wstring path=L""){b={drawX+b.x*drawK,drawY+b.y*drawK,b.w*drawK,b.h*drawK};hits.push_back({b,std::move(click),kind,index,std::move(path),renderingWidget});}
    void label(const std::wstring& s,Box b,float size=13,unsigned color=0,bool bold=false,DWRITE_TEXT_ALIGNMENT align=DWRITE_TEXT_ALIGNMENT_LEADING){gfx->text(s,b,size,color?color:data.ink,bold,align,false,L"Bahnschrift");}
    unsigned inputColor()const{return blend(data.background,data.card,.88f);}
    void panel(Box b){if(suppressPanel)return;gfx->fill({b.x,b.y+8,b.w,b.h},0,25,.16f);gfx->fill(b,blend(data.background,data.card,.66f),25,.9f);gfx->stroke(b,blend(data.background,data.ink,.14f),25,.65f);}
    void button(Box b,const wchar_t* glyph,const std::wstring& text,std::function<void()> click,bool selected=false,bool enabled=true){bool over=enabled&&hover==(int)hits.size();gfx->fill(b,!enabled?blend(data.background,data.card,.55f):selected?blend(data.card,data.accent,.24f):over?data.hover:data.card,12);if(glyph)gfx->icon(glyph,{b.x+9,b.y,25,b.h},15,!enabled?blend(data.card,data.muted,.6f):selected?data.accent:data.muted);label(text,{b.x+(glyph?42:12),b.y,b.w-(glyph?52:24),b.h},12,enabled?data.ink:blend(data.card,data.muted,.7f));if(enabled)add(b,std::move(click));else add(b,[]{},-1);}
    void iconButton(Box b,const wchar_t* glyph,std::function<void()> click){if(hover==(int)hits.size())gfx->fill(b,data.hover,12);gfx->icon(glyph,b,17,data.muted);add(b,std::move(click));}
    void dockControl(Box b,bool plus,std::function<void()> click){if(hover==(int)hits.size())gfx->fill(b,data.hover,12);float cx=b.x+b.w/2,cy=b.y+b.h/2;if(plus){gfx->line(cx-9,cy,cx+9,cy,data.muted,1.2f);gfx->line(cx,cy-9,cx,cy+9,data.muted,1.2f);}else for(int i=-1;i<=1;i++){gfx->fill({cx-8,cy+i*6-1,2,2},data.muted);gfx->line(cx-3,cy+i*6,cx+8,cy+i*6,data.muted,1.2f);}add(b,std::move(click));}
    void resize();void draw();void pulse(bool force=false);void editors(bool show);void context(POINT position);void dragFile(const std::wstring& path);void clock();void apps();void weather();void music();void spectrumWidget();void notes();void timerWidget();void files();void tasks();void wallpaperControls();void background();
    static LRESULT CALLBACK inputProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp);
};

void Desktop::Impl::run(){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);std::map<std::wstring,std::shared_ptr<const PixelImage>> cache;std::map<std::wstring,std::wstring> names;uint64_t nextId=1ull<<52;
    while(true){std::vector<std::wstring> apps,extra;
        {std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::seconds(4),[&]{return stopping||dirty;});if(stopping)break;dirty=false;apps=pinned;extra=extraFiles;}
        auto next=std::make_shared<Catalog>();std::set<std::wstring> seen;
        auto load=[&](const std::wstring& path){auto e=readEntry(path);auto& icon=cache[path];if(!icon)icon=loadShellIcon(path,nextId++);e.icon=icon;return e;};
        for(auto& path:apps){auto app=load(path);auto& name=names[path];if(name.empty()){name=app.name;Com<IPropertyStore> properties;if(SUCCEEDED(SHGetPropertyStoreFromParsingName(app.target.c_str(),nullptr,GPS_BESTEFFORT,IID_PPV_ARGS(properties.put())))){PROPVARIANT value{};if(SUCCEEDED(properties->GetValue(PKEY_FileDescription,&value))&&value.vt==VT_LPWSTR&&value.pwszVal&&*value.pwszVal)name=value.pwszVal;PropVariantClear(&value);}if(name.size()>4&&!_wcsicmp(name.substr(name.size()-4).c_str(),L".exe"))name.resize(name.size()-4);}app.name=name;next->apps.emplace(path,std::move(app));}
        for(auto& path:extra)if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES&&seen.insert(path).second)next->files.push_back(load(path));
        std::vector<std::wstring> paths;
        for(auto root:{knownFolder(FOLDERID_Desktop),knownFolder(FOLDERID_PublicDesktop)}){std::error_code error;if(root.empty())continue;for(std::filesystem::directory_iterator it(root,std::filesystem::directory_options::skip_permission_denied,error),end;it!=end&&paths.size()<256;it.increment(error)){if(error){error.clear();continue;}auto path=it->path().wstring();DWORD attr=GetFileAttributesW(path.c_str());if(attr==INVALID_FILE_ATTRIBUTES||(attr&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)))continue;if(seen.insert(path).second)paths.push_back(path);}}
        std::stable_sort(paths.begin(),paths.end(),[](auto& a,auto& b){bool x=(GetFileAttributesW(a.c_str())&FILE_ATTRIBUTE_DIRECTORY)!=0,y=(GetFileAttributesW(b.c_str())&FILE_ATTRIBUTE_DIRECTORY)!=0;return x!=y?x>y:_wcsicmp(a.c_str(),b.c_str())<0;});
        for(auto& path:paths)next->files.push_back(load(path));
        if(cache.size()>384){std::set<std::wstring> keep;for(auto& e:next->files)keep.insert(e.path);for(auto& e:next->apps)keep.insert(e.first);for(auto it=cache.begin();it!=cache.end();)if(!keep.contains(it->first))it=cache.erase(it);else ++it;}
        {std::lock_guard lock(mutex);catalog=next;}
        PostMessageW(owner,WM_APP+13,0,0);
    }CoUninitialize();
}


Box Desktop::Impl::View::baseBounds(int id)const{
    bool vertical=layout[id].vertical;
    switch(id){case Navigation:return {0,-4,vertical?184.f:394.f,vertical?150.f:46.f};case Controls:return {1310,-4,vertical?52.f:290.f,vertical?290.f:44.f};case Clock:return {0,95,1065,320};case Spectrum:return {0,446,1130,232};case Apps:{int count=std::min(12,std::max(0,(int)data.pinned.size()-appPage*12));float w=std::max(250.f,count*66.f+150);return {(1130-w)/2,740,vertical?92.f:w,vertical?w:92.f};}case Weather:return {1240,86,360,172};case Music:return layout[Music].skin==1?Box{1240,278,460,128}:layout[Music].skin==2?Box{1240,278,320,408}:Box{1240,278,360,208};case Tools:case Timer:return {1240,506,360,296};case Calendar:return {0,95,560,356};case System:return {680,100,360,240};case Folders:return {680,370,360,220};case Image:return {650,110,360,300};default:return {0,86,1130,600};}
}
Box Desktop::Impl::View::bounds(int id)const{auto b=baseBounds(id);auto min=minimum(id,layout[id].vertical);if(layout[id].w>0)b.w=layout[id].w;if(layout[id].h>0)b.h=layout[id].h;b.w=std::clamp(b.w,min.w,std::max(min.w,width-24));b.h=std::clamp(b.h,min.h,std::max(min.h,height-90));b.x=std::clamp(left+b.x+layout[id].x,12.f,std::max(12.f,width-b.w-12));b.y=std::clamp(top+b.y+layout[id].y,42.f,std::max(42.f,height-b.h-12));return constrainWidget(b,layout[id].angle,width,height);}
Box Desktop::Impl::View::contentBox(int id,Box value)const{auto b=baseBounds(id),a=bounds(id);float k=std::min(a.w/b.w,a.h/b.h);return {a.x+(a.w-b.w*k)/2+(value.x-b.x)*k,a.y+(a.h-b.h*k)/2+(value.y-b.y)*k,value.w*k,value.h*k};}
Box Desktop::Impl::View::titleBox(int id)const{auto b=rotatedWidgetBounds(bounds(id),layout[id].angle);float w=std::min(280.f,(gfx?gfx->textWidth(widgetName(id)):wcslen(widgetName(id))*7.f)+20);return {std::clamp(b.x+14,12.f,std::max(12.f,width-w-40)),std::max(8.f,b.y-28),w,24};}
int Desktop::Impl::View::widgetAt(float x,float y)const{for(auto it=order.rbegin();it!=order.rend();++it)if(layout[*it].shown&&onPage(*it)&&!(appearance&&(*it==Weather||*it==Music||*it==Tools))&&(widgetContains(*it,x,y)||(arranging&&(titleBox(*it).contains(x,y)||closeBox(*it).contains(x,y)))))return *it;return -1;}
int Desktop::Impl::View::hitAt(float x,float y)const{for(size_t i=0;i<hits.size();i++)if(hits[i].widget==-2&&hits[i].box.contains(x,y))return (int)i;if(arranging)return -1;int widget=widgetAt(x,y);for(size_t i=0;i<hits.size();i++)if(hits[i].widget==widget&&widget!=Spectrum&&hitContains(hits[i],x,y))return (int)i;return -1;}
void Desktop::Impl::View::resetLayout(){layout={};for(int i=0;i<WidgetCount;i++){order[i]=i;layout[i].shown=i<Calendar;layout[i].background=i!=Clock&&i!=Spectrum&&i!=Controls&&i!=Image;}selectedWidget=-1;}
void Desktop::Impl::View::loadLayout(){
    resetLayout();layout[Clock].shadow=parent->clockShadow;MONITORINFOEXW info{};info.cbSize=sizeof(info);GetMonitorInfoW(MonitorFromRect(&monitor,MONITOR_DEFAULTTONEAREST),&info);std::wstring device=info.szDevice;for(auto& c:device)if(!iswalnum(c))c=L'_';layoutSection=L"desktop-layout-"+device;
    eqMode=std::clamp((int)GetPrivateProfileIntW(layoutSection.c_str(),L"eq-mode",parent->spectrumMode,parent->ini.c_str()),0,7);eqFPS=std::clamp((int)GetPrivateProfileIntW(layoutSection.c_str(),L"eq-fps",parent->eqFPS,parent->ini.c_str()),5,60);eqBands=GetPrivateProfileIntW(layoutSection.c_str(),L"eq-bands",parent->eqBands,parent->ini.c_str());if(eqBands!=12&&eqBands!=24)eqBands=48;eqEnabled=GetPrivateProfileIntW(layoutSection.c_str(),L"eq-enabled",parent->eqEnabled,parent->ini.c_str())!=0;eqSmooth=GetPrivateProfileIntW(layoutSection.c_str(),L"eq-smooth",parent->eqSmooth,parent->ini.c_str())!=0;eqPeaks=GetPrivateProfileIntW(layoutSection.c_str(),L"eq-peaks",parent->eqPeaks,parent->ini.c_str())!=0;wchar_t eqValue[32];GetPrivateProfileStringW(layoutSection.c_str(),L"eq-gain",std::to_wstring(parent->gain).c_str(),eqValue,32,parent->ini.c_str());{float value=wcstof(eqValue,nullptr);eqGain=std::isfinite(value)?std::clamp(value,.5f,2.f):1.f;}eqNumbers=GetPrivateProfileIntW(layoutSection.c_str(),L"eq-numbers",1,parent->ini.c_str())!=0;
    grid=std::clamp((int)GetPrivateProfileIntW(layoutSection.c_str(),L"grid",16,parent->ini.c_str()),8,48);std::set<int> seen;
    for(int id=0;id<WidgetCount;id++){wchar_t v[384]={};GetPrivateProfileStringW(layoutSection.c_str(),std::to_wstring(id).c_str(),L"",v,384,parent->ini.c_str());if(*v){float x=0,y=0,w=0,h=0,opacity=1,angle=0;int caption=-1,skin=0,fit=1;int shown=1,vertical=0,shadow=id==Clock&&parent->clockShadow,background=layout[id].background;swscanf(v,L"%f,%f,%d,%f,%f,%d,%d,%d,%f,%f,%d,%d,%d",&x,&y,&shown,&w,&h,&vertical,&shadow,&background,&opacity,&angle,&caption,&skin,&fit);auto finite=[](float f,float lo,float hi,float def=0.f){return std::isfinite(f)?std::clamp(f,lo,hi):def;};layout[id]={finite(x,-4096,4096),finite(y,-4096,4096),shown!=0,finite(w,0,4096),finite(h,0,4096),vertical!=0,shadow!=0,background!=0,finite(opacity,.3f,1,1),finite(angle,0,359.99f),std::clamp(caption,-1,2),std::clamp(skin,0,2),std::clamp(fit,0,2)};}auto key=L"z"+std::to_wstring(id);order[id]=GetPrivateProfileIntW(layoutSection.c_str(),key.c_str(),id,parent->ini.c_str());}for(int id:order)seen.insert(id);if(seen.size()!=WidgetCount||*seen.begin()!=0||*seen.rbegin()!=WidgetCount-1)std::iota(order.begin(),order.end(),0);
}
void Desktop::Impl::View::saveLayout(){WritePrivateProfileStringW(layoutSection.c_str(),L"grid",std::to_wstring(grid).c_str(),parent->ini.c_str());for(int id=0;id<WidgetCount;id++){auto& p=layout[id];wchar_t value[384];swprintf(value,384,L"%.1f,%.1f,%d,%.1f,%.1f,%d,%d,%d,%.2f,%.1f,%d,%d,%d",p.x,p.y,p.shown?1:0,p.w,p.h,p.vertical?1:0,p.shadow?1:0,p.background?1:0,p.opacity,p.angle,p.captionAlign,p.skin,p.mediaFit);WritePrivateProfileStringW(layoutSection.c_str(),std::to_wstring(id).c_str(),value,parent->ini.c_str());auto key=L"z"+std::to_wstring(id);WritePrivateProfileStringW(layoutSection.c_str(),key.c_str(),std::to_wstring(order[id]).c_str(),parent->ini.c_str());}}
void Desktop::Impl::View::beginArrange(){if(arranging)return;savedLayout=layout;savedOrder=order;savedGrid=grid;savedPage=page;appearance=eqSettings=false;arranging=true;selectedWidget=-1;hover=-1;editors(false);draw();}
void Desktop::Impl::View::finishArrange(bool save){dragWidget=dragHandle=-1;ReleaseCapture();if(save)saveLayout();else{layout=savedLayout;order=savedOrder;grid=savedGrid;}page=savedPage;arranging=false;hover=-1;draw();}
void Desktop::Impl::View::widgetMenu(){std::vector<MenuItem> items;for(int i=0;i<WidgetCount;i++)items.push_back({widgetName(i),nullptr,i+1,layout[i].shown});POINT p;GetCursorPos(&p);menu=true;int r=customMenu(hwnd,p,items,{data.background,data.hover,data.ink,data.muted,data.accent,data.outline,data.outlineWidth,data.outlineSmooth});menu=false;if(r>0&&r<=WidgetCount){int id=r-1;layout[id].shown=true;selectedWidget=id;if(id==Files)page=1;else if(id==Windows)page=2;else if(id>=Calendar||id==Clock||id==Spectrum)page=0;}draw();}
void Desktop::Impl::View::gridMenu(){std::vector<MenuItem> items;for(int n:{8,16,24,32,48})items.push_back({std::to_wstring(n),nullptr,n,grid==n});POINT p;GetCursorPos(&p);menu=true;int r=customMenu(hwnd,p,items,{data.background,data.hover,data.ink,data.muted,data.accent,data.outline,data.outlineWidth,data.outlineSmooth});menu=false;if(r)grid=r;draw();}
void Desktop::Impl::View::widgetSettings(int id){if(id<0||id>=WidgetCount||!layout[id].shown)return;selectedWidget=id;auto& p=layout[id];std::vector<MenuItem> items={{L"Показывать",nullptr,1,p.shown},{L"Тень",nullptr,2,p.shadow},{L"Фон",nullptr,3,p.background},{L"На передний план",L"\uE74A",4},{L"На задний план",L"\uE74B",5},{L"На слой выше",nullptr,6},{L"На слой ниже",nullptr,7},{L"Исходный размер",nullptr,8},{L"Масштаб 75%",nullptr,9},{L"Масштаб 125%",nullptr,10},{L"Непрозрачность 100%",nullptr,11,p.opacity>.95f},{L"Непрозрачность 75%",nullptr,12,p.opacity<.95f&&p.opacity>.6f},{L"Непрозрачность 50%",nullptr,13,p.opacity<=.6f}};
    if(id==Tools||id==Timer)std::erase_if(items,[](auto& item){return item.id>=11&&item.id<=13;});if(id==Navigation||id==Controls||id==Apps){items.insert(items.begin()+3,{L"Вертикально",nullptr,14,p.vertical});}items.push_back({L"Поворот: "+std::to_wstring((int)p.angle)+L"°",nullptr,16});if(id==Clock)items.push_back({L"Дата и день недели",nullptr,17});if(id==Music)items.push_back({L"Вид музыки",nullptr,18});if(id==Image){items.push_back({L"Выбрать изображение / GIF",L"\uE8B9",19});items.push_back({L"Масштаб изображения",nullptr,20});items.push_back({L"Убрать изображение",nullptr,21});}if(id==Spectrum)items.push_back({L"Настройки эквалайзера",L"\uE713",15});
    POINT pos;GetCursorPos(&pos);menu=true;int r=customMenu(hwnd,pos,items,{data.background,data.hover,data.ink,data.muted,data.accent,data.outline,data.outlineWidth,data.outlineSmooth});menu=false;
    if(r==1)p.shown=!p.shown;if(r==2)p.shadow=!p.shadow;if(r==3)p.background=!p.background;auto at=std::find(order.begin(),order.end(),id);if(r==4)std::rotate(at,at+1,order.end());if(r==5)std::rotate(order.begin(),at,at+1);if(r==6&&at+1!=order.end())std::iter_swap(at,at+1);if(r==7&&at!=order.begin())std::iter_swap(at,at-1);if(r==8)p.w=p.h=0;if(r==9||r==10){auto b=baseBounds(id);p.w=b.w*(r==9?.75f:1.25f);p.h=b.h*(r==9?.75f:1.25f);}if(r>=11&&r<=13)p.opacity=r==11?1:r==12?.75f:.5f;if(r==14){auto old=bounds(id);p.vertical=!p.vertical;p.w=p.h=0;auto base=baseBounds(id);p.x=old.x-left-base.x;p.y=old.y-top-base.y;}if(r==15){eqSettings=true;editors(false);}if(r==16)rotationMenu(id);if(r==17)clockAlignMenu();if(r==18)musicStyleMenu();if(r==19)imageMedia.choose(parent->owner);if(r==20)imageFitMenu();if(r==21)imageMedia.clear();if(r&&!arranging)saveLayout();hover=-1;draw();
}

int Desktop::Impl::View::resizeHandleAt(float x,float y)const{if(!arranging||selectedWidget<0||!layout[selectedWidget].shown)return -1;auto b=bounds(selectedWidget);float px[]={b.x,b.x+b.w/2,b.x+b.w,b.x+b.w,b.x+b.w,b.x+b.w/2,b.x,b.x},py[]={b.y,b.y,b.y,b.y+b.h/2,b.y+b.h,b.y+b.h,b.y+b.h,b.y+b.h/2};for(int i=0;i<8;i++){auto p=rotateWidgetPoint({px[i],py[i]},b,layout[selectedWidget].angle);if(Box{p.x-7,p.y-7,14,14}.contains(x,y))return i;}return -1;}
void Desktop::Impl::View::resizeWidget(float x,float y){int id=selectedWidget,h=dragHandle;auto b=resizeStart;auto old=rotateWidgetPoint({resizeX,resizeY},b,-layout[id].angle),point=rotateWidgetPoint({x,y},b,-layout[id].angle);float dx=point.x-old.x,dy=point.y-old.y,x1=b.x,y1=b.y,x2=b.x+b.w,y2=b.y+b.h;auto snap=[&](float f){return std::round(f/grid)*grid;};auto min=minimum(id,layout[id].vertical);if(h==0||h==6||h==7)x1=std::clamp(snap(x1+dx),x2-width+24,x2-min.w);if(h==2||h==3||h==4)x2=std::clamp(snap(x2+dx),x1+min.w,x1+width-24);if(h==0||h==1||h==2)y1=std::clamp(snap(y1+dy),y2-height+54,y2-min.h);if(h==4||h==5||h==6)y2=std::clamp(snap(y2+dy),y1+min.h,y1+height-54);auto center=rotateWidgetPoint({(x1+x2)/2,(y1+y2)/2},b,layout[id].angle);Box next{center.x-(x2-x1)/2,center.y-(y2-y1)/2,x2-x1,y2-y1};next=constrainWidget(next,layout[id].angle,width,height);auto base=baseBounds(id);auto& p=layout[id];p.x=next.x-left-base.x;p.y=next.y-top-base.y;p.w=next.w;p.h=next.h;draw();}
void Desktop::Impl::View::arrangeControls(){float x=(width-890)/2,y=height-62;gfx->fill({x-8,y-6,906,50},data.background,16,.98f,false);auto control=[&](float w,const wchar_t* glyph,const std::wstring& text,std::function<void()> click,bool chosen=false,bool enabled=true){button({x,y,w,38},glyph,text,std::move(click),chosen,enabled);x+=w+8;};const wchar_t* pages[]={L"Главная",L"Файлы",L"Окна"};control(112,L"\uE8A9",pages[page],[this]{page=(page+1)%3;draw();});control(116,L"\uE71D",L"Виджеты",[this]{widgetMenu();});control(104,nullptr,L"Сетка "+std::to_wstring(grid),[this]{gridMenu();});control(112,L"\uE777",L"Сбросить",[this]{resetLayout();draw();});control(142,L"\uE713",L"Свойства",[this]{widgetSettings(selectedWidget);},false,selectedWidget>=0&&layout[selectedWidget].shown);control(112,nullptr,L"Отмена",[this]{finishArrange(false);});control(136,L"\uE73E",L"Готово",[this]{finishArrange(true);},true);}
void Desktop::Impl::View::renderWidget(int id){
    auto base=baseBounds(id),a=bounds(id);auto& place=layout[id];renderBox=a;auto previous=gfx->transform();auto rotated=D2D1::Matrix3x2F::Rotation(place.angle,{a.x+a.w/2,a.y+a.h/2})*previous;gfx->layer(place.opacity);gfx->transform(rotated);renderingWidget=id;float oldLeft=left,oldTop=top;left=top=0;drawK=1;drawX=drawY=0;
    if(place.shadow&&id!=Clock){for(int d=12;d>=2;d-=2)gfx->fill({a.x-d*.4f,a.y+4-d*.2f,a.w+d*.8f,a.h+d*.4f},0,24+d*.2f,.025f,false);}
    if(place.background){suppressPanel=false;panel(a);}suppressPanel=true;
    if(id==Navigation){const wchar_t* pages[]={L"Главная",L"Файлы",L"Окна"};float gap=6,pad=8;for(int i=0;i<3;i++){Box b=place.vertical?Box{a.x+pad,a.y+pad+i*(a.h-2*pad+gap)/3,a.w-2*pad,(a.h-2*pad-2*gap)/3}:Box{a.x+pad+i*(a.w-2*pad+gap)/3,a.y+6,(a.w-2*pad-2*gap)/3,a.h-12};if(page==i)gfx->fill(b,data.accent,12,.16f);label(pages[i],b,12,page==i?data.accent:data.muted,page==i,DWRITE_TEXT_ALIGNMENT_CENTER);add(b,[this,i]{page=i;selectedFile=-1;hover=-1;draw();});}}
    else if(id==Controls){const wchar_t* icons[]={L"\uE70F",L"\uE721",L"\uE790",data.mutedAudio?L"\uE74F":L"\uE767",networkGlyph(data.network)};float extent=place.vertical?a.h:a.w,step=(extent-8)/5,side=std::min(44.f,step-4);for(int i=0;i<5;i++){Box b=place.vertical?Box{a.x+(a.w-side)/2,a.y+4+i*step+(step-side)/2,side,side}:Box{a.x+4+i*step+(step-side)/2,a.y+(a.h-side)/2,side,side};iconButton(b,icons[i],[this,i]{if(i==0)beginArrange();if(i==1)parent->invoke(DeskSearch);if(i==2){appearance=!appearance;eqSettings=false;draw();}if(i==3)parent->invoke(DeskToggleMute);if(i==4)openTarget(L"ms-settings:network");});}}
    else if(id==Apps)apps();else if(id==Spectrum)spectrumWidget();else{
        drawK=std::min(a.w/base.w,a.h/base.h);drawX=a.x+(a.w-base.w*drawK)/2-base.x*drawK;drawY=a.y+(a.h-base.h*drawK)/2-base.y*drawK;gfx->transform(D2D1::Matrix3x2F::Scale(drawK,drawK)*D2D1::Matrix3x2F::Translation(drawX,drawY)*rotated);
        if(id==Clock){gfx->shadowEnabled=place.shadow;clock();gfx->shadowEnabled=false;}else if(id==Weather)weather();else if(id==Music)music();else if(id==Tools)notes();else if(id==Files)files();else if(id==Windows)tasks();else if(id==Calendar)calendarWidget();else if(id==Timer){label(L"Таймер",box(1264,521,300,30),18,0,true);timerWidget();}else if(id==System)systemWidget();else if(id==Folders)foldersWidget();else if(id==Image)imageWidget();
    }
    gfx->transform(previous);gfx->endLayer();gfx->transform(previous);left=oldLeft;top=oldTop;drawK=1;drawX=drawY=0;suppressPanel=false;renderingWidget=-1;
}

void Desktop::Impl::View::rotationMenu(int id){POINT p;GetCursorPos(&p);menu=true;int r=customMenu(hwnd,p,{{L"−15°",nullptr,1},{L"+15°",nullptr,2},{L"0°",nullptr,3,layout[id].angle==0},{L"90°",nullptr,4,layout[id].angle==90},{L"180°",nullptr,5,layout[id].angle==180},{L"270°",nullptr,6,layout[id].angle==270}},{data.background,data.hover,data.ink,data.muted,data.accent,data.outline,data.outlineWidth,data.outlineSmooth});menu=false;if(r){float angle=layout[id].angle;layout[id].angle=r==1?std::fmod(angle+345,360):r==2?std::fmod(angle+15,360):(r-3)*90.f;if(!arranging)saveLayout();draw();}}
void Desktop::Impl::View::clockAlignMenu(){POINT p;GetCursorPos(&p);int value=layout[Clock].captionAlign;menu=true;int r=customMenu(hwnd,p,{{L"Как в дизайне",nullptr,1,value==-1},{L"Слева",nullptr,2,value==0},{L"По центру",nullptr,3,value==1},{L"Справа",nullptr,4,value==2}},{data.background,data.hover,data.ink,data.muted,data.accent,data.outline,data.outlineWidth,data.outlineSmooth});menu=false;if(r){layout[Clock].captionAlign=r-2;if(!arranging)saveLayout();draw();}}
void Desktop::Impl::View::musicStyleMenu(){POINT p;GetCursorPos(&p);menu=true;int r=customMenu(hwnd,p,{{L"Карточка",nullptr,1,layout[Music].skin==0},{L"Компактный",nullptr,2,layout[Music].skin==1},{L"Обложка",nullptr,3,layout[Music].skin==2}},{data.background,data.hover,data.ink,data.muted,data.accent,data.outline,data.outlineWidth,data.outlineSmooth});menu=false;if(r){auto old=bounds(Music);layout[Music].skin=r-1;layout[Music].w=layout[Music].h=0;auto base=baseBounds(Music);layout[Music].x=old.x-left-base.x;layout[Music].y=old.y-top-base.y;if(!arranging)saveLayout();draw();}}
void Desktop::Impl::View::imageFitMenu(){POINT p;GetCursorPos(&p);menu=true;int r=customMenu(hwnd,p,{{L"Заполнить",nullptr,1,layout[Image].mediaFit==0},{L"Вписать",nullptr,2,layout[Image].mediaFit==1},{L"Растянуть",nullptr,3,layout[Image].mediaFit==2}},{data.background,data.hover,data.ink,data.muted,data.accent,data.outline,data.outlineWidth,data.outlineSmooth});menu=false;if(r){layout[Image].mediaFit=r-1;if(!arranging)saveLayout();draw();}}
void Desktop::Impl::View::clockCaption(const std::wstring& text,Box b,float size,unsigned color){int align=layout[Clock].captionAlign;if(align<0){label(text,b,size,color);return;}int style=parent->clockStyle;if(style==0||style==1||style==4||style==6||style==7){b.x=14;b.w=1037;}label(text,b,size,color,false,align==0?DWRITE_TEXT_ALIGNMENT_LEADING:align==1?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_TRAILING);}
void Desktop::Impl::View::imageWidget(){auto b=baseBounds(Image);if(imageState.image){float pad=layout[Image].background?12:0;gfx->sprite(imageState.image,{b.x+pad,b.y+pad,b.w-2*pad,b.h-2*pad},layout[Image].mediaFit);}else button({b.x+18,b.y+b.h/2-20,b.w-36,40},L"\uE8B9",imageState.loading?L"Загрузка…":L"Выбрать изображение / GIF",[this]{imageMedia.choose(parent->owner);});if(!imageState.error.empty())label(imageState.error,{b.x+18,b.y+b.h-38,b.w-36,28},11,0xe7a29a);}
void Desktop::Impl::View::resize(){
    RECT r{};GetClientRect(hwnd,&r);scale=std::max(.35f,std::min((r.right-r.left)/1728.f,(r.bottom-r.top)/972.f));width=r.right/scale;height=r.bottom/scale;left=(width-1600)/2;top=(height-840)/2+4;
    gfx=std::make_unique<Graphics>();if(!gfx->init(hwnd,r.right,r.bottom,scale,true)){gfx.reset();return;}
    if(font)DeleteObject(font);if(timerFont)DeleteObject(timerFont);
    font=CreateFontW(-(int)std::lround(15*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    timerFont=CreateFontW(-(int)std::lround(51*scale),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Bahnschrift");
    noteFontScale=timerFontScale=0;if(note)SendMessageW(note,WM_SETFONT,(WPARAM)font,TRUE);if(timer)SendMessageW(timer,WM_SETFONT,(WPARAM)timerFont,TRUE);draw();
}
void Desktop::Impl::View::editors(bool show){
    show=show&&!appearance&&!eqSettings&&!arranging&&(!animation||GetTickCount64()-animation>=620);
    if(!show){if(note)ShowWindow(note,SW_HIDE);if(timer)ShowWindow(timer,SW_HIDE);return;}
    if(!note){
        note=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL,0,0,0,0,hwnd,(HMENU)201,GetModuleHandleW(nullptr),nullptr);SetWindowLongPtrW(note,GWLP_USERDATA,(LONG_PTR)this);noteProc=(WNDPROC)SetWindowLongPtrW(note,GWLP_WNDPROC,(LONG_PTR)inputProc);SendMessageW(note,WM_SETFONT,(WPARAM)font,TRUE);SendMessageW(note,EM_SETLIMITTEXT,100000,0);
        timer=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|ES_CENTER|ES_AUTOHSCROLL,0,0,0,0,hwnd,(HMENU)202,GetModuleHandleW(nullptr),nullptr);SetWindowLongPtrW(timer,GWLP_USERDATA,(LONG_PTR)this);timerProc=(WNDPROC)SetWindowLongPtrW(timer,GWLP_WNDPROC,(LONG_PTR)inputProc);SendMessageW(timer,WM_SETFONT,(WPARAM)timerFont,TRUE);SendMessageW(timer,EM_SETLIMITTEXT,12,0);
        for(HWND input:{note,timer}){SetWindowLongPtrW(input,GWL_EXSTYLE,GetWindowLongPtrW(input,GWL_EXSTYLE)|WS_EX_LAYERED);SetLayeredWindowAttributes(input,0,255,LWA_ALPHA);}
    }

    auto place=[&](HWND input,int id,Box canonical,HFONT& inputFont,float& fontScale,float size,HRGN& savedMask){
        if(!layout[id].shown||!onPage(id)||std::abs(layout[id].angle)>.01f){ShowWindow(input,SW_HIDE);return;}auto b=contentBox(id,canonical);float k=b.w/canonical.w;if(std::abs(fontScale-k)>.001f){fontScale=k;if(inputFont)DeleteObject(inputFont);inputFont=CreateFontW(-(int)std::lround(size*k*scale),0,0,0,id==Timer?FW_BOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,id==Timer?L"Bahnschrift":L"Segoe UI");SendMessageW(input,WM_SETFONT,(WPARAM)inputFont,TRUE);}
        RECT previous{};GetWindowRect(input,&previous);MapWindowPoints(nullptr,hwnd,(POINT*)&previous,2);int x=(int)std::lround(b.x*scale),y=(int)std::lround(b.y*scale),w=(int)std::lround(b.w*scale),h=(int)std::lround(b.h*scale);if(previous.left!=x||previous.top!=y||previous.right-previous.left!=w||previous.bottom-previous.top!=h)SetWindowPos(input,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
        HRGN mask=CreateRectRgn(0,0,w,h);bool above=false;for(int other:order){if(other==id){above=true;continue;}if(!above||!layout[other].shown||!onPage(other))continue;auto o=bounds(other);POINT polygon[4];D2D1_POINT_2F vertices[]={{o.x,o.y},{o.x+o.w,o.y},{o.x+o.w,o.y+o.h},{o.x,o.y+o.h}};for(int j=0;j<4;j++){auto v=rotateWidgetPoint(vertices[j],o,layout[other].angle);polygon[j]={(LONG)std::lround(v.x*scale)-x,(LONG)std::lround(v.y*scale)-y};}HRGN cut=CreatePolygonRgn(polygon,4,WINDING);CombineRgn(mask,mask,cut,RGN_DIFF);DeleteObject(cut);}RECT remaining{};if(GetRgnBox(mask,&remaining)==NULLREGION){DeleteObject(mask);ShowWindow(input,SW_HIDE);return;}if(!savedMask||!EqualRgn(savedMask,mask)){if(savedMask)DeleteObject(savedMask);savedMask=CreateRectRgn(0,0,0,0);CombineRgn(savedMask,mask,nullptr,RGN_COPY);SetWindowRgn(input,mask,TRUE);}else DeleteObject(mask);if(!IsWindowVisible(input))ShowWindow(input,SW_SHOWNOACTIVATE);
    };
    editing=true;if(GetFocus()!=note&&!noteDirty&&textOf(note)!=data.note)SetWindowTextW(note,data.note.c_str());if(GetFocus()!=timer&&textOf(timer)!=formattedTime(data.timerSeconds))SetWindowTextW(timer,formattedTime(data.timerSeconds).c_str());editing=false;
    place(note,Tools,{1268,566,304,202},font,noteFontScale,15,noteMask);ShowScrollBar(note,SB_VERT,FALSE);
    if(!data.timerRunning&&!data.timerPaused)place(timer,Timer,{1268,588,304,60},timerFont,timerFontScale,51,timerMask);else ShowWindow(timer,SW_HIDE);

}
LRESULT CALLBACK Desktop::Impl::View::inputProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){auto v=(View*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(StandaloneMode&&msg==WM_LBUTTONDOWN){SetForegroundWindow(v->parent->owner);SetFocus(hwnd);}
    if(msg==WM_KEYDOWN&&wp=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){SendMessageW(hwnd,EM_SETSEL,0,-1);return 0;}
    if(msg==WM_CHAR&&wp==1)return 0;
    if(hwnd==v->timer&&msg==WM_KEYDOWN&&wp==VK_RETURN){auto value=textOf(hwnd);SetFocus(v->hwnd);v->parent->invoke(DeskTimerToggle,value);return 0;}
    if(hwnd==v->timer&&msg==WM_CHAR&&wp==VK_RETURN)return 0;
    if(msg==WM_CONTEXTMENU){POINT p{};GetCursorPos(&p);v->menu=true;auto& d=v->data;int cmd=customMenu(v->hwnd,p,{{L"Вырезать",L"\uE8C6",1},{L"Копировать",L"\uE8C8",2},{L"Вставить",L"\uE77F",3},{L"Выделить всё",L"\uE8B3",4}},{d.background,d.hover,d.ink,d.muted,d.accent,d.outline,d.outlineWidth,d.outlineSmooth});v->menu=false;SetFocus(hwnd);if(cmd==1)SendMessageW(hwnd,WM_CUT,0,0);if(cmd==2)SendMessageW(hwnd,WM_COPY,0,0);if(cmd==3)SendMessageW(hwnd,WM_PASTE,0,0);if(cmd==4)SendMessageW(hwnd,EM_SETSEL,0,-1);return 0;}
    return CallWindowProcW(hwnd==v->note?v->noteProc:v->timerProc,hwnd,msg,wp,lp);
}
void Desktop::Impl::View::calendarWidget(){
SYSTEMTIME now{};GetLocalTime(&now);

        panel(box(0,95,560,320));int serial=now.wYear*12+now.wMonth-1+calendarMonth,year=serial/12,month=serial%12+1;
        SYSTEMTIME first{};first.wYear=year;first.wMonth=month;first.wDay=1;FILETIME f{};SystemTimeToFileTime(&first,&f);FileTimeToSystemTime(&f,&first);
        wchar_t caption[100]={};GetDateFormatEx(L"ru-RU",0,&first,L"MMMM yyyy",caption,100,nullptr);label(caption,box(28,115,350,34),21,0,true);
        iconButton(box(422,115,34,34),L"\uE76B",[this]{calendarMonth--;draw();});iconButton(box(460,115,34,34),L"\uE76C",[this]{calendarMonth++;draw();});button(box(22,405,150,28),nullptr,L"Сегодня",[this]{calendarMonth=0;draw();});
        const wchar_t* days[]={L"ПН",L"ВТ",L"СР",L"ЧТ",L"ПТ",L"СБ",L"ВС"};for(int i=0;i<7;i++)label(days[i],box(22+i*74,164,68,22),11,data.muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        int offset=(first.wDayOfWeek+6)%7;for(int day=1;day<=monthDays(year,month);day++){int cell=offset+day-1;auto b=box(22+(cell%7)*74,192+(cell/7)*33,68,30);bool today=day==now.wDay&&month==now.wMonth&&year==now.wYear;if(today)gfx->fill(b,blend(data.card,data.accent,.35f),12);label(std::to_wstring(day),b,14,today?data.accent:data.ink,today,DWRITE_TEXT_ALIGNMENT_CENTER);}

}
void Desktop::Impl::View::clock(){
    SYSTEMTIME now{};GetLocalTime(&now);
    wchar_t weekday[100]={},date[100]={},time[16]={};GetDateFormatEx(L"ru-RU",0,&now,L"dddd",weekday,100,nullptr);GetDateFormatEx(L"ru-RU",0,&now,L"d MMMM yyyy",date,100,nullptr);swprintf(time,16,L"%02d:%02d",now.wHour,now.wMinute);
    int style=parent->clockStyle;
    if(style>0){
        auto center=DWRITE_TEXT_ALIGNMENT_CENTER;
        auto dial=[&](float x,float y,float radius,int steps,int selected,unsigned color,float thickness){for(int i=0;i<steps;i++){float a=i*2*Pi/steps-Pi/2;float inner=radius-(i%5?6:12);gfx->line(left+x+std::cos(a)*inner,top+y+std::sin(a)*inner,left+x+std::cos(a)*radius,top+y+std::sin(a)*radius,i<=selected?color:blend(data.background,data.ink,.15f),thickness);}};
        if(style==1){clockCaption(weekday,box(12,106,900,38),24,data.accent);for(int i=0;i<2;i++){auto b=box(10+i*300,158,274,195);gfx->fill(b,data.card,22,.93f);wchar_t part[4];swprintf(part,4,L"%02d",i?now.wMinute:now.wHour);gfx->text(part,b,143,data.ink,true,center,false,L"Bahnschrift");gfx->line(b.x+5,b.y+98,b.x+b.w-5,b.y+98,data.background,3);for(float px:{b.x+4,b.x+b.w-10})gfx->fill({px,b.y+88,6,20},data.accent,3);}clockCaption(date,box(12,372,900,30),20,data.muted);}
        if(style==2){float x=210,y=252,r=144;dial(x,y,r,60,59,data.ink,2);for(int i=1;i<=12;i++){float a=i*Pi/6-Pi/2;label(std::to_wstring(i),box(x+std::cos(a)*111-17,y+std::sin(a)*111-17,34,34),17,data.muted,false,center);}auto hand=[&](float amount,float length,float thickness,unsigned color){float a=amount*2*Pi-Pi/2;gfx->line(left+x,top+y,left+x+std::cos(a)*length,top+y+std::sin(a)*length,color,thickness);};hand((now.wHour%12+now.wMinute/60.f)/12,72,6,data.ink);hand((now.wMinute+now.wSecond/60.f)/60,105,4,data.ink);hand(now.wSecond/60.f,124,1.8f,data.accent);gfx->fill(box(x-5,y-5,10,10),data.accent,5);clockCaption(weekday,box(440,160,600,45),32,data.accent);label(time,box(437,204,620,108),84,0);clockCaption(date,box(440,322,600,34),20,data.muted);}
        if(style==3){dial(234,252,152,60,now.wSecond,data.accent,3);dial(234,252,132,60,now.wMinute,data.ink,5);dial(234,252,108,12,now.wHour%12,data.accent,9);label(time,box(110,210,248,78),52,0,false,center);clockCaption(weekday,box(460,161,600,46),32,data.accent);clockCaption(date,box(460,215,600,40),24);label(std::to_wstring(now.wSecond)+L" сек",box(460,273,600,35),20,data.muted);}
        if(style==4){
            clockCaption(weekday,box(14,110,900,32),23,data.accent);auto b=box(0,157,1070,198);gfx->clip({b.x,b.y,b.w,92});gfx->text(time,b,174,data.ink,true,center,false,L"Bahnschrift");gfx->unclip();gfx->clip({b.x,b.y+103,b.w,b.h-103});gfx->text(time,{b.x+16,b.y,b.w,b.h},174,data.accent,true,center,false,L"Bahnschrift");gfx->unclip();gfx->line(b.x+80,b.y+96,b.x+b.w-64,b.y+96,data.accent,1);clockCaption(date,box(14,371,1000,32),20,data.muted);
        }
        if(style==6){
            clockCaption(weekday,box(14,110,900,32),22,data.accent);gfx->textOutline(time,box(10,156,1045,190),177,data.ink,1.7f);gfx->clip(box(10,251,1050,52));gfx->textOutline(time,box(21,156,1045,190),177,data.accent,2.3f);gfx->unclip();clockCaption(date,box(14,373,900,30),20,data.muted);
        }
        if(style==7){
            const unsigned masks[]={0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};int digits[]={now.wHour/10,now.wHour%10,now.wMinute/10,now.wMinute%10};float start=30;for(int d=0;d<4;d++){float x=left+start+d*190+(d>=2?40:0),y=top+175;float lines[7][4]={{12,0,114,0},{120,8,120,70},{120,86,120,148},{12,156,114,156},{6,86,6,148},{6,8,6,70},{12,78,114,78}};for(int j=0;j<7;j++){bool on=masks[digits[d]]&(1<<j);auto a=lines[j];if(on)gfx->line(x+a[0],y+a[1],x+a[2],y+a[3],blend(data.background,data.accent,.15f),17);gfx->line(x+a[0],y+a[1],x+a[2],y+a[3],on?data.accent:blend(data.background,data.ink,.075f),on?5.f:3.f);}}for(int j=0;j<2;j++)gfx->fill(box(395,223+j*64,8,8),data.ink,4);clockCaption(weekday,box(14,110,900,32),22,data.muted);clockCaption(date,box(14,373,900,30),20,data.muted);
        }
        if(style==8){
            float x=left+224,y=top+255;for(int ring=0;ring<3;ring++){float r=146-ring*34.f;gfx->stroke({x-r,y-r,2*r,2*r},blend(data.background,data.accent,.18f+ring*.08f),r,1.2f);float amount=ring==0?now.wSecond/60.f:ring==1?(now.wMinute+now.wSecond/60.f)/60:(now.wHour%12+now.wMinute/60.f)/12;float a=amount*2*Pi-Pi/2;float px=x+std::cos(a)*r,py=y+std::sin(a)*r;gfx->fill({px-5-ring,py-5-ring,10.f+2*ring,10.f+2*ring},ring==1?data.ink:data.accent,7,1,false);}label(std::to_wstring(now.wDay),box(181,218,86,72),46,data.ink,true,center);clockCaption(weekday,box(448,159,600,43),29,data.accent);label(time,box(442,211,620,104),79,data.ink);clockCaption(date,box(448,330,600,33),19,data.muted);
        }
        if(style==5){auto value=std::wstring(time);gfx->text(value.substr(0,2),box(5,92,300,150),132,data.ink,true,center,false,L"Bahnschrift");gfx->text(value.substr(3),box(5,243,300,150),132,data.accent,true,center,false,L"Bahnschrift");gfx->fill(box(340,136,3,235),data.accent,1.5f);clockCaption(weekday,box(395,180,660,50),34,data.accent);clockCaption(date,box(395,242,660,44),25);}
        return;
    }
    clockCaption(weekday,box(10,112,740,38),26,data.accent);gfx->text(time,box(-2,155,865,202),183,data.ink,false,DWRITE_TEXT_ALIGNMENT_LEADING,false,L"Bahnschrift");
    clockCaption(date,box(10,357,740,34),21,data.ink);float cx=left+964,cy=top+252;
    for(int i=0;i<60;i++){float angle=i*Pi/30-Pi/2,inner=i%5?48.f:44.f;gfx->line(cx+std::cos(angle)*inner,cy+std::sin(angle)*inner,cx+std::cos(angle)*52,cy+std::sin(angle)*52,i<=now.wSecond?data.accent:blend(data.background,data.ink,.22f),i%5?1.4f:2.4f);}
    wchar_t seconds[4];swprintf(seconds,4,L"%02d",now.wSecond);label(seconds,box(925,230,78,46),30,0,false,DWRITE_TEXT_ALIGNMENT_CENTER);
}
void Desktop::Impl::View::apps(){
    auto a=renderBox;bool vertical=layout[Apps].vertical;float extent=vertical?a.h:a.w,cross=vertical?a.w:a.h;int cap=std::clamp((int)((extent-118)/56),1,12),total=(int)data.pinned.size();appsPerPage=cap;appPage=std::clamp(appPage,0,std::max(0,(total-1)/cap));int count=std::min(cap,total-appPage*cap);float control=std::min(44.f,cross-16),step=count?std::min(66.f,(extent-118)/count):0;auto slot=[&](float along,float size){return vertical?Box{a.x+(a.w-size)/2,a.y+along,size,size}:Box{a.x+along,a.y+(a.h-size)/2,size,size};};dockControl(slot(12,control),false,[this]{parent->invoke(DeskApps);});
    for(int i=0;i<count;i++){int index=appPage*cap+i;auto path=data.pinned[index];auto found=catalog->apps.find(path);auto icon=found==catalog->apps.end()?nullptr:found->second.icon;if(!icon&&data.tasks){auto it=data.tasks->shortcuts.find(path);if(it!=data.tasks->shortcuts.end())icon=it->second;}float tile=std::min(58.f,cross-12);auto b=slot(65+i*step+(step-tile)/2,tile);bool over=hover==(int)hits.size();if(over)gfx->fill(b,data.hover,15,.85f);float size=std::min(40.f,tile-10);if(icon)gfx->pixels(icon,{b.x+(b.w-size)/2,b.y+(b.h-size)/2,size,size});else gfx->icon(L"\uE71D",b,26,data.accent);bool running=false;if(found!=catalog->apps.end()&&data.tasks)for(auto& group:data.tasks->groups)if(!_wcsicmp(group.path.c_str(),found->second.target.c_str()))running=true;if(running){if(vertical)gfx->fill({b.x+b.w-1,b.y+b.h/2-6,3,12},data.accent,1.5f);else gfx->fill({b.x+b.w/2-6,b.y+b.h-1,12,3},data.accent,1.5f);}add(b,[this,path]{openPinnedTarget(path,data.tasks);},1,index,path);}
    dockControl(slot(extent-control-12,control),true,[this]{parent->invoke(DeskAddApp);});
}
void Desktop::Impl::View::weather(){
    panel(box(1240,86,360,172));auto& w=data.weather;
    if(!w.available){button(box(1262,144,316,44),L"\uE707",L"Выбрать город",[this]{parent->invoke(DeskWeather);});return;}
    label(w.city,box(1264,102,274,25),12,data.muted);iconButton(box(1548,99,30,30),L"\uE70F",[this]{parent->invoke(DeskWeather);});
    label(std::to_wstring((int)std::round(w.temperature))+L"°",box(1260,128,200,76),59,0,false);gfx->icon(weatherIcon(w.code),box(1500,134,66,66),44,data.accent);
    label(weatherText(w.code),box(1264,207,180,26),13,data.accent);label(std::to_wstring((int)std::round(w.wind))+L" км/ч · "+std::to_wstring(w.humidity)+L"%",box(1451,207,123,26),11,data.muted,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
}
void Desktop::Impl::View::music(){
    auto base=baseBounds(Music);auto& m=data.media;int skin=layout[Music].skin;Box cover,title,artist,previous,play,next;
    if(skin==1){cover={18,16,76,76};title={108,12,328,29};artist={108,44,328,24};previous={266,75,36,36};play={316,71,44,44};next={374,75,36,36};}
    else if(skin==2){cover={18,18,284,240};title={18,269,284,29};artist={18,302,284,24};previous={76,359,36,36};play={138,353,44,44};next={208,359,36,36};}
    else{cover={23,22,63,72};title={104,22,229,34};artist={104,59,229,26};previous={70,142,44,44};play={156,137,48,48};next={246,142,44,44};}
    auto at=[&](Box b){b.x+=base.x;b.y+=base.y;return b;};cover=at(cover);title=at(title);artist=at(artist);previous=at(previous);play=at(play);next=at(next);
    if(m.cover&&!m.cover->empty())gfx->image(*m.cover,m.revision,cover);else gfx->icon(L"\uE8D6",cover,31,data.accent);
    label(m.available?m.title:L"Ничего не играет",title,skin==2?18:16,0,true);label(m.available?m.artist:L"",artist,12,data.muted);
    auto seek=musicSeek();float progress=seekDragging?seekRatio:m.duration>0?(float)std::clamp(m.position/m.duration,0.,1.):0;gfx->fill(seek,blend(data.card,data.muted,.25f),1.5f);gfx->fill({seek.x,seek.y,seek.w*progress,seek.h},data.accent,1.5f);if((musicOver||seekDragging)&&m.duration>0)gfx->fill({seek.x+seek.w*progress-4,seek.y-3,8,8},data.ink,4);
    if(m.available&&m.duration>0)add({seek.x,seek.y-10,seek.w,23},[this]{POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);parent->invoke(101,std::to_wstring(data.media.duration*musicRatio(p.x/scale,p.y/scale)));},8);
    iconButton(previous,L"\uE892",[this]{parent->invoke(100,L"2");});gfx->fill(play,data.accent,play.w/2,1,false);gfx->icon(m.playing?L"\uE769":L"\uE768",play,17,data.background);add(play,[this]{parent->invoke(100,L"1");});iconButton(next,L"\uE893",[this]{parent->invoke(100,L"3");});
}
void Desktop::Impl::View::spectrumWidget(){
    auto b=renderBox;label(L"ЗВУК",{b.x+14,b.y+8,b.w-28,22},10.5f,data.accent,true);float x=b.x+14,y=b.y+40,w=b.w-28,h=std::max(30.f,b.h-69);int n=eqBands,mode=eqMode;auto value=[&](int i){int first=i*48/n,last=(i+1)*48/n;float v=0;for(int j=first;j<last;j++)v=std::max(v,levels[j]);return std::clamp(v*eqGain,0.f,1.f);};float step=w/n;
    if(mode==0||mode==3||mode==5){for(int i=0;i<n;i++){float v=value(i),bar=std::max(2.f,v*h);if(mode==3){gfx->fill({x+i*step,y+h/2-bar/2,std::max(2.f,step-4),bar},data.accent,3,.9f);}else if(mode==5){int dots=std::max(1,(int)(v*12));for(int j=0;j<dots;j++)gfx->fill({x+i*step,y+h-(j+1)*h/12,std::max(2.f,step-5),std::max(2.f,h/12-4)},blend(data.accent,data.ink,j/24.f),2,.9f);}else{gfx->fill({x+i*step,y+h-bar,std::max(2.f,step-5),bar},blend(data.accent,data.ink,i/(float)n*.35f),3,.9f);if(eqPeaks){float peak=peaks[i*48/n];if(peak>.025f)gfx->fill({x+i*step,y+h-std::clamp(peak*eqGain,0.f,1.f)*h-3,std::max(2.f,step-5),2},data.ink,1,.6f);}}}}
    else if(mode==1||mode==6){float previous=y+h/2;for(int i=0;i<128;i++){float v=std::clamp(spectrum.wave[i]*eqGain*2,-1.f,1.f),next=y+h/2-v*h*.48f;if(i){gfx->line(x+(i-1)*w/127,previous,x+i*w/127,next,data.accent,mode==6?3:1.8f);if(mode==6)gfx->line(x+(i-1)*w/127,y+h-previous+y,x+i*w/127,y+h-next+y,blend(data.accent,data.ink,.45f),1.2f);}previous=next;}}
    else if(mode==4){float cx=x+w/2,cy=y+h/2,r=std::min(w,h)*.24f;for(int i=0;i<n;i++){float angle=i*2*Pi/n-Pi/2,outer=r+value(i)*r;gfx->line(cx+std::cos(angle)*r,cy+std::sin(angle)*r,cx+std::cos(angle)*outer,cy+std::sin(angle)*outer,data.accent,std::max(2.f,r/24));}}
    else if(mode==7){float rms=std::clamp(spectrum.rms*eqGain*4,0.f,1.f);int segments=24;for(int i=0;i<segments;i++){float sx=x+i*w/segments;gfx->fill({sx,y+h*.28f,w/segments-4,h*.44f},i<rms*segments?(i>19?0xdf867c:i>14?0xd1bd85:data.accent):blend(data.background,data.ink,.10f),3);}}
    else for(int i=1;i<n;i++)gfx->line(x+(i-1)*w/(n-1),y+h-value(i-1)*h,x+i*w/(n-1),y+h-value(i)*h,data.accent,2.2f);
    if(eqNumbers&&b.w>450){const wchar_t* ticks[]={L"32 Гц",L"125",L"500",L"2 кГц",L"8",L"18"};for(int i=0;i<6;i++)label(ticks[i],{x+i*(w-40)/5,b.y+b.h-24,42,18},10,data.muted);}
}
void Desktop::Impl::View::notes(){label(L"Заметка",box(1264,521,270,30),18,0,true);gfx->fill(box(1260,558,320,220),inputColor(),12);if(std::abs(layout[Tools].angle)>.01f){gfx->text(data.note,box(1268,566,304,202),15,data.ink,false,DWRITE_TEXT_ALIGNMENT_LEADING,true,L"Segoe UI",DWRITE_PARAGRAPH_ALIGNMENT_NEAR);add(box(1260,558,320,220),[this]{parent->invoke(DeskNote);});}iconButton(box(1549,517,29,30),L"\uE8A7",[this]{parent->invoke(DeskNote);});}

void Desktop::Impl::View::timerWidget(){
    gfx->fill(box(1260,578,320,78),inputColor(),14);if(std::abs(layout[Timer].angle)>.01f&&!data.timerRunning&&!data.timerPaused)add(box(1260,578,320,78),[this]{parent->invoke(DeskNote);});
    if(data.timerRunning||data.timerPaused||!timer||!IsWindowVisible(timer))label(formattedTime(data.timerSeconds),box(1268,578,304,78),51,0,true,DWRITE_TEXT_ALIGNMENT_CENTER);
    int seconds[]={300,900,1500};for(int i=0;i<3;i++)button(box(1264+i*106,663,100,31),nullptr,std::to_wstring(seconds[i]/60)+L" мин",[this,seconds=seconds[i]]{parent->invoke(DeskSetTimer,std::to_wstring(seconds/60));});
    button(box(1264,722,148,44),data.timerRunning?L"\uE769":L"\uE768",data.timerRunning?L"Пауза":L"Начать",[this]{parent->invoke(DeskTimerToggle,timer&&!data.timerRunning&&!data.timerPaused?textOf(timer):L"");},data.timerRunning);button(box(1428,722,148,44),L"\uE777",L"Сбросить",[this]{parent->invoke(DeskTimerReset);});
}
void Desktop::Impl::View::files(){
    panel(box(0,86,1130,600));label(L"Файлы",box(28,105,650,45),28,0,true);iconButton(box(914,110,38,38),L"\uE8B7",[]{openTarget(knownFolder(FOLDERID_Desktop));});iconButton(box(960,110,38,38),L"\uE710",[this]{auto path=chooseFile(hwnd,L"Добавить файл");if(!path.empty())parent->addFile(path);});iconButton(box(1010,110,32,38),L"\uE76B",[this]{filePage=std::max(0,filePage-1);draw();});iconButton(box(1054,110,32,38),L"\uE76C",[this]{filePage=std::min(std::max(0,((int)catalog->files.size()-1)/21),filePage+1);draw();});
    filePage=std::clamp(filePage,0,std::max(0,((int)catalog->files.size()-1)/21));
    for(int slot=0;slot<21;slot++){int index=filePage*21+slot;if(index>=(int)catalog->files.size())break;auto entry=catalog->files[index];Box b=box(26+(slot%7)*154,182+(slot/7)*152,146,138);if(selectedFile==index||hover==(int)hits.size())gfx->fill(b,data.hover,18,.8f);gfx->pixels(entry.icon,{b.x+45,b.y+14,56,56});if(!entry.icon)gfx->icon(entry.directory?L"\uE8B7":L"\uE8A5",{b.x+45,b.y+14,56,56},36,data.accent);gfx->text(entry.name,{b.x+8,b.y+82,b.w-16,45},12,data.ink,false,DWRITE_TEXT_ALIGNMENT_CENTER,true,L"Bahnschrift",DWRITE_PARAGRAPH_ALIGNMENT_NEAR);add(b,[this,index]{selectedFile=index;draw();},2,index,entry.path);}
    if(catalog->files.empty()){gfx->icon(L"\uE8B7",box(460,275,180,85),44,data.muted);label(L"Нет файлов",box(100,390,930,36),19,data.muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);}
}
void Desktop::Impl::View::tasks(){
    label(L"Открытые приложения",box(10,100,900,52),30,0,true);int count=data.tasks?(int)data.tasks->groups.size():0;taskPage=std::clamp(taskPage,0,std::max(0,(count-1)/9));iconButton(box(1035,107,32,38),L"\uE76B",[this]{taskPage=std::max(0,taskPage-1);draw();});iconButton(box(1080,107,32,38),L"\uE76C",[this,count]{taskPage=std::min(std::max(0,(count-1)/9),taskPage+1);draw();});
    for(int i=0;i<9;i++){int index=taskPage*9+i;if(index>=count)break;auto group=data.tasks->groups[index];Box b=box((i%3)*380,181+(i/3)*163,364,146);panel(b);if(hover==(int)hits.size())gfx->fill(b,data.hover,25,.5f);gfx->pixels(group.icon,{b.x+23,b.y+21,46,46});if(!group.icon)gfx->icon(L"\uE737",{b.x+23,b.y+21,46,46},30,data.accent);label(group.name,{b.x+86,b.y+22,248,30},18,0,true);label(std::to_wstring(group.windows.size()),{b.x+305,b.y+58,28,25},12,data.accent,false,DWRITE_TEXT_ALIGNMENT_TRAILING);label(group.windows.empty()?L"":group.windows.front().title,{b.x+23,b.y+94,310,27},12,data.muted);
        add(b,[this,index]{if(!data.tasks||index>=(int)data.tasks->groups.size())return;auto group=data.tasks->groups[index];if(group.windows.size()==1){activateTaskWindow(group.windows.front());return;}std::vector<MenuItem> items;for(size_t i=0;i<group.windows.size()&&i<16;i++)items.push_back({group.windows[i].title,L"\uE737",(int)i+1});POINT p;GetCursorPos(&p);menu=true;int choice=customMenu(hwnd,p,items,{data.background,data.hover,data.ink,data.muted,data.accent,data.outline,data.outlineWidth,data.outlineSmooth});menu=false;if(choice>0&&choice<=(int)group.windows.size())activateTaskWindow(group.windows[choice-1]);},4,index);
    }
    if(!count)label(L"Нет открытых приложений",box(10,300,1100,70),22,data.muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Desktop::Impl::View::systemWidget(){
    MEMORYSTATUSEX mem{sizeof(mem)};GlobalMemoryStatusEx(&mem);label(L"Система",box(704,116,310,32),20,0,true);auto metric=[&](const wchar_t* name,float value,float y){label(name,box(704,y,200,25),12,data.muted);label(std::to_wstring((int)std::round(value))+L"%",box(920,y,94,25),17,data.ink,true,DWRITE_TEXT_ALIGNMENT_TRAILING);gfx->fill(box(704,y+36,310,5),data.hover,2);gfx->fill(box(704,y+36,310*value/100,5),data.accent,2);};metric(L"Процессор",parent->cpuUsage,160);metric(L"Память",(float)mem.dwMemoryLoad,222);wchar_t memory[100];swprintf(memory,100,L"%.1f / %.1f ГБ",(mem.ullTotalPhys-mem.ullAvailPhys)/1073741824.,mem.ullTotalPhys/1073741824.);label(memory,box(704,286,310,24),12,data.muted);
}
void Desktop::Impl::View::foldersWidget(){
    label(L"Папки",box(704,386,300,32),20,0,true);const wchar_t* names[]={L"Загрузки",L"Документы",L"Изображения",L"Рабочий стол"};const KNOWNFOLDERID* ids[]={&FOLDERID_Downloads,&FOLDERID_Documents,&FOLDERID_Pictures,&FOLDERID_Desktop};for(int i=0;i<4;i++)button(box(700+(i%2)*160,438+(i/2)*58,148,46),L"\uE8B7",names[i],[id=ids[i]]{openTarget(knownFolder(*id));});
}
void Desktop::Impl::View::eqControls(){
    float x=(width-454)/2,y=std::max(50.f,(height-748)/2);auto panelBox=Box{x,y,454,748};gfx->fill(panelBox,data.background,22,.99f);label(L"Эквалайзер",{x+24,y+14,350,38},22,0,true);iconButton({x+396,y+17,34,34},L"\uE711",[this]{eqSettings=false;draw();});
    bool enabled=eqEnabled&&layout[Spectrum].shown;
    const wchar_t* modes[]={L"Столбцы",L"Волна",L"Линия",L"Зеркало",L"Радиальный",L"Матрица",L"Лента",L"Индикатор"};for(int i=0;i<8;i++)button({x+22+(i%4)*104,y+70+(i/4)*42,98,34},nullptr,modes[i],[this,i]{eqMode=i;saveEQ();draw();},eqMode==i,enabled);
    auto choices=[&](float py,const wchar_t* caption,std::initializer_list<int> values,int current,std::function<void(int)> action,const wchar_t* suffix){label(caption,{x+24,y+py,400,25},12,data.muted);float px=x+22;for(int n:values){button({px,y+py+33,96,34},nullptr,std::to_wstring(n)+suffix,[action,n]{action(n);},current==n,enabled);px+=104;}};
    choices(169,L"Предел частоты",{10,20,30,60},eqFPS,[this](int n){eqFPS=n;saveEQ();draw();},L" Гц");choices(255,L"Полосы",{12,24,48},eqBands,[this](int n){eqBands=n;saveEQ();draw();},L"");choices(341,L"Усиление",{50,100,150,200},(int)(eqGain*100),[this](int n){eqGain=n/100.f;saveEQ();draw();},L"%");
    auto toggle=[&](float py,const wchar_t* name,bool value,std::function<void()> action,bool active){button({x+22,y+py,410,37},value?L"\uE73D":L"\uE739",name,std::move(action),value,active);};toggle(439,L"Включён",eqEnabled,[this]{eqEnabled=!eqEnabled;saveEQ();draw();},layout[Spectrum].shown);toggle(486,L"Плавное движение",eqSmooth,[this]{eqSmooth=!eqSmooth;saveEQ();draw();},enabled);toggle(533,L"Пики",eqPeaks,[this]{eqPeaks=!eqPeaks;saveEQ();draw();},enabled);
    toggle(580,L"Частоты",eqNumbers,[this]{eqNumbers=!eqNumbers;saveEQ();draw();},enabled);
    button({x+22,y+627,199,38},nullptr,L"Экономичный",[this]{eqFPS=10;eqBands=12;eqPeaks=false;saveEQ();draw();},false,enabled);button({x+233,y+627,199,38},nullptr,L"Обычный",[this]{eqFPS=30;eqBands=48;eqPeaks=true;saveEQ();draw();},false,enabled);button({x+22,y+681,410,34},L"\uE73E",L"Готово",[this]{eqSettings=false;draw();});add(panelBox,[]{},-1);
}
void Desktop::Impl::View::background(){
    unsigned base[]={0x0a1420,0x1d1117,0x11161b},end[]={0x123437,0x44302c,0x29343a},tint[]={0x66bcae,0xe1a77d,0xa6b9c4};int preset=parent->preset;
    gfx->fill({0,0,width,height},base[preset]);
    if(photo&&wallpaper)gfx->picture(wallpaper,{0,0,width,height},parent->placement);
    else {gfx->releaseWallpaper();gfx->gradient({0,0,width,height},base[preset],end[preset],false);gfx->glow({width*.2f,-height*.5f,width*1.1f,height*1.4f},preset==0?0x3d5b87:tint[preset],.38f);gfx->glow({width*.35f,height*.24f,width*.8f,height*.9f},tint[preset],.25f);for(int i=0;i<5;i++)gfx->ribbon({-100.f-i*20,height*.05f+i*34,width*1.15f,height*.92f},tint[preset],.045f+i*.017f);}
    gfx->fill({0,0,width,height},0,0,parent->shade/100.f);
    gfx->gradient({0,0,width,height},0,0,false,.24f,0);
    gfx->line(width/2,36,width/2,std::max(36.f,top-16),blend(base[preset],data.accent,.45f),1);
}
void Desktop::Impl::View::wallpaperControls(){
    panel(box(1190,86,410,716));label(L"Оформление",box(1216,105,310,43),23,0,true);iconButton(box(1501,108,34,34),L"\uE790",[this]{parent->invoke(DeskAppearance);});iconButton(box(1542,108,34,34),L"\uE711",[this]{appearance=false;draw();});
    const wchar_t* names[]={L"Сумерки",L"Дюны",L"Графит"};unsigned bases[]={0x0b2334,0x382329,0x202d33},ends[]={0x3e7e7d,0xc59468,0x7b8b91};
    for(int i=0;i<3;i++){Box b=box(1214+i*123,168,112,102);gfx->fill(b,data.card,16);gfx->clipRounded({b.x+4,b.y+4,b.w-8,62},12);gfx->gradient({b.x+4,b.y+4,b.w-8,62},bases[i],ends[i],false);gfx->ribbon({b.x,b.y-10,b.w,78},data.ink,.15f);gfx->unclipRounded();if(!photo&&parent->preset==i)gfx->stroke(b,data.accent,16,1.5f);label(names[i],{b.x,b.y+70,b.w,26},11,0,false,DWRITE_TEXT_ALIGNMENT_CENTER);add(b,[this,i]{parent->selectPreset(i);draw();});}
    button(box(1214,292,174,43),L"\uE8B9",L"Изображение",[this]{parent->selectWallpaper();draw();});button(box(1400,292,174,43),L"\uE714",L"Видео",[this]{parent->selectWallpaper(true);draw();},false,data.performance.video);
    label(wallLoading?L"Загрузка…":!wallError.empty()?wallError:photo?wallName:L"",box(1216,340,358,30),11,wallError.empty()?data.muted:0xe7a29a);
    label(L"Масштаб",box(1216,390,350,28),13,data.muted);const wchar_t* modes[]={L"Заполнить",L"Вписать",L"Растянуть"};for(int i=0;i<3;i++)button(box(1214+i*123,429,112,36),nullptr,modes[i],[this,i]{parent->placement=i;parent->saveAppearance();draw();},parent->placement==i,photo);
    label(L"Затемнение",box(1216,501,240,28),13,data.muted);label(std::to_wstring(parent->shade)+L"%",box(1487,501,86,28),13,data.ink,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
    Box slider=box(1220,548,347,26);gfx->fill({slider.x,slider.y+11,slider.w,4},data.hover,2);gfx->fill({slider.x,slider.y+11,slider.w*parent->shade/75,4},data.accent,2);gfx->fill({slider.x+slider.w*parent->shade/75-6,slider.y+7,12,12},data.ink,6);add(slider,[this,slider]{POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);parent->shade=std::clamp((int)std::lround((p.x/scale-slider.x)/slider.w*75),0,75);parent->saveAppearance();draw();},6);
    label(L"Дизайн часов",box(1216,596,210,26),13,data.muted);button(box(1440,590,134,32),layout[Clock].shadow?L"\uE73D":L"\uE739",L"Тень",[this]{layout[Clock].shadow=!layout[Clock].shadow;saveLayout();draw();},layout[Clock].shadow,layout[Clock].shown);
    const wchar_t* styles[]={L"Цифры",L"Перекидные",L"Стрелки",L"Кольца",L"Разрез",L"Вертикальные",L"Контур",L"Неон",L"Орбита"};for(int i=0;i<9;i++)button(box(1214+(i%3)*123,633+(i/3)*37,112,31),nullptr,styles[i],[this,i]{parent->clockStyle=i;WritePrivateProfileStringW(L"desktop",L"clock-style",std::to_wstring(i).c_str(),parent->ini.c_str());draw();},parent->clockStyle==i,layout[Clock].shown);
    button(box(1214,757,174,31),nullptr,L"Дата",[this]{clockAlignMenu();},false,layout[Clock].shown);button(box(1400,757,174,31),L"\uE73E",L"Готово",[this]{appearance=false;draw();});
}

void Desktop::Impl::View::draw(){
    if(!gfx||!IsWindow(hwnd)||(lastDraw&&!visible&&!arranging))return;
    SetPropW(hwnd,L"OwnIsland.Editing",(HANDLE)(UINT_PTR)arranging);SetPropW(hwnd,L"OwnIsland.ClockShadow",(HANDLE)(UINT_PTR)layout[Clock].shadow);
    for(int id=0;id<WidgetCount;id++){auto b=bounds(id);auto prefix=L"OwnIsland.Widget"+std::to_wstring(id);SetPropW(hwnd,(prefix+L"X").c_str(),(HANDLE)(INT_PTR)(int)b.x);SetPropW(hwnd,(prefix+L"Y").c_str(),(HANDLE)(INT_PTR)(int)b.y);SetPropW(hwnd,(prefix+L"Visible").c_str(),(HANDLE)(UINT_PTR)layout[id].shown);SetPropW(hwnd,(prefix+L"W").c_str(),(HANDLE)(INT_PTR)(int)b.w);SetPropW(hwnd,(prefix+L"H").c_str(),(HANDLE)(INT_PTR)(int)b.h);SetPropW(hwnd,(prefix+L"Vertical").c_str(),(HANDLE)(UINT_PTR)layout[id].vertical);SetPropW(hwnd,(prefix+L"Angle").c_str(),(HANDLE)(INT_PTR)(int)layout[id].angle);}

    auto stamp=GetTickCount64();data=parent->snapshot();{std::lock_guard lock(parent->mutex);catalog=parent->catalog;}auto media=parent->media.snapshot();wallpaper=media.image;photo=media.enabled;wallLoading=media.loading;motion=media.enabled&&media.video;wallError=media.error;wallName=media.name;imageState=imageMedia.snapshot();
    float elapsed=animation?std::clamp((stamp-animation)/620.f,0.f,1.f):1,ease=1-std::pow(1-elapsed,4.f);bool animating=elapsed<1;
    gfx->outlineColor=data.outline;gfx->outlineWidth=data.outlineWidth;gfx->outlineSmooth=data.outlineSmooth;gfx->begin();hits.clear();background();
    float revealWidth=240+(width-240)*ease;gfx->clip({(width-revealWidth)/2,0,revealWidth,36+(height-36)*ease});gfx->layer(ease,0,-24*(1-ease));
    if(arranging){for(float x=0;x<width;x+=grid)gfx->line(x,42,x,height,data.accent,.12f);for(float y=0;y<height;y+=grid)if(y>=42)gfx->line(0,y,width,y,data.accent,.12f);}
    for(int id:order)if(layout[id].shown&&onPage(id)&&!(appearance&&(id==Weather||id==Music||id==Tools)))renderWidget(id);
    renderingWidget=-2;if(appearance)wallpaperControls();
    if(arranging){
        for(int id:order)if(layout[id].shown&&onPage(id)){auto b=bounds(id);auto frameTransform=gfx->transform();gfx->transform(D2D1::Matrix3x2F::Rotation(layout[id].angle,{b.x+b.w/2,b.y+b.h/2})*frameTransform);gfx->stroke(b,id==selectedWidget?data.accent:blend(data.background,data.accent,.48f),12,id==selectedWidget?2:1);gfx->transform(frameTransform);auto title=titleBox(id);gfx->fill(title,data.background,7,.92f,false);label(widgetName(id),{title.x+9,title.y,title.w-18,title.h},11,data.ink);auto close=closeBox(id);gfx->fill(close,data.background,7,.92f,false);gfx->icon(L"\uE711",close,10,data.ink);}
        if(selectedWidget>=0&&layout[selectedWidget].shown&&onPage(selectedWidget)){auto b=bounds(selectedWidget);float px[]={b.x,b.x+b.w/2,b.x+b.w,b.x+b.w,b.x+b.w,b.x+b.w/2,b.x,b.x},py[]={b.y,b.y,b.y,b.y+b.h/2,b.y+b.h,b.y+b.h,b.y+b.h,b.y+b.h/2};for(int i=0;i<8;i++){auto p=rotateWidgetPoint({px[i],py[i]},b,layout[selectedWidget].angle);gfx->fill({p.x-4,p.y-4,8,8},data.accent,3,1,false);}}
        arrangeControls();
    }
    if(eqSettings)eqControls();renderingWidget=-1;
    gfx->endLayer();gfx->unclip();
    bool ok=gfx->end();lastDraw=stamp;SetPropW(hwnd,L"OwnIsland.DesktopFrames",(HANDLE)((UINT_PTR)GetPropW(hwnd,L"OwnIsland.DesktopFrames")+1));
    if(!ok){gfx.reset();PostMessageW(hwnd,WM_APP+20,0,0);return;}
    unsigned card=inputColor();COLORREF color=RGB((card>>16)&255,(card>>8)&255,card&255);LOGBRUSH current{};if(!brush||!GetObjectW(brush,sizeof(current),&current)||current.lbColor!=color){if(brush)DeleteObject(brush);brush=CreateSolidBrush(color);if(note)InvalidateRect(note,nullptr,TRUE);if(timer)InvalidateRect(timer,nullptr,TRUE);}
    editors(visible&&!animating);
}
void Desktop::Impl::View::pulse(bool force){
    auto stamp=GetTickCount64();data=parent->snapshot();
    if(force||stamp-lastVisibility>=500){bool now=data.awake&&IsWindowVisible(hwnd)&&!monitorBlocked(monitor)&&exposed(monitor);visible=now;lastVisibility=stamp;SetPropW(hwnd,L"OwnIsland.DesktopPaused",(HANDLE)(UINT_PTR)(visible?0:1));}
    imageMedia.setActive(visible&&page==0&&layout[Image].shown&&data.performance.video,data.performance.desktopFPS);if(!visible){if(timerInterval!=250){timerInterval=250;SetTimer(hwnd,1,timerInterval,nullptr);}editors(false);return;}
    bool animating=animation&&stamp-animation<620;
    if(parent->audio)spectrum=parent->audio->spectrum();if(!eqEnabled||!layout[Spectrum].shown)spectrum={};if(layout[System].shown)parent->stats();float dt=lastFrame?std::clamp((stamp-lastFrame)/1000.f,0.f,.1f):.016f;lastFrame=stamp;
    bool changing=false;for(int i=0;i<48;i++){float target=spectrum.bars[i];float old=levels[i];levels[i]=eqSmooth?levels[i]+(target-levels[i])*(1-std::exp(-dt*(target>levels[i]?25:7))):target;peaks[i]=std::max(levels[i],peaks[i]-dt*.35f);changing|=std::abs(old-levels[i])>.0002f;}
    imageState=imageMedia.snapshot();motion=data.performance.video&&(parent->media.snapshot().video||(layout[Image].shown&&imageState.animated));int fps=std::min(data.performance.desktopFPS,eqFPS);int interval=animating?16:motion?1000/data.performance.desktopFPS:changing||spectrum.rms>.00015f?1000/fps:100;if(interval!=timerInterval){timerInterval=interval;SetTimer(hwnd,1,timerInterval,nullptr);}
    auto seconds=stamp/1000;bool tick=clockTick!=seconds;clockTick=seconds;
    if((force||animating||motion||changing||spectrum.rms>.00015f||tick)&&(force||animating||stamp-lastDraw>=(ULONGLONG)(1000/(motion?data.performance.desktopFPS:fps))))draw();
}
void Desktop::Impl::View::dragFile(const std::wstring& path){
    PIDLIST_ABSOLUTE id=nullptr;if(FAILED(SHParseDisplayName(path.c_str(),nullptr,&id,0,nullptr)))return;
    Com<IShellFolder> folder;PCUITEMID_CHILD child=nullptr;Com<IDataObject> object;
    if(SUCCEEDED(SHBindToParent(id,IID_PPV_ARGS(folder.put()),&child))&&SUCCEEDED(folder->GetUIObjectOf(hwnd,1,&child,IID_IDataObject,nullptr,(void**)object.put()))){DWORD effect=0;SHDoDragDrop(hwnd,object.p,nullptr,DROPEFFECT_COPY|DROPEFFECT_LINK,&effect);}CoTaskMemFree(id);
}
void Desktop::Impl::View::context(POINT position){
    POINT local=position;ScreenToClient(hwnd,&local);float x=local.x/scale,y=local.y/scale;if(arranging){int id=widgetAt(x,y);if(id>=0)widgetSettings(id);else widgetMenu();return;}if(widgetAt(x,y)==Spectrum)return;Hit hit{};int hitIndex=hitAt(x,y);bool found=hitIndex>=0;if(found)hit=hits[hitIndex];
    std::vector<MenuItem> items;std::wstring path;
    if(found&&(hit.kind==1||hit.kind==2)){
        path=hit.path;items={{L"Открыть",L"\uE8A5",1},{L"Расположение",L"\uE8B7",2},{L"Копировать путь",L"\uE8C8",3}};
        if(hit.kind==1){items.insert(items.begin()+1,{L"От имени администратора",L"\uE7EF",4});items.push_back({L"Открепить",L"\uE77A",5});}
        else{items.push_back({L"Закрепить в приложениях",L"\uE718",6});if(std::find(parent->extraFiles.begin(),parent->extraFiles.end(),path)!=parent->extraFiles.end())items.push_back({L"Убрать из файлов",L"\uE77A",7});}
    }else if(found&&hit.kind==3){items={{L"Столбцы",nullptr,20,eqMode==0},{L"Волна",nullptr,21,eqMode==1},{L"Линия",nullptr,22,eqMode==2},{L"Чувствительность ×1",nullptr,23,eqGain==1},{L"Чувствительность ×1,5",nullptr,24,eqGain>1}};}
    else items={{L"Редактировать рабочий стол",L"\uE70F",17},{L"Поиск",L"\uE721",10},{L"Добавить приложение",L"\uE710",11},{L"Обои и оформление",L"\uE790",16},{StandaloneMode?L"Файлы рабочего стола":L"Рабочий стол в Проводнике",L"\uE8B7",12},{L"Обновить",L"\uE72C",13},{L"Настройки панели",L"\uE713",14},{L"Диспетчер задач",L"\uE9D9",15}};
    menu=true;int result=customMenu(hwnd,position,items,{data.background,data.hover,data.ink,data.muted,data.accent,data.outline,data.outlineWidth,data.outlineSmooth});menu=false;
    if(result==1){if(hit.kind==1)openPinnedTarget(path,data.tasks);else openTarget(path);}if(result==2)folderLocation(path);if(result==3)setClipboard(hwnd,path);
    if(result==4){SearchResult item;item.path=path;item.target=path;item.administrator=true;Com<IShellLinkW> link;Com<IPersistFile> file;auto ext=std::filesystem::path(path).extension().wstring();if(!_wcsicmp(ext.c_str(),L".lnk")&&SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put())))&&SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(file.put())))&&SUCCEEDED(file->Load(path.c_str(),STGM_READ))){wchar_t target[32768]={},args[32768]={},work[32768]={};link->GetPath(target,32768,nullptr,0);link->GetArguments(args,32768);link->GetWorkingDirectory(work,32768);if(*target)item.target=target;item.arguments=args;item.workingDirectory=work;}searchOpen(hwnd,item,true);}
    if(result==5)parent->invoke(DeskUnpinApp,path);if(result==6)parent->invoke(DeskPinApp,path);
    if(result==7){{std::lock_guard lock(parent->mutex);std::erase(parent->extraFiles,path);}parent->saveFiles();parent->refresh();}
    if(result==10)parent->invoke(DeskSearch);if(result==11)parent->invoke(DeskAddApp);if(result==12)openTarget(knownFolder(FOLDERID_Desktop));if(result==13)parent->refresh();if(result==14)parent->invoke(DeskSettings);if(result==15)openTarget(L"taskmgr.exe");
    if(result==16)appearance=true;if(result==17)beginArrange();
    if(result>=20&&result<=22){eqMode=result-20;WritePrivateProfileStringW(L"desktop",L"spectrum",std::to_wstring(eqMode).c_str(),parent->ini.c_str());}
    if(result==23||result==24){eqGain=result==23?1:1.5f;WritePrivateProfileStringW(L"desktop",L"gain",result==23?L"1":L"1.5",parent->ini.c_str());}draw();
}
LRESULT CALLBACK Desktop::Impl::proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    auto v=(View*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);if(msg==WM_NCCREATE){v=(View*)((CREATESTRUCTW*)lp)->lpCreateParams;v->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)v);}if(!v)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_CREATE:DragAcceptFiles(hwnd,TRUE);SetTimer(hwnd,1,33,nullptr);return 0;
    case WM_PAINT:{PAINTSTRUCT ps;BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);v->draw();return 0;}
    case WM_ERASEBKGND:return 1;
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_TIMER:if(wp==1)v->pulse();if(wp==2){KillTimer(hwnd,2);if(v->note&&v->noteDirty){v->parent->invoke(DeskSaveNote,textOf(v->note));v->noteDirty=false;}}return 0;
    case WM_SETCURSOR:if(LOWORD(lp)==HTCLIENT){POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);float x=p.x/v->scale,y=p.y/v->scale;int handle=v->dragHandle>=0?v->dragHandle:v->resizeHandleAt(x,y);LPCWSTR cursor=v->arranging&&v->widgetAt(x,y)>=0?IDC_SIZEALL:v->hover>=0&&v->hover<(int)v->hits.size()&&v->hits[v->hover].kind>=0?IDC_HAND:IDC_ARROW;if(handle>=0){float directions[]={135,90,45,0,135,90,45,0};int axis=((int)std::lround((directions[handle]+v->layout[v->selectedWidget].angle)/45))%4;cursor=axis==0?IDC_SIZEWE:axis==1?IDC_SIZENESW:axis==2?IDC_SIZENS:IDC_SIZENWSE;}SetCursor(LoadCursorW(nullptr,cursor));return TRUE;}break;
    case WM_MOUSEMOVE:{float x=GET_X_LPARAM(lp)/v->scale,y=GET_Y_LPARAM(lp)/v->scale;if(v->arranging&&v->dragHandle>=0){v->resizeWidget(x,y);return 0;}if(v->arranging&&v->dragWidget>=0){int id=v->dragWidget;auto base=v->baseBounds(id);float px=std::round((x-v->grabX)/v->grid)*v->grid,py=std::round((y-v->grabY)/v->grid)*v->grid;auto old=v->bounds(id);auto next=constrainWidget({px,py,old.w,old.h},v->layout[id].angle,v->width,v->height);v->layout[id].x=next.x-v->left-base.x;v->layout[id].y=next.y-v->top-base.y;v->draw();return 0;}
        int next=v->hitAt(x,y);bool overMusic=v->layout[View::Music].shown&&v->widgetContains(View::Music,x,y)&&!v->appearance&&!v->arranging;if(next!=v->hover||overMusic!=v->musicOver){v->hover=next;v->musicOver=overMusic;v->draw();}if(!v->tracking){TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);v->tracking=true;}
        if(v->pressed>=0&&v->pressed<(int)v->hits.size()&&v->hits[v->pressed].kind==8){v->seekDragging=true;v->seekRatio=v->musicRatio(x,y);v->draw();return 0;}
        if(v->pressed>=0&&v->pressed<(int)v->hits.size()&&v->hits[v->pressed].kind==6){auto b=v->hits[v->pressed].box;v->parent->shade=std::clamp((int)std::lround((x-b.x)/b.w*75),0,75);v->draw();return 0;}
        if(v->pressed>=0&&v->pressed<(int)v->hits.size()&&v->hits[v->pressed].kind==2&&(std::abs(GET_X_LPARAM(lp)-v->down.x)>GetSystemMetrics(SM_CXDRAG)||std::abs(GET_Y_LPARAM(lp)-v->down.y)>GetSystemMetrics(SM_CYDRAG))){auto path=v->hits[v->pressed].path;v->pressed=-1;ReleaseCapture();v->dragFile(path);}return 0;}
    case WM_MOUSELEAVE:v->tracking=false;v->musicOver=false;v->hover=-1;v->draw();return 0;
    case WM_LBUTTONDOWN:{if(!v->arranging&&v->widgetAt(GET_X_LPARAM(lp)/v->scale,GET_Y_LPARAM(lp)/v->scale)==View::Spectrum)return 0;SetForegroundWindow(v->parent->owner);if(v->animation&&GetTickCount64()-v->animation<620)return 0;v->down={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};v->pressed=-1;float x=v->down.x/v->scale,y=v->down.y/v->scale;if(v->arranging){int toolbar=v->hitAt(x,y);if(toolbar>=0&&v->hits[toolbar].widget==-2){v->pressed=toolbar;SetCapture(hwnd);return 0;}int handle=v->resizeHandleAt(x,y);if(handle>=0){v->dragHandle=handle;v->resizeStart=v->bounds(v->selectedWidget);v->resizeX=x;v->resizeY=y;SetCapture(hwnd);return 0;}int id=v->widgetAt(x,y);if(id>=0){auto b=v->bounds(id);if(v->closeBox(id).contains(x,y)){v->layout[id].shown=false;v->draw();return 0;}v->selectedWidget=v->dragWidget=id;v->grabX=x-b.x;v->grabY=y-b.y;SetCapture(hwnd);v->draw();}return 0;}
        v->pressed=v->hitAt(x,y);if(v->pressed>=0)SetCapture(hwnd);return 0;}
    case WM_LBUTTONUP:{if(v->dragWidget>=0||v->dragHandle>=0){v->dragWidget=v->dragHandle=-1;ReleaseCapture();v->draw();return 0;}int pressed=v->pressed;v->pressed=-1;ReleaseCapture();if(pressed>=0&&pressed<(int)v->hits.size()){auto start=v->hits[pressed];float x=GET_X_LPARAM(lp)/v->scale,y=GET_Y_LPARAM(lp)/v->scale;if(start.kind==1&&(std::abs(GET_X_LPARAM(lp)-v->down.x)>GetSystemMetrics(SM_CXDRAG)||std::abs(GET_Y_LPARAM(lp)-v->down.y)>GetSystemMetrics(SM_CYDRAG))){for(auto& hit:v->hits)if(hit.kind==1&&v->hitContains(hit,x,y)){v->parent->invoke(DeskReorderApp,std::to_wstring(start.index)+L","+std::to_wstring(hit.index));break;}}else if(start.kind==8){v->parent->invoke(101,std::to_wstring(v->data.media.duration*v->musicRatio(x,y)));v->seekDragging=false;v->draw();}else if((start.kind==6||v->hitContains(start,x,y))&&start.click){health::Hold protection;if(protection)start.click();}}return 0;}
    case WM_CAPTURECHANGED:v->dragWidget=v->dragHandle=-1;v->pressed=-1;v->seekDragging=false;return 0;
    case WM_LBUTTONDBLCLK:{if(v->arranging)return 0;float x=GET_X_LPARAM(lp)/v->scale,y=GET_Y_LPARAM(lp)/v->scale;for(auto& hit:v->hits)if(hit.kind==2&&v->hitContains(hit,x,y)){openTarget(hit.path);return 0;}return 0;}
    case WM_RBUTTONUP:case WM_CONTEXTMENU:{POINT p;GetCursorPos(&p);v->context(p);return 0;}
    case WM_MOUSEWHEEL:{if(v->arranging)return 0;POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&p);float x=p.x/v->scale,y=p.y/v->scale;int id=v->widgetAt(x,y);if(id==View::Spectrum)return 0;int delta=GET_WHEEL_DELTA_WPARAM(wp)>0?-1:1;if(id==View::Apps)v->appPage=std::clamp(v->appPage+delta,0,std::max(0,((int)v->data.pinned.size()-1)/v->appsPerPage));else if(id==View::Files)v->filePage=std::clamp(v->filePage+delta,0,std::max(0,((int)v->catalog->files.size()-1)/21));else if(id==View::Windows&&v->data.tasks)v->taskPage=std::clamp(v->taskPage+delta,0,std::max(0,((int)v->data.tasks->groups.size()-1)/9));else if(id==View::Controls){auto b=v->bounds(View::Controls);if(x>=b.x+180&&x<b.x+224)v->parent->invoke(DeskVolume,std::to_wstring(delta<0?3:-3));}v->draw();return 0;}
    case WM_KEYDOWN:if(v->arranging){if(wp==VK_ESCAPE)v->finishArrange(false);return 0;}if(wp==VK_ESCAPE&&v->eqSettings){v->eqSettings=false;v->draw();}if(wp==VK_ESCAPE&&v->appearance){v->appearance=false;v->draw();}if(wp==VK_RETURN&&v->page==1&&v->catalog&&v->selectedFile>=0&&v->selectedFile<(int)v->catalog->files.size())openTarget(v->catalog->files[v->selectedFile].path);if(wp==VK_F5)v->parent->refresh();if(wp=='F'&&(GetKeyState(VK_CONTROL)&0x8000))v->parent->invoke(DeskSearch);return 0;
    case WM_DROPFILES:{HDROP drop=(HDROP)wp;for(UINT i=0,n=DragQueryFileW(drop,0xffffffff,nullptr,0);i<n;i++){UINT size=DragQueryFileW(drop,i,nullptr,0);std::wstring path(size+1,0);DragQueryFileW(drop,i,path.data(),size+1);path.resize(size);v->parent->addFile(path);}DragFinish(drop);v->page=1;v->draw();return 0;}
    case WM_COMMAND:if(LOWORD(wp)==201&&!v->editing){if(HIWORD(wp)==EN_CHANGE){v->noteDirty=true;SetTimer(hwnd,2,500,nullptr);}if(HIWORD(wp)==EN_KILLFOCUS&&v->noteDirty){KillTimer(hwnd,2);v->parent->invoke(DeskSaveNote,textOf(v->note));v->noteDirty=false;}}if(LOWORD(wp)==202&&HIWORD(wp)==EN_KILLFOCUS&&!v->editing&&!v->data.timerRunning&&!v->data.timerPaused)v->parent->invoke(DeskSetTimer,textOf(v->timer));return 0;
    case WM_CTLCOLOREDIT:{SetTextColor((HDC)wp,RGB((v->data.ink>>16)&255,(v->data.ink>>8)&255,v->data.ink&255));unsigned color=v->inputColor();SetBkColor((HDC)wp,RGB((color>>16)&255,(color>>8)&255,color&255));return (LRESULT)v->brush;}
    case WM_APP+20:v->resize();return 0;
    case WM_DESTROY:if(v->note&&v->noteDirty)v->parent->invoke(DeskSaveNote,textOf(v->note));KillTimer(hwnd,1);KillTimer(hwnd,2);v->hwnd=nullptr;return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void Desktop::Impl::rebuild(){
    if(rebuilding)return;auto newHost=StandaloneMode?(IsWindow(host)?host:createIndependentHost()):desktopHost();if(!newHost)return;rebuilding=true;auto oldViews=std::move(views);host=newHost;
    std::vector<RECT> monitors;EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT r,LPARAM p)->BOOL{((std::vector<RECT>*)p)->push_back(*r);return TRUE;},(LPARAM)&monitors);
    for(auto rect:monitors){auto view=std::make_unique<View>(this);view->monitor=rect;view->loadLayout();view->imageMedia.start(view->layoutSection);POINT position{rect.left,rect.top};MapWindowPoints(nullptr,host,&position,1);
        auto previous=SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(host));
        auto background=snapshot().background;COLORREF color=RGB((background>>16)&255,(background>>8)&255,background&255);
        view->backdrop=CreateWindowExW(0,BackdropClass,L"",WS_CHILD|WS_CLIPSIBLINGS,position.x,position.y,rect.right-rect.left,rect.bottom-rect.top,host,nullptr,GetModuleHandleW(nullptr),(LPVOID)(UINT_PTR)color);
        HWND child=CreateWindowExW(WS_EX_CONTROLPARENT,DeskClass,L"Рабочий стол",WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,position.x,position.y,rect.right-rect.left,rect.bottom-rect.top,host,nullptr,GetModuleHandleW(nullptr),view.get());
        SetThreadDpiAwarenessContext(previous);if(!child)continue;
        view->data=snapshot();view->resize();view->animation=0;
        if(view->backdrop){SetWindowPos(view->backdrop,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW|SWP_NOACTIVATE);RedrawWindow(view->backdrop,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);}
        SetWindowPos(child,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW|SWP_NOACTIVATE);view->pulse(true);auto key=L"OwnIsland.DesktopView"+std::to_wstring(views.size());SetPropW(owner,key.c_str(),child);views.push_back(std::move(view));
    }
    if(StandaloneMode){SetWindowPos(host,nullptr,GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);}
    keepBelowApplications(host);
    bool ready=views.size()==monitors.size()&&!views.empty();for(auto& v:views)ready&=v->gfx&&IsWindow(v->backdrop);
    if(ready)nativeDesktopTakeover(StandaloneMode?desktopHost():host);
    rebuilding=false;
}
void Desktop::Impl::stop(){
    nativeDesktopRelease();
    media.stop();
    {std::lock_guard lock(mutex);stopping=true;}wake.notify_all();if(worker.joinable())worker.join();views.clear();if(StandaloneMode&&IsWindow(host))DestroyWindow(host);host=nullptr;activeAudio=false;
}
Desktop::Desktop()=default;
Desktop::~Desktop(){stop();}
void Desktop::start(HWND owner,AudioMeter* audio,std::function<DesktopData()> snapshot,std::function<void(int,const std::wstring&)> action){
    if(impl)return;impl=std::make_unique<Impl>();impl->owner=owner;impl->audio=audio;impl->snapshot=std::move(snapshot);impl->action=std::move(action);impl->pinned=impl->snapshot().pinned;
    impl->spectrumMode=std::clamp((int)GetPrivateProfileIntW(L"desktop",L"spectrum",0,impl->ini.c_str()),0,7);wchar_t gain[16]={};GetPrivateProfileStringW(L"desktop",L"gain",L"1",gain,16,impl->ini.c_str());impl->gain=std::clamp(wcstof(gain,nullptr),.5f,2.f);impl->eqFPS=std::clamp((int)GetPrivateProfileIntW(L"desktop",L"eq-fps",30,impl->ini.c_str()),5,60);impl->eqBands=GetPrivateProfileIntW(L"desktop",L"eq-bands",48,impl->ini.c_str());if(impl->eqBands!=12&&impl->eqBands!=24)impl->eqBands=48;impl->eqEnabled=GetPrivateProfileIntW(L"desktop",L"eq-enabled",1,impl->ini.c_str())!=0;impl->eqSmooth=GetPrivateProfileIntW(L"desktop",L"eq-smooth",1,impl->ini.c_str())!=0;impl->eqPeaks=GetPrivateProfileIntW(L"desktop",L"eq-peaks",1,impl->ini.c_str())!=0;
    impl->preset=std::clamp((int)GetPrivateProfileIntW(L"desktop",L"wallpaper-preset",0,impl->ini.c_str()),0,2);impl->placement=std::clamp((int)GetPrivateProfileIntW(L"desktop",L"wallpaper-fit",0,impl->ini.c_str()),0,2);impl->shade=std::clamp((int)GetPrivateProfileIntW(L"desktop",L"wallpaper-shade",28,impl->ini.c_str()),0,75);impl->photo=GetPrivateProfileIntW(L"desktop",L"wallpaper-photo",0,impl->ini.c_str())!=0;
    for(int i=0;i<64;i++){wchar_t path[32768]={};GetPrivateProfileStringW(L"desktop-files",std::to_wstring(i).c_str(),L"",path,32768,impl->ini.c_str());if(*path)impl->extraFiles.push_back(path);}
    WNDCLASSEXW cls={sizeof(cls)};cls.lpfnWndProc=Impl::proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=DeskClass;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.style=CS_DBLCLKS;RegisterClassExW(&cls);cls.lpszClassName=BackdropClass;cls.lpfnWndProc=backdropProc;cls.style=0;RegisterClassExW(&cls);
    impl->clockShadow=GetPrivateProfileIntW(L"desktop",L"clock-shadow",0,impl->ini.c_str())!=0;impl->clockStyle=std::clamp((int)GetPrivateProfileIntW(L"desktop",L"clock-style",0,impl->ini.c_str()),0,8);impl->media.start(L"desktop",impl->photo);
    impl->rebuild();auto target=impl.get();impl->worker=std::thread([target]{target->run();});
}
void Desktop::stop(){if(impl){HWND owner=impl->owner;size_t count=impl->views.size();impl->stop();for(size_t i=0;i<count;i++){auto key=L"OwnIsland.DesktopView"+std::to_wstring(i);RemovePropW(owner,key.c_str());}RemovePropW(owner,L"OwnIsland.DesktopCount");RemovePropW(owner,L"OwnIsland.DesktopActive");impl.reset();}}
void Desktop::displayChanged(){if(impl)impl->displayDirty=true;}
void Desktop::maintain(){
    if(!impl)return;HWND host=StandaloneMode?(IsWindow(impl->host)?impl->host:nullptr):desktopHost();std::vector<RECT> monitors;EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT r,LPARAM p)->BOOL{((std::vector<RECT>*)p)->push_back(*r);return TRUE;},(LPARAM)&monitors);
    bool rebuild=host!=impl->host||monitors.size()!=impl->views.size();for(size_t i=0;i<impl->views.size();i++)if(!IsWindow(impl->views[i]->hwnd)||i>=monitors.size()||!EqualRect(&monitors[i],&impl->views[i]->monitor))rebuild=true;
    if(impl->displayDirty&&!rebuild){for(auto& v:impl->views)v->resize();impl->displayDirty=false;}
    if(rebuild){bool modal=!IsWindowEnabled(impl->owner);for(auto& view:impl->views)modal|=view->menu;if(modal)return;impl->rebuild();impl->displayDirty=false;}impl->activeAudio=false;
    keepBelowApplications(impl->host);
    auto data=impl->snapshot();
    bool ready=impl->views.size()==monitors.size()&&!impl->views.empty();for(auto& v:impl->views)ready&=IsWindow(v->hwnd)&&v->gfx&&IsWindow(v->backdrop);
    if(ready)nativeDesktopTakeover(StandaloneMode?desktopHost():impl->host);
    COLORREF color=RGB((data.background>>16)&255,(data.background>>8)&255,data.background&255);
    for(auto& v:impl->views)if(v->backdrop&&(COLORREF)GetWindowLongPtrW(v->backdrop,GWLP_USERDATA)!=color){SetWindowLongPtrW(v->backdrop,GWLP_USERDATA,color);InvalidateRect(v->backdrop,nullptr,TRUE);}
    {std::lock_guard lock(impl->mutex);if(impl->pinned!=data.pinned){impl->pinned=data.pinned;impl->dirty=true;impl->wake.notify_one();}}
    HWND native=impl->host?FindWindowExW(impl->host,nullptr,L"SHELLDLL_DefView",nullptr):nullptr;
    bool nativeAbove=false;for(HWND child=impl->host?GetWindow(impl->host,GW_CHILD):nullptr;child;child=GetWindow(child,GW_HWNDNEXT)){if(child==native){nativeAbove=true;break;}wchar_t cls[64]={};GetClassNameW(child,cls,64);if(!wcscmp(cls,DeskClass))break;}
    for(auto& v:impl->views){auto asset=v->imageMedia.snapshot();SetPropW(v->hwnd,L"OwnIsland.ImageFrame",(HANDLE)(UINT_PTR)(asset.image?(asset.image->id&0xfffffff):0));if(nativeAbove&&IsWindow(v->hwnd))SetWindowPos(v->hwnd,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOREDRAW);v->pulse();impl->activeAudio|=v->visible&&v->page==0&&v->layout[Impl::View::Spectrum].shown&&v->eqEnabled;}
    bool anyVisible=false;for(auto& v:impl->views)anyVisible|=v->visible;impl->media.setActive(anyVisible&&data.performance.video);
    SetPropW(impl->owner,L"OwnIsland.DesktopCount",(HANDLE)(UINT_PTR)impl->views.size());SetPropW(impl->owner,L"OwnIsland.DesktopActive",(HANDLE)(UINT_PTR)(impl->activeAudio?1:0));
}
void Desktop::reveal(){if(!impl)return;for(auto& v:impl->views){v->animation=GetTickCount64();v->pulse(true);}}
bool Desktop::needsAudio()const{return impl&&impl->activeAudio;}
int Desktop::analysisRate()const{int fps=5;if(impl)for(auto& v:impl->views)if(v->visible&&v->page==0&&v->eqEnabled&&v->layout[Impl::View::Spectrum].shown)fps=std::max(fps,v->eqFPS);return fps;}
void Desktop::appearance(){if(!impl)return;for(auto& v:impl->views)if(v->visible){v->appearance=true;v->draw();break;}}
void Desktop::refreshFiles(){if(impl)impl->refresh();}




