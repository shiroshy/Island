#ifdef ISLAND_INTERNAL_SETTINGS
#include "internal_settings.h"
#else
#include "native_settings.h"
#endif
#include <taskschd.h>
#include <dwmapi.h>
#include <windowsx.h>
#include "legacy_cleanup.h"

using namespace setup;
namespace {
HWND window=nullptr,actionButton=nullptr,cancelButton=nullptr,autostartCheck=nullptr;HFONT font=nullptr,titleFont=nullptr;HBRUSH background=nullptr;bool removing=false,busy=false,finished=false,cleanupAttempted=false;std::wstring caption=L"Island";
int compareVersion(const std::wstring& a,const std::wstring& b){auto left=a.c_str(),right=b.c_str();for(int i=0;i<4;i++){wchar_t* next=nullptr;auto x=wcstoul(left,&next,10);left=*next?next+1:next;auto y=wcstoul(right,&next,10);right=*next?next+1:next;if(x!=y)return x<y?-1:1;}return 0;}
DWORD userProbe(){
    auto root=targetRoot();Com<ITaskService> service;VARIANT empty{};Com<ITaskFolder> folder;Com<IRegisteredTask> task;
    HRESULT hr=CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(service.put()));if(SUCCEEDED(hr))hr=service->Connect(empty,empty,empty,empty);if(SUCCEEDED(hr))hr=service->GetFolder(Bstr(L"\\"),folder.put());if(SUCCEEDED(hr))hr=folder->GetTask(Bstr(L"OwnIsland.Logon."+currentSid()),task.put());
    if(SUCCEEDED(hr)){Com<ITaskDefinition> definition;Com<IActionCollection> actions;Com<IAction> action;Com<IExecAction> exec;LONG count=0;if(FAILED(task->get_Definition(definition.put()))||FAILED(definition->get_Actions(actions.put()))||FAILED(actions->get_Count(&count))||count!=1||FAILED(actions->get_Item(1,action.put()))||FAILED(action->QueryInterface(IID_PPV_ARGS(exec.put()))))return ERROR_INVALID_OWNER;BSTR path=nullptr;exec->get_Path(&path);bool own=path&&same(path,root+L"\\Island.Start.exe");SysFreeString(path);if(!own)return ERROR_INVALID_OWNER;VARIANT_BOOL enabled=VARIANT_FALSE;check(SUCCEEDED(task->get_Enabled(&enabled)));return enabled==VARIANT_FALSE?11:10;}
    if(hr!=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)&&hr!=HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))return ERROR_NOT_READY;
    HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS){wchar_t value[32768]={};DWORD length=sizeof(value),type=0;auto error=RegQueryValueExW(key,L"OwnIsland",nullptr,&type,(BYTE*)value,&length);RegCloseKey(key);if(error==ERROR_SUCCESS){check(type==REG_SZ&&length<=sizeof(value),ERROR_INVALID_OWNER);int count=0;auto args=CommandLineToArgvW(value,&count);bool own=args&&count&&(same(args[0],root+L"\\Island.Start.exe")||same(args[0],root+L"\\Island.exe"));if(args)LocalFree(args);if(!own)return ERROR_INVALID_OWNER;BYTE approval[12]={};DWORD size=sizeof(approval);if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",L"OwnIsland",RRF_RT_REG_BINARY,nullptr,approval,&size)==ERROR_SUCCESS&&(approval[0]==3||approval[0]==7))return 11;return 12;}}
    return 0;
}
void checkRoot(const std::wstring& root,const std::wstring& sid){if(exists(root)){noLink(root);bool ours=ownedInstall(root);if(!ours){bool empty=std::filesystem::is_empty(root);check(empty,ERROR_ALREADY_EXISTS);}else{auto file=exists(root+L"\\installed.ini")?root+L"\\installed.ini":root+L"\\transaction.ini";check(ini(file,L"sid")==sid,ERROR_INVALID_OWNER);auto mode=ini(file,L"settings-mode");
#ifdef ISLAND_INTERNAL_SETTINGS
check(mode==L"internal",ERROR_INVALID_OWNER);
#else
check(mode.empty()||mode==L"native",ERROR_INVALID_OWNER);
#endif
auto version=ini(file,L"version");bool renumbered=false;
#ifdef ISLAND_INTERNAL_SETTINGS
// These two development packages were issued before the public version was frozen.
renumbered=!wcscmp(PackageVersion,L"0.20.3")&&(version==L"0.20.4"||version==L"0.20.5");
#endif
check(version.empty()||compareVersion(version,PackageVersion)<=0||renumbered,ERROR_PRODUCT_VERSION);}}}
void checkServiceOwner(const std::wstring& root,const std::wstring& sid){
#ifdef ISLAND_SETUP_FIXTURE
return;
#endif
auto file=installedDirectory(sid)+L"\\service.ini";if(exists(file)){Config config;check(config.load(file)&&same(config.sid,sid)&&same(config.launcher,root+L"\\Island.Start.exe"),ERROR_INVALID_OWNER);}}
void stopApps(const std::wstring& root,const std::wstring& sid){
#ifdef ISLAND_SETUP_FIXTURE
return;
#endif

    struct State{const std::wstring& root;const std::wstring& sid;std::vector<HANDLE> processes;};State state{root,sid,{}};
    EnumWindows([](HWND hwnd,LPARAM value)->BOOL{auto& state=*(State*)value;wchar_t cls[100]={};GetClassNameW(hwnd,cls,100);if(wcscmp(cls,L"OwnIsland.Surface")&&wcscmp(cls,L"OwnIsland.Files"))return TRUE;DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));if(!process.h)return TRUE;wchar_t path[32768]={};DWORD length=32768;if(!QueryFullProcessImageNameW(process,0,path,&length)||(!same(path,state.root+L"\\Island.exe")&&!same(path,state.root+L"\\Island.Files.exe")))return TRUE;Handle token;if(!OpenProcessToken(process,TOKEN_QUERY,&token.h)||!same(sidOf(token),state.sid))return TRUE;state.processes.push_back(process.h);process.h=nullptr;PostMessageW(hwnd,WM_CLOSE,0,0);return TRUE;},(LPARAM)&state);
    bool done=true;for(auto process:state.processes){done&=WaitForSingleObject(process,12000)==WAIT_OBJECT_0;CloseHandle(process);}check(done,ERROR_BUSY);
    auto deadline=GetTickCount64()+6000;
    do{bool inUse=false;Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));PROCESSENTRY32W p{sizeof(p)};if(Process32FirstW(snapshot,&p))do{if(wcscmp(p.szExeFile,L"Island.exe")&&wcscmp(p.szExeFile,L"Island.Desktop.exe")&&wcscmp(p.szExeFile,L"Island.Files.exe")&&wcscmp(p.szExeFile,L"Island.Start.exe"))continue;Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,p.th32ProcessID));wchar_t path[32768]={};DWORD size=32768;if(process.h&&QueryFullProcessImageNameW(process,0,path,&size)&&same(std::filesystem::path(path).parent_path().wstring(),root))inUse=true;}while(Process32NextW(snapshot,&p));if(!inUse)return;Sleep(50);}while(GetTickCount64()<deadline);throw (DWORD)ERROR_BUSY;
}
void shortcut(REFKNOWNFOLDERID location,const std::wstring& root,bool remove){PWSTR directory=nullptr;check(SUCCEEDED(SHGetKnownFolderPath(location,0,nullptr,&directory)));auto file=std::wstring(directory)+L"\\Island.lnk";CoTaskMemFree(directory);if(remove){if(!exists(file))return;Com<IShellLinkW> link;Com<IPersistFile> persist;check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put()))));check(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(persist.put()))));check(SUCCEEDED(persist->Load(file.c_str(),STGM_READ)));wchar_t target[32768]={};link->GetPath(target,32768,nullptr,0);if(same(target,root+L"\\Island.Start.exe"))DeleteFileW(file.c_str());return;}
    Com<IShellLinkW> link;Com<IPersistFile> persist;check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put()))));check(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(persist.put()))));if(exists(file)){check(SUCCEEDED(persist->Load(file.c_str(),STGM_READ)));wchar_t target[32768]={};link->GetPath(target,32768,nullptr,0);check(same(target,root+L"\\Island.Start.exe"),ERROR_ALREADY_EXISTS);}link->SetPath((root+L"\\Island.Start.exe").c_str());link->SetWorkingDirectory(root.c_str());link->SetIconLocation((root+L"\\Island.exe").c_str(),0);check(SUCCEEDED(persist->Save(file.c_str(),TRUE)));}
