// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "components.h"
#include "file_browser.h"
#include "runtime.h"
#include "native_desktop.h"
#include "health.h"
#include <shellapi.h>
#include <filesystem>
#include <cstring>
#include <type_traits>
#include <stdexcept>

ComponentRole CurrentComponent=ComponentRole::Panel;
namespace {
constexpr DWORD Protocol=0x49534332;
constexpr ULONG_PTR ActionPacket=0x49534143,FilePacket=0x49534650;
constexpr size_t StateCapacity=1024*1024,TaskCapacity=16*1024*1024,CoverCapacity=4*1024*1024;
constexpr UINT HostCommand=WM_APP+81;
struct Shared {
    DWORD protocol,parent; FILETIME created; UINT_PTR owner,host;
    DWORD stateSize,taskSize,coverSize,stateRevision,taskRevision,coverRevision;
    DWORD audioNeeded,analysisFPS; ULONGLONG heartbeat;
    SpectrumFrame spectrum;
    unsigned char state[StateCapacity],tasks[TaskCapacity],cover[CoverCapacity];
};
struct Channel {
    HANDLE mapping=nullptr,mutex=nullptr; Shared* data=nullptr;
    ~Channel(){if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);if(mutex)CloseHandle(mutex);}
    bool open(const std::wstring& name,bool create){
        mutex=create?CreateMutexW(nullptr,FALSE,(name+L".Lock").c_str()):OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,(name+L".Lock").c_str());
        mapping=create?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),name.c_str()):OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
        if(!mutex||!mapping)return false;
        data=(Shared*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared));return data!=nullptr;
    }
};
struct Lock {
    HANDLE mutex; bool held=false;
    Lock(HANDLE h,DWORD wait=0):mutex(h){if(h){auto r=WaitForSingleObject(h,wait);held=r==WAIT_OBJECT_0||r==WAIT_ABANDONED;}}
    ~Lock(){if(held)ReleaseMutex(mutex);}
};
struct Writer {
    std::vector<unsigned char> bytes;
    template<class T> void pod(const T& x){static_assert(std::is_trivially_copyable_v<T>);raw(&x,sizeof(x));}
    void raw(const void* p,size_t n){if(n>TaskCapacity||bytes.size()>TaskCapacity-n)throw std::runtime_error("packet limit");if(n)bytes.insert(bytes.end(),(const unsigned char*)p,(const unsigned char*)p+n);}
    void string(const std::wstring& s){if(s.size()>131072)throw std::runtime_error("string limit");pod((DWORD)s.size());raw(s.data(),s.size()*sizeof(wchar_t));}
};
struct Reader {
    const unsigned char* p;size_t n;
    template<class T> T pod(){T x{};raw(&x,sizeof(x));return x;}
    void raw(void* out,size_t size){if(size>n)throw std::runtime_error("truncated packet");if(size)memcpy(out,p,size);p+=size;n-=size;}
    DWORD count(DWORD limit){auto size=pod<DWORD>();if(size>limit)throw std::runtime_error("count limit");return size;}
    std::wstring string(){auto size=count(131072);std::wstring out(size,0);raw(out.data(),size*sizeof(wchar_t));return out;}
};
void strings(Writer& w,const std::vector<std::wstring>& list){w.pod((DWORD)list.size());for(auto& s:list)w.string(s);}
std::vector<std::wstring> strings(Reader& r){std::vector<std::wstring> out;for(DWORD n=r.count(128);n;n--)out.push_back(r.string());return out;}
Writer statePacket(const DesktopData& d){
    Writer w;w.pod(d.performance);w.pod(d.network);w.pod(d.media.available);w.pod(d.media.playing);w.string(d.media.title);w.string(d.media.artist);w.string(d.media.source);w.pod(d.media.position);w.pod(d.media.duration);w.pod(d.media.revision);
    w.pod(d.weather.available);w.string(d.weather.city);w.string(d.weather.error);w.pod(d.weather.errorCode);w.pod(d.weather.temperature);w.pod(d.weather.wind);w.pod(d.weather.humidity);w.pod(d.weather.code);w.pod((DWORD)d.weather.days.size());for(auto& day:d.weather.days){w.string(day.date);w.pod(day.low);w.pod(day.high);w.pod(day.code);}
    strings(w,d.pinned);w.string(d.note);for(auto c:{d.background,d.card,d.accent,d.ink,d.muted,d.hover,d.outline})w.pod(c);w.pod(d.outlineWidth);w.pod(d.outlineSmooth);w.pod(d.timerSeconds);w.pod(d.timerRunning);w.pod(d.timerPaused);w.pod(d.mutedAudio);w.pod(d.awake);w.pod(d.volume);return w;
}
DesktopData statePacket(Reader& r){
    DesktopData d;d.performance=r.pod<PerformanceSettings>();d.network=r.pod<NetworkKind>();d.media.available=r.pod<bool>();d.media.playing=r.pod<bool>();d.media.title=r.string();d.media.artist=r.string();d.media.source=r.string();d.media.position=r.pod<double>();d.media.duration=r.pod<double>();d.media.revision=r.pod<unsigned long long>();
    d.weather.available=r.pod<bool>();d.weather.city=r.string();d.weather.error=r.string();d.weather.errorCode=r.pod<long>();d.weather.temperature=r.pod<double>();d.weather.wind=r.pod<double>();d.weather.humidity=r.pod<int>();d.weather.code=r.pod<int>();for(auto n=r.count(32);n;n--){WeatherDay day;day.date=r.string();day.low=r.pod<double>();day.high=r.pod<double>();day.code=r.pod<int>();d.weather.days.push_back(day);}
    d.pinned=strings(r);d.note=r.string();unsigned* colors[]={&d.background,&d.card,&d.accent,&d.ink,&d.muted,&d.hover,&d.outline};for(auto c:colors)*c=r.pod<unsigned>();d.outlineWidth=r.pod<float>();d.outlineSmooth=r.pod<bool>();d.timerSeconds=r.pod<int>();d.timerRunning=r.pod<bool>();d.timerPaused=r.pod<bool>();d.mutedAudio=r.pod<bool>();d.awake=r.pod<bool>();d.volume=r.pod<float>();return d;
}
void image(Writer& w,const std::shared_ptr<const PixelImage>& image){
    w.pod(bool(image));if(!image)return;w.pod(image->id);w.pod(image->width);w.pod(image->height);w.pod((DWORD)image->bgra.size());w.raw(image->bgra.data(),image->bgra.size());
}
std::shared_ptr<const PixelImage> image(Reader& r){
    if(!r.pod<bool>())return {};auto p=std::make_shared<PixelImage>();p->id=r.pod<uint64_t>();p->width=r.pod<unsigned>();p->height=r.pod<unsigned>();auto n=r.count(1024*1024);if(p->width>512||p->height>512||n!=(size_t)p->width*p->height*4)throw std::runtime_error("invalid image");p->bgra.resize(n);r.raw(p->bgra.data(),n);return p;
}
Writer taskPacket(const std::shared_ptr<const TaskSnapshot>& tasks){
    Writer w;w.pod((DWORD)(tasks?tasks->groups.size():0));if(!tasks){w.pod(DWORD(0));return w;}for(auto& g:tasks->groups){w.string(g.key);w.string(g.name);w.string(g.path);w.pod(g.active);image(w,g.icon);w.pod((DWORD)g.windows.size());for(auto& window:g.windows){w.pod((UINT_PTR)window.hwnd);w.pod(window.pid);w.string(window.title);}}
    w.pod((DWORD)tasks->shortcuts.size());for(auto& [path,icon]:tasks->shortcuts){w.string(path);image(w,icon);}return w;
}
std::shared_ptr<const TaskSnapshot> taskPacket(Reader& r){
    auto tasks=std::make_shared<TaskSnapshot>();for(auto n=r.count(512);n;n--){TaskGroup g;g.key=r.string();g.name=r.string();g.path=r.string();g.active=r.pod<bool>();g.icon=image(r);for(auto count=r.count(1024);count;count--){TaskWindow window;window.hwnd=(HWND)r.pod<UINT_PTR>();window.pid=r.pod<DWORD>();window.title=r.string();g.windows.push_back(window);}tasks->groups.push_back(std::move(g));}for(auto n=r.count(128);n;n--){auto path=r.string();tasks->shortcuts[path]=image(r);}return tasks;
}
std::wstring quote(const std::wstring& value){std::wstring out=L"\"";size_t slashes=0;for(auto c:value){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}out.append(slashes*2,L'\\');return out+L"\"";}
bool spawn(const std::wstring& args,PROCESS_INFORMATION& pi){
    wchar_t module[32768]={};GetModuleFileNameW(nullptr,module,32768);auto exe=std::filesystem::path(module).parent_path()/(args.find(L" --desktop-host ")==0?L"Island.Desktop.exe":L"Island.Files.exe");auto command=quote(exe.wstring())+args+runtimeArguments();STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;
    return CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)!=FALSE;
}
std::wstring filesClass(){wchar_t desk[256]={};DWORD size=0;GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desk,sizeof(desk),&size);uint64_t hash=1469598103934665603ull;for(auto c:stateDirectory()+L"|"+desk){hash^=(uint16_t)c;hash*=1099511628211ull;}return L"OwnIsland.FilesHost."+std::to_wstring(hash);}
bool sendFile(HWND host,const std::wstring& path,bool select,bool fresh){
    if(path.size()>32766)return false;COPYDATASTRUCT cd{FilePacket+(select?1:0)+(fresh?2:0),(DWORD)((path.size()+1)*sizeof(wchar_t)),(void*)path.c_str()};DWORD_PTR result=0;return SendMessageTimeoutW(host,WM_COPYDATA,0,(LPARAM)&cd,SMTO_ABORTIFHUNG,1500,&result)&&result;
}
MenuColors filesPalette(){
    auto ini=stateDirectory()+L"\\settings.ini";MenuColors c{0x09090b,0x252328,0xeae9ed,0x8a8790,0xb8d8c9};unsigned* fields[]={&c.background,&c.card,&c.text,&c.muted,&c.accent,&c.outline};const wchar_t* keys[]={L"background",L"card",L"text",L"secondary",L"accent",L"outline"};for(int i=0;i<6;i++){wchar_t s[24]={};GetPrivateProfileStringW(L"colors",keys[i],L"",s,24,ini.c_str());if(wcslen(s)==7&&s[0]==L'#')*fields[i]=wcstoul(s+1,nullptr,16);}wchar_t width[24]={};GetPrivateProfileStringW(L"colors",L"outline-width",L"0",width,24,ini.c_str());c.width=std::clamp(wcstof(width,nullptr),0.f,6.f);c.smooth=GetPrivateProfileIntW(L"colors",L"outline-smooth",1,ini.c_str())!=0;return c;
}
struct FileHost {
    unsigned idle=0;MenuColors palette=filesPalette();
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto self=(FileHost*)GetWindowLongPtrW(w,GWLP_USERDATA);if(m==WM_NCCREATE){self=(FileHost*)((CREATESTRUCTW*)b)->lpCreateParams;SetWindowLongPtrW(w,GWLP_USERDATA,(LONG_PTR)self);}if(!self)return DefWindowProcW(w,m,a,b);
        if(m==WM_COPYDATA){auto cd=(COPYDATASTRUCT*)b;if(!cd||cd->dwData<FilePacket||cd->dwData>FilePacket+3||!cd->lpData||cd->cbData<2||cd->cbData>65536||cd->cbData%2)return FALSE;auto raw=(const wchar_t*)cd->lpData;if(raw[cd->cbData/2-1])return FALSE;self->idle=0;return openFileBrowser(std::wstring(raw,cd->cbData/2-1),(cd->dwData-FilePacket)&1,(cd->dwData-FilePacket)&2);}
        if(m==WM_TIMER){auto count=fileBrowserWindowCount();if(HWND panel=FindWindowW(L"OwnIsland.Surface",nullptr))SetPropW(panel,L"OwnIsland.FilesWindow",lastFileBrowserWindow());SetPropW(w,L"OwnIsland.Files.Count",(HANDLE)(UINT_PTR)count);SetPropW(w,L"OwnIsland.Files.Busy",(HANDLE)(UINT_PTR)fileOperationsActive());if(!count&&!fileOperationsActive()){if(++self->idle>=4)DestroyWindow(w);}else self->idle=0;auto colors=filesPalette();if(colors.background!=self->palette.background||colors.card!=self->palette.card||colors.text!=self->palette.text||colors.muted!=self->palette.muted||colors.accent!=self->palette.accent||colors.outline!=self->palette.outline||colors.width!=self->palette.width||colors.smooth!=self->palette.smooth){self->palette=colors;fileBrowserColors(colors);}return 0;}
        if(m==WM_DISPLAYCHANGE||m==WM_DPICHANGED){fileBrowserDisplayChanged();return 0;}
        if(m==WM_QUERYENDSESSION)return !fileOperationsActive();
        if(m==WM_ENDSESSION&&a){health::stopping(true);return 0;}
        if(m==WM_CLOSE){if(!fileOperationsActive()){closeFileBrowsers();DestroyWindow(w);}return 0;}
        if(m==WM_DESTROY){KillTimer(w,1);PostQuitMessage(0);return 0;}return DefWindowProcW(w,m,a,b);
    }
};
struct DesktopHost {
    Channel channel;Desktop desktop;AudioMeter audio;DesktopData data;
    HANDLE parent=nullptr;HWND hwnd=nullptr;DWORD stateRevision=0,taskRevision=0,coverRevision=0;ULONGLONG lastMaintain=0;bool stopping=false;
    std::shared_ptr<const TaskSnapshot> tasks;std::shared_ptr<const std::vector<unsigned char>> cover;
    ~DesktopHost(){if(parent)CloseHandle(parent);}
    void read(){
        {Lock lock(channel.mutex);if(!lock.held)return;auto& s=*channel.data;if(s.protocol!=Protocol)return;
        try{
            if(s.taskRevision!=taskRevision&&s.taskSize<=TaskCapacity){Reader r{s.tasks,s.taskSize};tasks=taskPacket(r);taskRevision=s.taskRevision;}
            if(s.coverRevision!=coverRevision&&s.coverSize<=CoverCapacity){cover=s.coverSize?std::make_shared<const std::vector<unsigned char>>(s.cover,s.cover+s.coverSize):nullptr;coverRevision=s.coverRevision;}
            if(s.stateRevision!=stateRevision&&s.stateSize<=StateCapacity){Reader r{s.state,s.stateSize};auto next=statePacket(r);data=std::move(next);stateRevision=s.stateRevision;}
            data.tasks=tasks;data.media.cover=cover;audio.importSpectrum(s.spectrum);
            s.host=(UINT_PTR)hwnd;s.heartbeat=GetTickCount64();
        }catch(...){data.awake=false;}}
    }
    void action(int code,const std::wstring& value){
        if(code==DeskSaveNote)data.note=value;
        if(value.size()>131072)return;Writer w;w.pod(code);w.string(value);COPYDATASTRUCT cd{ActionPacket,(DWORD)w.bytes.size(),w.bytes.data()};DWORD_PTR result=0;
        bool delivered=SendMessageTimeoutW((HWND)channel.data->owner,WM_COPYDATA,(WPARAM)hwnd,(LPARAM)&cd,SMTO_ABORTIFHUNG,1500,&result)&&result;
        if(code==DeskSaveNote&&!delivered)writeNote(value);
    }
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
        auto self=(DesktopHost*)GetWindowLongPtrW(w,GWLP_USERDATA);if(m==WM_NCCREATE){self=(DesktopHost*)((CREATESTRUCTW*)b)->lpCreateParams;SetWindowLongPtrW(w,GWLP_USERDATA,(LONG_PTR)self);}if(!self)return DefWindowProcW(w,m,a,b);
        if(m==WM_TIMER){if(self->stopping||self->channel.data->protocol!=Protocol||WaitForSingleObject(self->parent,0)!=WAIT_TIMEOUT){self->stopping=true;if((health::shared&&health::read(health::shared->gate)>0)||GetCapture()||!IsWindowEnabled(w))return 0;DestroyWindow(w);return 0;}self->read();auto now=GetTickCount64();if(now-self->lastMaintain>=250){self->desktop.maintain();self->lastMaintain=now;}Lock lock(self->channel.mutex);if(lock.held){self->channel.data->audioNeeded=self->desktop.needsAudio();self->channel.data->analysisFPS=self->desktop.analysisRate();}return 0;}
        if(m==HostCommand){if(a==1)self->desktop.reveal();if(a==2)self->desktop.appearance();if(a==3)self->desktop.displayChanged();if(a==4)self->desktop.refreshFiles();return 0;}
        if(m==WM_APP+13){self->desktop.maintain();return 0;}
        if(m==WM_DISPLAYCHANGE||m==WM_DPICHANGED){self->desktop.displayChanged();return 0;}
        if(m==WM_APP+5)return 0;
        if(m==WM_CLOSE){self->stopping=true;return 0;}
        if(m==WM_DESTROY){self->desktop.stop();KillTimer(w,1);PostQuitMessage(0);return 0;}
        return DefWindowProcW(w,m,a,b);
    }
};
}

