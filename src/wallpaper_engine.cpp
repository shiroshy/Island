// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "wallpaper_engine.h"
#include "platform.h"
#include "health.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <exception>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

namespace {
struct EngineMonitor {RECT bounds;bool primary;std::wstring device;};
int engineMonitorIndex(std::vector<EngineMonitor> monitors,RECT target){
    std::stable_sort(monitors.begin(),monitors.end(),[](const auto& a,const auto& b){if(a.primary!=b.primary)return a.primary;return _wcsicmp(a.device.c_str(),b.device.c_str())<0;});
    for(size_t i=0;i<monitors.size();i++)if(EqualRect(&monitors[i].bounds,&target))return (int)i;return -1;
}
std::wstring engineQuote(const std::wstring& value){
    std::wstring out=L"\"";size_t slashes=0;for(wchar_t c:value){if(c==L'\\'){slashes++;continue;}if(c==L'\"')out.append(slashes*2+1,L'\\');else out.append(slashes,L'\\');slashes=0;out+=c;}out.append(slashes*2,L'\\');return out+L"\"";
}
std::wstring engineSetting(const std::wstring& section,const wchar_t* key){wchar_t text[32768]={};GetPrivateProfileStringW(section.c_str(),key,L"",text,32768,(stateDirectory()+L"\\settings.ini").c_str());return text;}
bool engineFile(const std::wstring& path){auto a=GetFileAttributesW(path.c_str());return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_DIRECTORY);}
std::wstring processPath(DWORD pid){HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!p)return {};wchar_t path[32768];DWORD n=32768;bool ok=QueryFullProcessImageNameW(p,0,path,&n);CloseHandle(p);return ok?std::wstring(path,n):L"";}
bool samePath(const std::wstring& a,const std::wstring& b){return !a.empty()&&!b.empty()&&!_wcsicmp(std::filesystem::path(a).lexically_normal().c_str(),std::filesystem::path(b).lexically_normal().c_str());}
bool engineExecutable(const std::wstring& path){auto name=std::filesystem::path(path).filename().wstring();return engineFile(path)&&(!_wcsicmp(name.c_str(),L"wallpaper64.exe")||!_wcsicmp(name.c_str(),L"wallpaper32.exe"));}
bool engineRendererPath(const std::wstring& actual,const std::wstring& controller){
    if(samePath(actual,controller))return true;auto path=std::filesystem::path(actual),root=std::filesystem::path(controller).parent_path();auto name=path.filename().wstring();bool renderer=false;for(auto known:{L"wallpaper32.exe",L"wallpaper64.exe",L"webwallpaper32.exe",L"webwallpaper64.exe"})renderer|=!_wcsicmp(name.c_str(),known);
    return renderer&&(samePath(path.parent_path().wstring(),root.wstring())||samePath(path.parent_path().wstring(),(root/L"bin").wstring()));
}
std::wstring installedEngine(){
    auto saved=engineSetting(L"wallpaper-engine",L"executable");if(engineExecutable(saved))return saved;
    // Installation can appear after Island starts. Prefer the actual controller
    // in this Windows session before consulting possibly stale Steam manifests.
    HANDLE processes=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);
    if(processes!=INVALID_HANDLE_VALUE){PROCESSENTRY32W entry{sizeof(entry)};std::wstring running;if(Process32FirstW(processes,&entry))do{
            if(_wcsicmp(entry.szExeFile,L"wallpaper64.exe")&&_wcsicmp(entry.szExeFile,L"wallpaper32.exe"))continue;
            DWORD candidateSession=~0u;if(!ProcessIdToSessionId(entry.th32ProcessID,&candidateSession)||candidateSession!=session)continue;
            auto path=processPath(entry.th32ProcessID);if(engineExecutable(path)){running=std::move(path);break;}
        }while(Process32NextW(processes,&entry));CloseHandle(processes);if(!running.empty())return running;}
    wchar_t steam[32768]={};DWORD size=sizeof(steam);RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,steam,&size);std::set<std::wstring> roots;if(*steam)roots.insert(steam);
    // Read only Steam's bounded library manifest; never scan drives or Workshop.
    for(auto root:std::vector<std::wstring>(roots.begin(),roots.end())){
        auto file=std::filesystem::path(root)/L"steamapps/libraryfolders.vdf";std::error_code ec;auto length=std::filesystem::file_size(file,ec);if(ec||length>1024*1024)continue;
        std::ifstream in(file,std::ios::binary);std::string line;while(std::getline(in,line)){auto key=line.find("\"path\"");if(key==std::string::npos)continue;auto begin=line.find('"',key+6),end=begin==std::string::npos?begin:line.find('"',begin+1);if(end==std::string::npos)continue;auto bytes=line.substr(begin+1,end-begin-1);std::string decoded;for(size_t i=0;i<bytes.size();i++){decoded+=bytes[i];if(bytes[i]=='\\'&&i+1<bytes.size()&&bytes[i+1]=='\\')i++;}int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,decoded.data(),(int)decoded.size(),nullptr,0);if(n){std::wstring path(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,decoded.data(),(int)decoded.size(),path.data(),n);roots.insert(path);}}
    }
    for(auto& root:roots)for(auto name:{L"wallpaper64.exe",L"wallpaper32.exe"}){auto path=(std::filesystem::path(root)/L"steamapps/common/wallpaper_engine"/name).wstring();if(engineExecutable(path))return path;}return {};
}
std::wstring enginePicker(HWND owner,bool executable){
    health::Hold protection;Com<IFileOpenDialog> dialog;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.put()))))return {};
    COMDLG_FILTERSPEC filter{executable?L"Wallpaper Engine":L"Обои Wallpaper Engine",executable?L"wallpaper64.exe;wallpaper32.exe":L"project.json;*.pkg;*.html;*.htm;*.mp4;*.webm;*.m4v;*.mov;*.wmv;*.avi"};dialog->SetFileTypes(1,&filter);dialog->SetTitle(executable?L"Wallpaper Engine":L"Выбрать обои");dialog->SetOptions(FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST|FOS_DONTADDTORECENT);if(FAILED(dialog->Show(owner)))return {};Com<IShellItem> item;PWSTR value=nullptr;if(FAILED(dialog->GetResult(item.put()))||FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&value)))return {};std::wstring result=value;CoTaskMemFree(value);return result;
}
bool engineProject(const std::wstring& path){if(!engineFile(path)||!std::filesystem::path(path).is_absolute())return false;auto extension=std::filesystem::path(path).extension().wstring();for(auto& c:extension)c=towlower(c);return extension==L".json"||extension==L".pkg"||extension==L".html"||extension==L".htm"||extension==L".mp4"||extension==L".webm"||extension==L".m4v"||extension==L".mov"||extension==L".wmv"||extension==L".avi";}
struct EngineCommand {bool ok=false;std::wstring output;};
EngineCommand engineCommand(const std::wstring& exe,const std::wstring& arguments){
    EngineCommand result;SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE read=nullptr,write=nullptr;if(!CreatePipe(&read,&write,&security,0))return result;SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
    // Explicit handle list prevents inheriting unrelated application handles.
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<BYTE> storage(bytes);auto attributes=(LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();
    bool initialized=InitializeProcThreadAttributeList(attributes,1,0,&bytes)!=FALSE;
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);HANDLE handles[]={write,input};
    STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;si.StartupInfo.wShowWindow=SW_HIDE;si.StartupInfo.hStdOutput=write;si.StartupInfo.hStdError=write;si.StartupInfo.hStdInput=input;si.lpAttributeList=attributes;
    PROCESS_INFORMATION process{};auto command=engineQuote(exe)+L" "+arguments;auto cwd=std::filesystem::path(exe).parent_path().wstring();
    bool launched=initialized&&input!=INVALID_HANDLE_VALUE&&UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr)&&CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,cwd.c_str(),&si.StartupInfo,&process);
    CloseHandle(write);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);if(initialized)DeleteProcThreadAttributeList(attributes);
    if(!launched){CloseHandle(read);return result;}
    std::string output;auto deadline=GetTickCount64()+5000;
    for(;;){DWORD available=0;if(PeekNamedPipe(read,nullptr,0,nullptr,&available,nullptr)&&available){char buffer[4096];DWORD got=0;if(ReadFile(read,buffer,std::min<DWORD>(available,sizeof(buffer)),&got,nullptr)&&output.size()+got<=65536)output.append(buffer,got);}
        if(WaitForSingleObject(process.hProcess,10)==WAIT_OBJECT_0){DWORD code=1;GetExitCodeProcess(process.hProcess,&code);result.ok=code==0;DWORD remaining=0;while(output.size()<65536&&PeekNamedPipe(read,nullptr,0,nullptr,&remaining,nullptr)&&remaining){char buffer[4096];DWORD got=0;if(!ReadFile(read,buffer,std::min<DWORD>(remaining,sizeof(buffer)),&got,nullptr)||!got)break;output.append(buffer,std::min<size_t>(got,65536-output.size()));}break;}if(GetTickCount64()>=deadline)break;
    }
    // A timed-out command could be the primary engine: never terminate it.
    CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(read);
    if(output.size()>=2&&(BYTE)output[0]==0xff&&(BYTE)output[1]==0xfe){result.output.assign((wchar_t*)(output.data()+2),(output.size()-2)/2);}else{int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,output.data(),(int)output.size(),nullptr,0);if(n){result.output.resize(n);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,output.data(),(int)output.size(),result.output.data(),n);}}
    while(!result.output.empty()&&iswspace(result.output.back()))result.output.pop_back();if(!result.output.empty()&&result.output.front()==0xfeff)result.output.erase(0,1);if(result.output.size()>1&&result.output.front()==L'"'&&result.output.back()==L'"')result.output=result.output.substr(1,result.output.size()-2);return result;
}
bool engineRunning(const std::wstring& exe){HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return false;PROCESSENTRY32W entry{sizeof(entry)};bool found=false;if(Process32FirstW(snapshot,&entry))do{if(!_wcsicmp(entry.szExeFile,std::filesystem::path(exe).filename().c_str())&&samePath(processPath(entry.th32ProcessID),exe)){found=true;break;}}while(Process32NextW(snapshot,&entry));CloseHandle(snapshot);return found;}
std::wstring engineConfigProject(const std::wstring& exe,int index){
    // Some shipped versions return no redirected getWallpaper output. Read only
    // the current user's exact saved selection; never rewrite vendor settings.
    try{
        auto root=std::filesystem::path(exe).parent_path();if(!_wcsicmp(root.filename().c_str(),L"distribution"))root=root.parent_path();auto file=root/L"config.json";std::error_code error;auto size=std::filesystem::file_size(file,error);if(error||size>8*1024*1024)return {};
        std::ifstream input(file,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(input)),{});wchar_t user[257]={};DWORD length=257;if(!GetUserNameW(user,&length))return {};
        auto config=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(bytes));
        for(auto entry:config){if(_wcsicmp(entry.Key().c_str(),user))continue;auto profile=entry.Value().GetObject();auto general=profile.HasKey(L"general")?profile.GetNamedObject(L"general"):profile;auto wallpapers=general.GetNamedObject(L"wallpaperconfig").GetNamedObject(L"selectedwallpapers");auto selection=wallpapers.GetNamedObject(L"Monitor"+std::to_wstring(index));std::wstring path=selection.GetNamedString(L"file").c_str();return engineProject(path)?path:L"";}
    }catch(...){}return {};
}
std::wstring currentEngineProject(const std::wstring& exe,int index){auto answer=engineCommand(exe,L"-control getWallpaper -monitor "+std::to_wstring(index));return answer.ok&&engineProject(answer.output)?answer.output:engineConfigProject(exe,index);}
void launchEngine(const std::wstring& exe){STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION p{};auto command=engineQuote(exe);auto cwd=std::filesystem::path(exe).parent_path().wstring();if(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,cwd.c_str(),&si,&p)){CloseHandle(p.hThread);CloseHandle(p.hProcess);}}
HWND engineWindow(const std::wstring& exe,const std::wstring& title){struct Find{const std::wstring& exe;const std::wstring& title;HWND result=nullptr;} find{exe,title};EnumWindows([](HWND w,LPARAM p)->BOOL{auto& f=*(Find*)p;wchar_t name[512]={};GetWindowTextW(w,name,512);if(f.title!=name)return TRUE;DWORD pid=0;GetWindowThreadProcessId(w,&pid);if(!engineRendererPath(processPath(pid),f.exe))return TRUE;f.result=w;return FALSE;},(LPARAM)&find);return find.result;}
HANDLE engineWatch(HWND source,HANDLE sourceProcess,const std::wstring& title,const std::wstring& controller){
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,GetCurrentProcessId()),engine=nullptr;
    HANDLE cancel=CreateEventW(&security,TRUE,FALSE,nullptr),ready=CreateEventW(&security,TRUE,FALSE,nullptr);DuplicateHandle(GetCurrentProcess(),sourceProcess,GetCurrentProcess(),&engine,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0);
    SIZE_T length=0;InitializeProcThreadAttributeList(nullptr,1,0,&length);std::vector<BYTE> storage(length);auto list=(LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();bool initialized=InitializeProcThreadAttributeList(list,1,0,&length)!=FALSE;
    HANDLE handles[]={parent,engine,cancel,ready};STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;si.StartupInfo.wShowWindow=SW_HIDE;si.lpAttributeList=list;PROCESS_INFORMATION process{};
    wchar_t self[32768]={};GetModuleFileNameW(nullptr,self,32768);auto number=[](HANDLE h){return std::to_wstring((UINT_PTR)h);};auto command=engineQuote(self)+L" --wallpaper-watch "+number(parent)+L" "+number(engine)+L" "+number(cancel)+L" "+number(ready)+L" "+std::to_wstring((UINT_PTR)source)+L" "+engineQuote(title)+L" "+engineQuote(controller);
    bool launched=parent&&engine&&cancel&&ready&&initialized&&UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr)&&CreateProcessW(self,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&process);
    bool guarded=launched&&WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;if(launched){CloseHandle(process.hThread);CloseHandle(process.hProcess);}if(initialized)DeleteProcThreadAttributeList(list);for(auto handle:{parent,engine,ready})if(handle)CloseHandle(handle);if(!guarded&&cancel){SetEvent(cancel);CloseHandle(cancel);cancel=nullptr;}return cancel;
}
}