DWORD userShortcuts(bool remove){Handle token;check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.h)!=FALSE);check(integrity(token)<=SECURITY_MANDATORY_MEDIUM_RID,ERROR_ACCESS_DENIED);shortcut(FOLDERID_Programs,targetRoot(),remove);shortcut(FOLDERID_Desktop,targetRoot(),remove);return 0;}
void uninstallEntry(const std::wstring& root,bool remove){HKEY key=nullptr;auto error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,UninstallKey,0,KEY_QUERY_VALUE,&key);if(error==0){wchar_t location[32768]={};DWORD bytes=sizeof(location);auto query=RegQueryValueExW(key,L"InstallLocation",nullptr,nullptr,(BYTE*)location,&bytes);RegCloseKey(key);check(query==0&&same(location,root),ERROR_INVALID_OWNER);}else check(error==ERROR_FILE_NOT_FOUND,error);if(remove){if(error==0)check(RegDeleteTreeW(HKEY_LOCAL_MACHINE,UninstallKey)==0);return;}check(RegCreateKeyExW(HKEY_LOCAL_MACHINE,UninstallKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)==0);auto text=[&](const wchar_t* name,const std::wstring& value){check(RegSetValueExW(key,name,0,REG_SZ,(const BYTE*)value.c_str(),(DWORD)((value.size()+1)*sizeof(wchar_t)))==0);};try{text(L"DisplayName",L"Island");text(L"DisplayVersion",PackageVersion);text(L"Publisher",L"Shizuka");text(L"InstallLocation",root);text(L"DisplayIcon",root+L"\\Island.exe");text(L"UninstallString",quote(root+L"\\Island.Setup.exe")+L" --uninstall");DWORD one=1;RegSetValueExW(key,L"NoModify",0,REG_DWORD,(BYTE*)&one,sizeof(one));RegSetValueExW(key,L"NoRepair",0,REG_DWORD,(BYTE*)&one,sizeof(one));RegCloseKey(key);}catch(...){RegCloseKey(key);throw;}}
int install(const std::wstring& sid,bool autostart){
    check(admin(),ERROR_ELEVATION_REQUIRED);Owner owner;owner.load();check(owner.sid==sid,ERROR_INVALID_OWNER);auto root=targetRoot();checkRoot(root,sid);checkServiceOwner(root,sid);preflightNative(root);auto prior=owner.execute(module(),L"--user-probe");check(prior==0||prior==10||prior==11||prior==12,prior);
    bool existing=ownedInstall(root);auto oldState=existing&&exists(root+L"\\installed.ini")?read(root+L"\\installed.ini"):Bytes{};std::map<std::wstring,Bytes> oldFiles;std::map<std::wstring,std::wstring> touched;NativeTransaction native(root);bool startupChanged=false,serviceApplied=false,shortcutsTouched=false;
    try{
        stopApps(root,sid);folder(root);for(auto& file:PackageFiles){auto path=root+L"\\"+file.name;if(exists(path))oldFiles[path]=read(path);auto bytes=resource(file.resource);check(bytes.size()==file.size&&sha(bytes)==file.digest,ERROR_CRC);touched[path]=file.digest;write(path,bytes);}
        auto setupPath=root+L"\\Island.Setup.exe";if(!same(module(),setupPath)){if(exists(setupPath))oldFiles[setupPath]=read(setupPath,128*1024*1024);auto bytes=read(module(),128*1024*1024);touched[setupPath]=sha(bytes);write(setupPath,bytes);}
        put(root+L"\\transaction.ini",L"product",ProductTag);put(root+L"\\transaction.ini",L"sid",sid);put(root+L"\\transaction.ini",L"version",PackageVersion);
#ifndef ISLAND_INTERNAL_SETTINGS
put(root+L"\\transaction.ini",L"resource-hash",PatchedPriSHA);
#else
put(root+L"\\transaction.ini",L"settings-mode",L"internal");
#endif
protect(root+L"\\transaction.ini",false);
        native.apply();
        bool beforeOn=prior==10||prior==12;if(autostart!=beforeOn||prior==12){auto code=owner.execute(root+L"\\Island.exe",autostart?L"--configure-autostart on":L"--configure-autostart off");check(code==0,code);startupChanged=true;}
        shortcutsTouched=true;check(owner.execute(module(),L"--user-shortcuts")==0,ERROR_WRITE_FAULT);
        serviceApplied=true;auto code=execute(root+L"\\Island.Preload.Install.exe",L"--install --sid "+quote(sid)+L" --launcher "+quote(root+L"\\Island.Start.exe"));check(code==0,code);
        auto state=root+L"\\installed.ini";put(state,L"product",ProductTag);put(state,L"sid",sid);put(state,L"version",PackageVersion);put(state,L"shell-mode",L"explorer");
#ifndef ISLAND_INTERNAL_SETTINGS
put(state,L"resource-hash",PatchedPriSHA);put(state,L"original-hash",OriginalPriSHA);put(state,L"setting-ids",packagedIds());put(state,L"settings-mode",L"native");
#else
put(state,L"settings-mode",L"internal");
#endif
protect(state,false);uninstallEntry(root,false);DeleteFileW((root+L"\\transaction.ini").c_str());
    }catch(...){auto error=std::current_exception();try{if(serviceApplied&&!existing)execute(root+L"\\Island.Preload.Install.exe",L"--remove --sid "+quote(sid));if(shortcutsTouched&&!existing)owner.execute(module(),L"--user-remove-shortcuts");if(startupChanged)owner.execute(root+L"\\Island.exe",(prior==10||prior==12)?L"--configure-autostart on":L"--configure-autostart off");native.rollback();for(auto& [path,digest]:touched){auto found=oldFiles.find(path);if(found!=oldFiles.end())write(path,found->second);else if(exists(path)&&hash(path)==digest)check(DeleteFileW(path.c_str())!=FALSE);}if(!oldState.empty())write(root+L"\\installed.ini",oldState);else DeleteFileW((root+L"\\installed.ini").c_str());if(serviceApplied&&existing)execute(root+L"\\Island.Preload.Install.exe",L"--install --sid "+quote(sid)+L" --launcher "+quote(root+L"\\Island.Start.exe"));DeleteFileW((root+L"\\transaction.ini").c_str());if(existing)owner.execute(root+L"\\Island.Start.exe",L"--silent");}catch(...){early::log(root+L"\\setup.log",L"rollback-needs-repair",GetLastError());}std::rethrow_exception(error);}
    owner.execute(root+L"\\Island.Start.exe",L"--silent");early::log(root+L"\\setup.log",L"installed",0,owner.session);return 0;
}
int uninstall(const std::wstring& sid){check(admin(),ERROR_ELEVATION_REQUIRED);Owner owner;owner.load();check(owner.sid==sid,ERROR_INVALID_OWNER);auto root=targetRoot();check(ownedInstall(root)&&ini(root+L"\\installed.ini",L"sid")==sid,ERROR_INVALID_OWNER);checkRoot(root,sid);
#ifndef ISLAND_INTERNAL_SETTINGS
for(auto& def:island_settings::definitions)ownKey(def.keyId,root+L"\\Island.SettingsHandler.dll");check(!settingsRunning(),ERROR_BUSY);
#endif
stopApps(root,sid);
    auto code=execute(root+L"\\Island.Preload.Install.exe",L"--remove --sid "+quote(sid));check(code==0,code);check(owner.execute(root+L"\\Island.exe",L"--configure-autostart off")==0,ERROR_ACCESS_DENIED);removeNative(root);check(owner.execute(module(),L"--user-remove-shortcuts")==0,ERROR_WRITE_FAULT);uninstallEntry(root,true);
    for(auto& file:PackageFiles){auto path=root+L"\\"+file.name;if(exists(path)){noLink(path);check(DeleteFileW(path.c_str())!=FALSE);}}
    DeleteFileW((root+L"\\transaction.ini").c_str());DeleteFileW((root+L"\\installed.ini").c_str()); // User profile is deliberately outside this tree and is never deleted.
    auto self=root+L"\\Island.Setup.exe";if(!DeleteFileW(self.c_str()))check(MoveFileExW(self.c_str(),nullptr,MOVEFILE_DELAY_UNTIL_REBOOT)!=FALSE);early::log(root+L"\\setup.log",L"uninstalled-profile-kept",0,owner.session);return 0;
}
int cleanupAndInstall(const std::wstring& sid,bool autostart){check(admin(),ERROR_ELEVATION_REQUIRED);Owner owner;owner.load();check(owner.sid==sid,ERROR_INVALID_OWNER);legacy::Plan plan(targetRoot(),sid);plan.discover(owner.token);plan.apply(stopApps);return install(sid,autostart);}
DWORD elevated(const std::wstring& mode,bool autostart){Owner owner;owner.load();auto parameters=mode+L" --sid "+quote(owner.sid)+L" --autostart "+(autostart?L"1":L"0");auto self=module();SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS;launch.lpVerb=L"runas";launch.lpFile=self.c_str();launch.lpParameters=parameters.c_str();launch.nShow=SW_HIDE;check(ShellExecuteExW(&launch)!=FALSE);Handle process(launch.hProcess);EnableWindow(actionButton,FALSE);EnableWindow(cancelButton,FALSE);EnableWindow(autostartCheck,FALSE);caption=removing?L"Удаление...":L"Установка...";InvalidateRect(window,nullptr,TRUE);busy=true;
    for(;;){auto result=MsgWaitForMultipleObjects(1,&process.h,FALSE,100,QS_ALLINPUT);if(result==WAIT_OBJECT_0)break;MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}}}DWORD code=1;GetExitCodeProcess(process,&code);busy=false;EnableWindow(cancelButton,TRUE);return code;
}
LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){switch(message){
    case WM_CREATE:{window=hwnd;background=CreateSolidBrush(RGB(9,9,11));font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");titleFont=CreateFontW(-34,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");actionButton=CreateWindowW(L"BUTTON",removing?L"Удалить":L"Установить",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,182,158,146,38,hwnd,(HMENU)1,nullptr,nullptr);cancelButton=CreateWindowW(L"BUTTON",L"Отмена",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,28,158,136,38,hwnd,(HMENU)2,nullptr,nullptr);autostartCheck=CreateWindowW(L"BUTTON",L"Автозапуск",WS_CHILD|BS_AUTOCHECKBOX|(removing?0:WS_VISIBLE),30,112,220,26,hwnd,(HMENU)3,nullptr,nullptr);bool enabled=true;try{auto status=userProbe();if(ownedInstall(targetRoot()))enabled=status==10||status==12;
        else if(status==0||status==ERROR_INVALID_OWNER){enabled=status==0;try{Owner owner;owner.load();legacy::Plan legacy(targetRoot(),owner.sid);legacy.discover(owner.token);if(legacy.hasWork())enabled=legacy.startupEnabled();}catch(...){enabled=false;}}
        }catch(...){}SendMessageW(autostartCheck,BM_SETCHECK,enabled?BST_CHECKED:BST_UNCHECKED,0);for(auto child:{actionButton,cancelButton,autostartCheck})SendMessageW(child,WM_SETFONT,(WPARAM)font,TRUE);BOOL dark=TRUE;DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));return 0;}
    case WM_CTLCOLORBTN:case WM_CTLCOLORSTATIC:SetTextColor((HDC)wp,RGB(234,233,237));SetBkColor((HDC)wp,RGB(9,9,11));return (LRESULT)background;
    case WM_DRAWITEM:{auto item=(DRAWITEMSTRUCT*)lp;auto brush=CreateSolidBrush(item->CtlID==1?RGB(184,216,201):RGB(37,35,40));FillRect(item->hDC,&item->rcItem,brush);DeleteObject(brush);SelectObject(item->hDC,font);SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,item->CtlID==1?RGB(9,9,11):RGB(234,233,237));wchar_t text[100]={};GetWindowTextW(item->hwndItem,text,100);DrawTextW(item->hDC,text,-1,&item->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);return TRUE;}
    case WM_PAINT:{PAINTSTRUCT paint;auto dc=BeginPaint(hwnd,&paint);RECT client{};GetClientRect(hwnd,&client);FillRect(dc,&client,background);SelectObject(dc,titleFont);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(234,233,237));RECT text{28,30,338,96};DrawTextW(dc,caption.c_str(),-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);EndPaint(hwnd,&paint);return 0;}
    case WM_COMMAND:if(LOWORD(wp)==2&&!busy){DestroyWindow(hwnd);return 0;}if(LOWORD(wp)==1&&!busy){if(finished){DestroyWindow(hwnd);return 0;}try{auto sid=currentSid();checkRoot(targetRoot(),sid);
#ifndef ISLAND_INTERNAL_SETTINGS
if(removing)check(!settingsRunning(),ERROR_BUSY);else
#endif
preflightNative(targetRoot());if(!removing){checkServiceOwner(targetRoot(),sid);auto prior=userProbe();check(prior==0||prior==10||prior==11||prior==12,prior);}auto code=elevated(removing?L"--remove":L"--apply",SendMessageW(autostartCheck,BM_GETCHECK,0,0)==BST_CHECKED);check(code==0,code);finished=true;caption=removing?L"Удалено":L"Установлено";SetWindowTextW(actionButton,L"Готово");ShowWindow(autostartCheck,SW_HIDE);ShowWindow(cancelButton,SW_HIDE);EnableWindow(actionButton,TRUE);InvalidateRect(hwnd,nullptr,TRUE);}catch(DWORD error){
            if(!removing&&!cleanupAttempted&&(error==ERROR_INVALID_OWNER||error==ERROR_ALREADY_EXISTS)&&MessageBoxW(hwnd,L"Обнаружены остатки старой версии. Убрать её автозапуск, службу и ярлыки, сохранив резервную копию?\n\nНастройки и файлы из старой папки архива сохранятся.",L"Island",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES){
                cleanupAttempted=true;try{auto code=elevated(L"--clean-install",SendMessageW(autostartCheck,BM_GETCHECK,0,0)==BST_CHECKED);check(code==0,code);finished=true;caption=L"Установлено";SetWindowTextW(actionButton,L"Готово");ShowWindow(autostartCheck,SW_HIDE);ShowWindow(cancelButton,SW_HIDE);EnableWindow(actionButton,TRUE);InvalidateRect(hwnd,nullptr,TRUE);return 0;}catch(DWORD cleanupError){MessageBoxW(hwnd,errorText(cleanupError).c_str(),L"Island",MB_OK|MB_ICONERROR);}
            }else MessageBoxW(hwnd,errorText(error).c_str(),L"Island",MB_OK|MB_ICONERROR);
            caption=L"Island";EnableWindow(actionButton,TRUE);EnableWindow(autostartCheck,TRUE);EnableWindow(cancelButton,TRUE);InvalidateRect(hwnd,nullptr,TRUE);
        }return 0;}break;
    case WM_CLOSE:if(!busy)DestroyWindow(hwnd);return 0;case WM_CONTEXTMENU:return 0;case WM_DESTROY:if(font)DeleteObject(font);if(titleFont)DeleteObject(titleFont);if(background)DeleteObject(background);PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,message,wp,lp);}