struct DesktopProcess::Impl {
    HWND owner=nullptr;AudioMeter* audio=nullptr;std::function<DesktopData()> snapshot;
    Channel channel;std::wstring name;PROCESS_INFORMATION child{};
    std::shared_ptr<const TaskSnapshot> lastTasks;std::shared_ptr<const std::vector<unsigned char>> lastCover;
    bool wanted=false,needAudio=false;int fps=5,restarts=0;ULONGLONG retryAt=0,windowStart=0;HWND host=nullptr;
    ~Impl(){if(child.hProcess)CloseHandle(child.hProcess);}
    void publish(){
        if(!channel.data)return;try{auto d=snapshot();auto state=statePacket(d);bool tasksChanged=d.tasks!=lastTasks,coverChanged=d.media.cover!=lastCover;Writer tasks;if(tasksChanged)tasks=taskPacket(d.tasks);if(state.bytes.size()>StateCapacity||tasks.bytes.size()>TaskCapacity||(d.media.cover&&d.media.cover->size()>CoverCapacity))return;
            Lock lock(channel.mutex);if(!lock.held)return;auto& s=*channel.data;memcpy(s.state,state.bytes.data(),state.bytes.size());s.stateSize=(DWORD)state.bytes.size();++s.stateRevision;
            if(tasksChanged){memcpy(s.tasks,tasks.bytes.data(),tasks.bytes.size());s.taskSize=(DWORD)tasks.bytes.size();++s.taskRevision;lastTasks=d.tasks;}
            if(coverChanged||!s.coverRevision){s.coverSize=d.media.cover?(DWORD)d.media.cover->size():0;if(s.coverSize)memcpy(s.cover,d.media.cover->data(),s.coverSize);++s.coverRevision;lastCover=d.media.cover;}
        }catch(...){}
    }
    void launch(){
        if(child.hProcess){CloseHandle(child.hProcess);child={};}host=nullptr;needAudio=false;
        if(spawn(L" --desktop-host "+quote(name),child)){CloseHandle(child.hThread);child.hThread=nullptr;SetPropW(owner,L"OwnIsland.DesktopProcess",(HANDLE)(UINT_PTR)child.dwProcessId);}retryAt=GetTickCount64()+2000;
    }
    void command(WPARAM command){if(host&&IsWindow(host))PostMessageW(host,HostCommand,command,0);}
};
DesktopProcess::DesktopProcess()=default;
DesktopProcess::~DesktopProcess(){stop();}
void DesktopProcess::start(HWND owner,AudioMeter* audio,std::function<DesktopData()> snapshot,std::function<void(int,const std::wstring&)>){
    if(impl)return;auto p=std::make_unique<Impl>();p->owner=owner;p->audio=audio;p->snapshot=std::move(snapshot);FILETIME created{},a{},b{},c{};GetProcessTimes(GetCurrentProcess(),&created,&a,&b,&c);
    p->name=L"Local\\OwnIsland.Desktop."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());if(!p->channel.open(p->name,true))return;auto& s=*p->channel.data;s.protocol=Protocol;s.parent=GetCurrentProcessId();s.created=created;s.owner=(UINT_PTR)owner;p->wanted=true;p->windowStart=GetTickCount64();p->publish();p->launch();impl=std::move(p);
}
void DesktopProcess::stop(){
    if(!impl)return;auto p=std::move(impl);p->wanted=false;p->command(0);if(p->host)PostMessageW(p->host,WM_CLOSE,0,0);
    // The child also observes its parent's lifetime. No forced termination of modal work.
    InterlockedExchange((volatile LONG*)&p->channel.data->protocol,0);
    RemovePropW(p->owner,L"OwnIsland.DesktopProcess");RemovePropW(p->owner,L"OwnIsland.DesktopHost");RemovePropW(p->owner,L"OwnIsland.DesktopCount");RemovePropW(p->owner,L"OwnIsland.DesktopActive");for(int i=0;i<16;i++)RemovePropW(p->owner,(L"OwnIsland.DesktopView"+std::to_wstring(i)).c_str());
}
void DesktopProcess::maintain(){
    if(!impl)return;auto& p=*impl;auto now=GetTickCount64();if(p.child.hProcess&&WaitForSingleObject(p.child.hProcess,0)==WAIT_OBJECT_0){p.needAudio=false;nativeDesktopRecover(stateDirectory()+L"\\shell-restore.ini");if(now-p.windowStart>60000){p.windowStart=now;p.restarts=0;}if(now>=p.retryAt&&p.restarts<3){++p.restarts;p.launch();}}
    else if(!p.child.hProcess&&now>=p.retryAt&&p.restarts<3){++p.restarts;p.launch();}p.publish();audioFrame();
    if(p.host){auto count=(UINT_PTR)GetPropW(p.host,L"OwnIsland.DesktopCount");SetPropW(p.owner,L"OwnIsland.DesktopCount",(HANDLE)count);SetPropW(p.owner,L"OwnIsland.DesktopActive",(HANDLE)(UINT_PTR)p.needAudio);for(UINT_PTR i=0;i<count&&i<16;i++){auto key=L"OwnIsland.DesktopView"+std::to_wstring(i);SetPropW(p.owner,key.c_str(),GetPropW(p.host,key.c_str()));}}
}
void DesktopProcess::audioFrame(){if(!impl||!impl->channel.data)return;auto& p=*impl;Lock lock(p.channel.mutex);if(!lock.held)return;auto& s=*p.channel.data;HWND host=(HWND)s.host;DWORD pid=0;GetWindowThreadProcessId(host,&pid);if(pid==p.child.dwProcessId){p.host=host;SetPropW(p.owner,L"OwnIsland.DesktopHost",host);p.needAudio=s.audioNeeded&&GetTickCount64()-s.heartbeat<2000;p.fps=std::clamp((int)s.analysisFPS,5,60);}if(p.audio&&p.needAudio)s.spectrum=p.audio->spectrum();else s.spectrum={};}
void DesktopProcess::displayChanged(){if(impl)impl->command(3);}
void DesktopProcess::reveal(){if(impl)impl->command(1);}
bool DesktopProcess::needsAudio()const{return impl&&impl->needAudio;}
int DesktopProcess::analysisRate()const{return impl?impl->fps:5;}
void DesktopProcess::refreshFiles(){if(impl)impl->command(4);}
void DesktopProcess::appearance(){if(impl)impl->command(2);}
DWORD DesktopProcess::processId()const{return impl?impl->child.dwProcessId:0;}