int wallpaperEngineMonitorIndex(RECT target){
    std::vector<EngineMonitor> monitors;EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT bounds,LPARAM p)->BOOL{MONITORINFOEXW info{};info.cbSize=sizeof(info);if(GetMonitorInfoW(monitor,&info))((std::vector<EngineMonitor>*)p)->push_back({*bounds,(info.dwFlags&MONITORINFOF_PRIMARY)!=0,info.szDevice});return TRUE;},(LPARAM)&monitors);
    return engineMonitorIndex(std::move(monitors),target);
}

int wallpaperEngineWatch(const std::vector<std::wstring>& arguments){
    if(arguments.size()!=8)return 2;auto handle=[&](int n){return (HANDLE)(UINT_PTR)wcstoull(arguments[n].c_str(),nullptr,10);};HANDLE parent=handle(1),engine=handle(2),cancel=handle(3),ready=handle(4);HWND window=(HWND)handle(5);auto title=arguments[6];DWORD parentPid=GetProcessId(parent),enginePid=GetProcessId(engine),windowPid=0;GetWindowThreadProcessId(window,&windowPid);
    auto expected=L"Island.Wallpaper."+std::to_wstring(parentPid)+L".";bool valid=parentPid&&enginePid&&enginePid==windowPid&&title.rfind(expected,0)==0&&engineExecutable(arguments[7])&&engineRendererPath(processPath(enginePid),arguments[7]);
    if(valid){SetEvent(ready);HANDLE waits[]={cancel,parent};if(WaitForMultipleObjects(2,waits,FALSE,INFINITE)==WAIT_OBJECT_0+1&&WaitForSingleObject(engine,0)==WAIT_TIMEOUT){wchar_t actual[512]={};GetWindowTextW(window,actual,512);GetWindowThreadProcessId(window,&windowPid);if(windowPid==enginePid&&title==actual)PostMessageW(window,WM_CLOSE,0,0);}}
    for(auto h:{parent,engine,cancel,ready})if(h)CloseHandle(h);return valid?0:3;
}