int gui(bool remove){removing=remove;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);WNDCLASSEXW cls{sizeof(cls)};cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"OwnIsland.Setup";cls.lpfnWndProc=procedure;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(cls.hInstance,MAKEINTRESOURCEW(1));RegisterClassExW(&cls);DWORD style=WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;RECT r{0,0,356,224};AdjustWindowRectEx(&r,style,FALSE,0);auto hwnd=CreateWindowExW(0,cls.lpszClassName,remove?L"Удаление Island":L"Установка Island",style,(GetSystemMetrics(SM_CXSCREEN)-(r.right-r.left))/2,(GetSystemMetrics(SM_CYSCREEN)-(r.bottom-r.top))/2,r.right-r.left,r.bottom-r.top,nullptr,nullptr,cls.hInstance,nullptr);check(hwnd!=nullptr);ShowWindow(hwnd,SW_SHOWNORMAL);MSG message;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}return 0;}
}
#ifndef ISLAND_SETUP_TEST
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);int count=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&count);std::vector<std::wstring> args;for(int i=1;i<count;i++)args.emplace_back(argv[i]);LocalFree(argv);DWORD result=0;try{
    if(args.empty()||(args.size()==1&&args[0]==L"--uninstall"))result=gui(!args.empty());
    else if(args.size()==1&&args[0]==L"--preflight"){Owner owner;owner.load();checkRoot(targetRoot(),owner.sid);preflightNative(targetRoot());}
    else if(args.size()==1&&args[0]==L"--user-probe")result=userProbe();
    else if(args.size()==1&&args[0]==L"--user-shortcuts")result=userShortcuts(false);
    else if(args.size()==1&&args[0]==L"--user-remove-shortcuts")result=userShortcuts(true);
    else if(args.size()==5&&(args[0]==L"--apply"||args[0]==L"--remove"||args[0]==L"--clean-install")&&args[1]==L"--sid"&&args[3]==L"--autostart"&&(args[4]==L"0"||args[4]==L"1")){check(admin(),ERROR_ELEVATION_REQUIRED);Owner owner;owner.load();check(owner.sid==args[2],ERROR_INVALID_OWNER);auto mutexName=L"Global\\OwnIsland.Setup."+owner.sid;Handle mutex(CreateMutexW(nullptr,TRUE,mutexName.c_str()));auto mutexError=GetLastError();check(mutex.h!=nullptr,mutexError);check(mutexError!=ERROR_ALREADY_EXISTS,ERROR_INSTALL_ALREADY_RUNNING);result=args[0]==L"--apply"?install(owner.sid,args[4]==L"1"):args[0]==L"--remove"?uninstall(owner.sid):cleanupAndInstall(owner.sid,args[4]==L"1");ReleaseMutex(mutex);}
    else result=ERROR_INVALID_PARAMETER;
}catch(DWORD error){result=error;}catch(...){result=ERROR_UNHANDLED_EXCEPTION;}CoUninitialize();return (int)result;}
#endif