bool receiveComponentAction(HWND sender,const COPYDATASTRUCT* cd,std::unique_ptr<ComponentAction>& action){
    if(!cd||cd->dwData!=ActionPacket||!cd->lpData||cd->cbData>270000)return false;HWND panel=FindWindowW(L"OwnIsland.Surface",nullptr);DWORD pid=0;GetWindowThreadProcessId(sender,&pid);if(!pid||pid!=(DWORD)(UINT_PTR)GetPropW(panel,L"OwnIsland.DesktopProcess"))return false;
    try{Reader r{(const unsigned char*)cd->lpData,cd->cbData};auto value=std::make_unique<ComponentAction>();value->code=r.pod<int>();value->value=r.string();if(r.n||!((value->code>=DeskSearch&&value->code<=DeskAppearance)||value->code==100||value->code==101))return false;action=std::move(value);return true;}catch(...){return false;}
}
int runDesktopComponent(const std::wstring& name){
    CurrentComponent=ComponentRole::Desktop;health::initialize();DesktopHost host;if(!host.channel.open(name,false))return 10;auto& s=*host.channel.data;if(s.protocol!=Protocol)return 11;host.parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,s.parent);FILETIME created{},a{},b{},c{};if(!host.parent||!GetProcessTimes(host.parent,&created,&a,&b,&c)||CompareFileTime(&created,&s.created))return 12;DWORD owner=0;GetWindowThreadProcessId((HWND)s.owner,&owner);if(owner!=s.parent)return 13;ProtectionOwner=s.parent;
    auto singleName=L"Local\\OwnIsland.DesktopOwner."+std::to_wstring(s.parent)+L"."+std::to_wstring(s.created.dwLowDateTime);
    std::unique_ptr<void,decltype(&CloseHandle)> singleHandle(CreateMutexW(nullptr,FALSE,singleName.c_str()),CloseHandle);Lock single(singleHandle.get(),5000);if(!single.held)return 15;
    if(s.protocol!=Protocol)return 16;
    WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=DesktopHost::proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"OwnIsland.DesktopController";RegisterClassExW(&cls);host.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,cls.hInstance,&host);if(!host.hwnd)return 14;
    // A visible, empty tool window owns keyboard focus without activating the
    // multi-monitor desktop container above applications on the other monitor.
    SetWindowRgn(host.hwnd,CreateRectRgn(0,0,0,0),FALSE);SetWindowPos(host.hwnd,nullptr,-32000,-32000,1,1,SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    host.read();host.desktop.start(host.hwnd,&host.audio,[&]{return host.data;},[&](int code,const std::wstring& value){host.action(code,value);});SetTimer(host.hwnd,1,16,nullptr);MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return 0;
}
bool windowsFileManager(){return GetPrivateProfileIntW(L"shell",L"windows-file-manager",0,(stateDirectory()+L"\\settings.ini").c_str())!=0;}
void setWindowsFileManager(bool enabled){WritePrivateProfileStringW(L"shell",L"windows-file-manager",enabled?L"1":L"0",(stateDirectory()+L"\\settings.ini").c_str());}
void componentFilesDisplayChanged(){if(HWND host=FindWindowW(filesClass().c_str(),nullptr))PostMessageW(host,WM_DISPLAYCHANGE,0,0);}
bool routeFileBrowser(const std::wstring& path,bool select,bool fresh){
    if(windowsFileManager()){
        wchar_t windows[32768]={};GetWindowsDirectoryW(windows,32768);auto exe=std::wstring(windows)+L"\\explorer.exe";auto args=path.empty()?L"":(select?L"/select,":L"")+quote(path);SHELLEXECUTEINFOW info{sizeof(info)};info.lpFile=exe.c_str();info.lpParameters=args.c_str();info.nShow=SW_SHOWNORMAL;return ShellExecuteExW(&info)!=FALSE;
    }
    auto host=FindWindowW(filesClass().c_str(),nullptr);if(host){DWORD pid=0;GetWindowThreadProcessId(host,&pid);AllowSetForegroundWindow(pid);return sendFile(host,path,select,fresh);}
    PROCESS_INFORMATION child{};if(!spawn(L" --files-host "+quote(path)+(select?L" --select":L"")+(fresh?L" --new-window":L""),child))return false;AllowSetForegroundWindow(child.dwProcessId);CloseHandle(child.hThread);CloseHandle(child.hProcess);return true;
}
int runFilesComponent(const std::wstring& path,bool select,bool fresh){
    CurrentComponent=ComponentRole::Files;auto clsName=filesClass();HANDLE mutex=CreateMutexW(nullptr,FALSE,(L"Local\\"+clsName).c_str());if(!mutex)return 20;auto lock=WaitForSingleObject(mutex,0);if(lock!=WAIT_OBJECT_0&&lock!=WAIT_ABANDONED){auto until=GetTickCount64()+10000;HWND existing=nullptr;while(!(existing=FindWindowW(clsName.c_str(),nullptr))&&GetTickCount64()<until)Sleep(20);int result=existing&&sendFile(existing,path,select,fresh)?0:21;CloseHandle(mutex);return result;}
    health::initialize();FileHost host;fileBrowserColors(host.palette);WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=FileHost::proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=clsName.c_str();RegisterClassExW(&cls);HWND window=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,cls.hInstance,&host);int result=22;
    if(window){SetPropW(window,L"OwnIsland.Files.Process",(HANDLE)(UINT_PTR)GetCurrentProcessId());health::ready(window);if(openFileBrowser(path,select,fresh)){SetTimer(window,1,250,nullptr);MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}result=0;}else DestroyWindow(window);}ReleaseMutex(mutex);CloseHandle(mutex);return result;
}