struct EngineWallpaper::Impl {
    std::mutex mutex;std::condition_variable wake;std::thread worker;bool stopping=false,active=false,current=false,follow=false;
    uint64_t revision=0;int index=0;RECT monitor{};HWND anchor=nullptr;std::wstring section,executable,file,title;EngineWallpaperState state;
    void save(){auto ini=stateDirectory()+L"\\settings.ini";WritePrivateProfileStringW(section.c_str(),L"wallpaper-engine",state.enabled?L"1":L"0",ini.c_str());WritePrivateProfileStringW(section.c_str(),L"wallpaper-engine-follow",follow?L"1":L"0",ini.c_str());WritePrivateProfileStringW(section.c_str(),L"wallpaper-engine-file",file.c_str(),ini.c_str());WritePrivateProfileStringW(L"wallpaper-engine",L"executable",executable.c_str(),ini.c_str());}
    void run(){
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::wstring ownedExe,ownedTitle;HWND source=nullptr;HANDLE owner=nullptr,watch=nullptr;uint64_t applied=~0ull;int attempts=0;ULONGLONG retry=0,poll=0;
        auto close=[&]{if(!ownedTitle.empty()){
                engineCommand(ownedExe,L"-control closeWallpaper -location "+engineQuote(ownedTitle));
                DWORD pid=0;wchar_t name[512]={};if(source){GetWindowThreadProcessId(source,&pid);GetWindowTextW(source,name,512);}
                if(owner&&pid==GetProcessId(owner)&&WaitForSingleObject(owner,0)==WAIT_TIMEOUT&&ownedTitle==name)PostMessageW(source,WM_CLOSE,0,0);
                ownedTitle.clear();
            }if(watch){SetEvent(watch);CloseHandle(watch);watch=nullptr;}if(owner){CloseHandle(owner);owner=nullptr;}source=nullptr;};
        for(;;){std::wstring exe,path;bool use=false,enabled=false,lookup=false,sync=false;uint64_t generation;
            {std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(100));if(stopping)break;exe=executable;path=file;generation=revision;enabled=state.enabled;use=active&&enabled;lookup=current;current=false;sync=follow&&use&&GetTickCount64()>=poll;}
            if(generation!=applied||!enabled){close();std::lock_guard lock(mutex);state.window=nullptr;state.loading=use;applied=generation;attempts=0;retry=0;}
            DWORD actualPid=0;wchar_t actualTitle[512]={};if(source){GetWindowThreadProcessId(source,&actualPid);GetWindowTextW(source,actualTitle,512);}
            if(source&&(!IsWindow(source)||!owner||actualPid!=GetProcessId(owner)||WaitForSingleObject(owner,0)!=WAIT_TIMEOUT||ownedTitle!=actualTitle||GetParent(source)!=anchor)){close();std::lock_guard lock(mutex);state.window=nullptr;}
            if(source&&(IsIconic(source)!=FALSE)==use){
                // Preserve this window's decoder/composition resources. Minimization
                // gives its renderer the native SIZE_MINIMIZED state, without a
                // global pause command affecting wallpapers on other monitors.
                ShowWindowAsync(source,use?SW_SHOWNOACTIVATE:SW_SHOWMINNOACTIVE);
            }
            if(!use&&!lookup)continue;
            if(!engineExecutable(exe)){std::lock_guard lock(mutex);state.loading=false;state.error=L"Не найден Wallpaper Engine";continue;}
            if(lookup||sync){
                poll=GetTickCount64()+5000;
                if(!engineRunning(exe))launchEngine(exe);
                auto selected=currentEngineProject(exe,index);
                std::lock_guard lock(mutex);if(generation!=revision)continue;
                if(selected.empty()){if(lookup){state.loading=false;state.error=L"Не удалось получить текущие обои";}continue;}
                if(lookup||!samePath(selected,file)){file=selected;state.enabled=true;follow=true;state.name=std::filesystem::path(file).parent_path().filename().wstring();state.error.clear();revision++;save();continue;}
            }
            if(source||GetTickCount64()<retry||attempts>=3)continue;
            attempts++;retry=GetTickCount64()+15000;
            if(!engineProject(path)){std::lock_guard lock(mutex);state.loading=false;state.error=L"Файл обоев недоступен";continue;}
            if(!engineRunning(exe)){launchEngine(exe);for(int i=0;i<20;i++){std::unique_lock lock(mutex);if(stopping||revision!=generation||!active)break;wake.wait_for(lock,std::chrono::milliseconds(100));if(engineRunning(exe))break;}}
            ownedExe=exe;ownedTitle=title;auto args=L"-control openWallpaper -file "+engineQuote(path)+L" -playInWindow "+engineQuote(title)+L" -width "+std::to_wstring(monitor.right-monitor.left)+L" -height "+std::to_wstring(monitor.bottom-monitor.top)+L" -x -30000 -y -30000 -borderless true -activate false";
            engineCommand(exe,args);
            for(int i=0;i<60;i++){source=engineWindow(exe,title);if(source)break;std::unique_lock lock(mutex);if(stopping||revision!=generation||!active)break;wake.wait_for(lock,std::chrono::milliseconds(100));}
            if(source){DWORD pid=0;GetWindowThreadProcessId(source,&pid);owner=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);watch=owner?engineWatch(source,owner,title,exe):nullptr;if(!watch){close();}else{
                // Only our uniquely named pop-out is embedded. Its host is Island's opaque
                // per-monitor backdrop, never an existing Wallpaper Engine desktop window.
                auto style=GetWindowLongPtrW(source,GWL_STYLE);SetWindowLongPtrW(source,GWL_STYLE,(style|WS_CHILD|WS_CLIPSIBLINGS)&~(WS_POPUP|WS_CAPTION|WS_THICKFRAME));
                auto extended=GetWindowLongPtrW(source,GWL_EXSTYLE);SetWindowLongPtrW(source,GWL_EXSTYLE,(extended|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW)&~WS_EX_APPWINDOW);
                SetParent(source,anchor);if(GetParent(source)!=anchor){close();}else{RECT bounds{};GetClientRect(anchor,&bounds);SetWindowPos(source,HWND_TOP,0,0,bounds.right,bounds.bottom,SWP_NOACTIVATE|SWP_FRAMECHANGED|SWP_SHOWWINDOW);}
            }}
            {std::lock_guard lock(mutex);if(generation==revision&&active&&state.enabled){state.window=source;state.loading=false;state.error=source?L"":L"Wallpaper Engine не открыл обои";}}
        }
        close();CoUninitialize();
    }
};
EngineWallpaper::EngineWallpaper()=default;
EngineWallpaper::~EngineWallpaper(){stop();}
void EngineWallpaper::start(const std::wstring& section,RECT monitor,int index,HWND backgroundHost){if(impl)return;impl=std::make_unique<Impl>();impl->section=section;impl->monitor=monitor;impl->index=index;impl->anchor=backgroundHost;impl->executable=installedEngine();impl->file=engineSetting(section,L"wallpaper-engine-file");impl->follow=engineSetting(section,L"wallpaper-engine-follow")==L"1";impl->state.enabled=engineSetting(section,L"wallpaper-engine")==L"1";impl->state.name=std::filesystem::path(impl->file).parent_path().filename().wstring();impl->title=L"Island.Wallpaper."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L"."+std::to_wstring(index);auto p=impl.get();impl->worker=std::thread([p]{p->run();});}
void EngineWallpaper::stop(){if(!impl)return;{std::lock_guard lock(impl->mutex);impl->stopping=true;}impl->wake.notify_all();if(impl->worker.joinable())impl->worker.join();impl.reset();}
void EngineWallpaper::setActive(bool value){if(!impl)return;bool changed;{std::lock_guard lock(impl->mutex);changed=impl->active!=value;impl->active=value;}if(changed)impl->wake.notify_all();}
void EngineWallpaper::choose(HWND owner,bool current){if(!impl)return;std::wstring exe;{std::lock_guard lock(impl->mutex);exe=impl->executable;}if(!engineExecutable(exe))exe=installedEngine();if(!engineExecutable(exe))exe=enginePicker(owner,true);if(exe.empty())return;if(!engineExecutable(exe)){std::lock_guard lock(impl->mutex);impl->state.error=L"Выберите wallpaper64.exe или wallpaper32.exe";return;}std::wstring path;if(!current){path=enginePicker(owner,false);if(path.empty())return;if(!engineProject(path)){std::lock_guard lock(impl->mutex);impl->state.error=L"Неподдерживаемый файл обоев";return;}}
    {std::lock_guard lock(impl->mutex);impl->executable=exe;impl->current=current;if(!current){impl->follow=false;impl->state.enabled=true;}impl->state.loading=true;impl->state.error.clear();if(!current){impl->file=path;impl->state.name=std::filesystem::path(path).parent_path().filename().wstring();}impl->revision++;if(!current)impl->save();}impl->wake.notify_all();
}
void EngineWallpaper::disable(){if(!impl)return;{std::lock_guard lock(impl->mutex);impl->state.enabled=false;impl->state.window=nullptr;impl->state.loading=false;impl->state.error.clear();impl->revision++;impl->save();}impl->wake.notify_all();}
EngineWallpaperState EngineWallpaper::snapshot(){if(!impl)return {};std::lock_guard lock(impl->mutex);return impl->state;}
