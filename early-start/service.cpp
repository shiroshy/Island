#include "common.h"
#include <wtsapi32.h>
#include <userenv.h>
#include <taskschd.h>
#include <map>
#include <set>
#include <atomic>

using namespace early;
namespace {
Config config;std::wstring directory,journal,name;
SERVICE_STATUS_HANDLE statusHandle=nullptr;SERVICE_STATUS status{};
Handle wake(CreateEventW(nullptr,FALSE,FALSE,nullptr));std::atomic<bool> stopping=false;
// The control callback only records IDs, without COM, I/O, locks or process launch.
volatile LONG pending[16]={};
void enqueue(DWORD session){if(!session||session>MAXLONG)return;for(auto& entry:pending)if(InterlockedCompareExchange(&entry,(LONG)session,0)==0)break;if(wake.h)SetEvent(wake);}
void report(DWORD state,DWORD error=NO_ERROR){status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;status.dwCurrentState=state;status.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN|SERVICE_ACCEPT_SESSIONCHANGE:0;status.dwWin32ExitCode=error;status.dwWaitHint=state==SERVICE_START_PENDING?5000:0;SetServiceStatus(statusHandle,&status);}
DWORD WINAPI control(DWORD code,DWORD event,LPVOID data,LPVOID){if(code==SERVICE_CONTROL_STOP||code==SERVICE_CONTROL_SHUTDOWN){stopping=true;SetEvent(wake);return NO_ERROR;}if(code==SERVICE_CONTROL_SESSIONCHANGE&&event==WTS_SESSION_LOGON&&data){auto notification=(WTSSESSION_NOTIFICATION*)data;if(notification->cbSize>=sizeof(*notification))enqueue(notification->dwSessionId);return NO_ERROR;}return code==SERVICE_CONTROL_INTERROGATE?NO_ERROR:ERROR_CALL_NOT_IMPLEMENTED;}
enum class Intent {Enabled,Disabled,Unavailable};
Intent enabled(){
    Com<ITaskService> scheduler;VARIANT empty{};if(FAILED(CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(scheduler.put())))||FAILED(scheduler->Connect(empty,empty,empty,empty)))return Intent::Unavailable;
    Com<ITaskFolder> root;Com<IRegisteredTask> task;if(FAILED(scheduler->GetFolder(Bstr(L"\\"),root.put())))return Intent::Unavailable;
    HRESULT found=root->GetTask(Bstr(L"OwnIsland.Logon."+config.sid),task.put());if(found==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))return Intent::Disabled;if(FAILED(found))return Intent::Unavailable;
    VARIANT_BOOL on=VARIANT_FALSE;if(FAILED(task->get_Enabled(&on)))return Intent::Unavailable;if(on==VARIANT_FALSE)return Intent::Disabled;
    Com<ITaskDefinition> definition;Com<IActionCollection> actions;Com<IAction> action;Com<IExecAction> exec;Com<IPrincipal> principal;
    if(FAILED(task->get_Definition(definition.put()))||FAILED(definition->get_Actions(actions.put())))return Intent::Unavailable;
    LONG count=0;actions->get_Count(&count);if(count!=1||FAILED(actions->get_Item(1,action.put()))||FAILED(action->QueryInterface(IID_PPV_ARGS(exec.put()))))return Intent::Disabled;
    BSTR path=nullptr,args=nullptr;exec->get_Path(&path);exec->get_Arguments(&args);bool own=path&&args&&same(path,config.launcher)&&!wcscmp(args,L"--logon --silent");SysFreeString(path);SysFreeString(args);
    if(FAILED(definition->get_Principal(principal.put())))return Intent::Unavailable;TASK_LOGON_TYPE logon{};TASK_RUNLEVEL_TYPE level{};principal->get_LogonType(&logon);principal->get_RunLevel(&level);
    BSTR user=nullptr;principal->get_UserId(&user);own=own&&accountMatches(user,config.sid);SysFreeString(user);
    return own&&logon==TASK_LOGON_INTERACTIVE_TOKEN&&level==TASK_RUNLEVEL_LUA?Intent::Enabled:Intent::Disabled;
}
void warm(){
    SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN);auto end=GetTickCount64()+1500;DWORD budget=16*1024*1024,total=0;BYTE bytes[65536];auto root=std::filesystem::path(config.launcher).parent_path();
    for(auto file:{L"Island.Start.exe",L"Island.exe",L"Island.Desktop.exe",L"Island.Files.exe"}){
        if(stopping||GetTickCount64()>end||!budget)break;auto path=(root/file).wstring();Handle input(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(input.h==INVALID_HANDLE_VALUE)continue;
        BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(input,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))continue;
        DWORD read=0;while(!stopping&&budget&&GetTickCount64()<end&&ReadFile(input,bytes,std::min<DWORD>(budget,sizeof(bytes)),&read,nullptr)&&read){budget-=read;total+=read;}
    }
    SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_END);log(journal,L"file-cache-warmed",0,0,total);
}
bool launch(DWORD session,HANDLE& helper){
    Handle original,token;if(!WTSQueryUserToken(session,&original.h))return false;
    if(!same(sidOf(original),config.sid))return true;
    if(!limited(original,token)||!userToken(token,config.sid,session)){log(journal,L"token-rejected",ERROR_ACCESS_DENIED,session);return true;}
    auto intent=enabled();if(intent==Intent::Unavailable)return false;if(intent==Intent::Disabled){log(journal,L"autostart-disabled",0,session);return true;}
    WTS_CONNECTSTATE_CLASS* connection=nullptr;DWORD connectionBytes=0;bool active=WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSConnectState,(LPWSTR*)&connection,&connectionBytes)&&connectionBytes>=sizeof(WTS_CONNECTSTATE_CLASS)&&*connection==WTSActive;if(connection)WTSFreeMemory(connection);if(!active)return false;
    LPVOID environment=nullptr;if(!CreateEnvironmentBlock(&environment,token,FALSE))return false;
    bool profile=false;for(auto entry=(wchar_t*)environment;*entry;entry+=lstrlenW(entry)+1)if(!_wcsnicmp(entry,L"LOCALAPPDATA=",13)&&entry[13]){profile=true;break;}if(!profile||stopping){DestroyEnvironmentBlock(environment);return !profile?false:true;}
    auto program=module(),command=quote(program)+L" --user-start";STARTUPINFOW start{sizeof(start)};start.lpDesktop=(LPWSTR)L"winsta0\\default";start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION child{};
    bool launched=CreateProcessAsUserW(token,program.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,environment,directory.c_str(),&start,&child)!=FALSE;DWORD error=launched?0:GetLastError();DestroyEnvironmentBlock(environment);
    if(launched){log(journal,L"user-helper-launched-medium",0,session,child.dwProcessId);CloseHandle(child.hThread);helper=child.hProcess;}else log(journal,L"user-helper-failed",error,session);
    return launched;
}
void WINAPI serviceMain(DWORD,LPWSTR*){
    statusHandle=RegisterServiceCtrlHandlerExW(name.c_str(),control,nullptr);if(!statusHandle)return;report(SERVICE_START_PENDING);
    if(!config.load(directory+L"\\service.ini")||!wake.h){report(SERVICE_STOPPED,ERROR_INVALID_DATA);return;}
    report(SERVICE_RUNNING);CoInitializeEx(nullptr,COINIT_MULTITHREADED);log(journal,L"service-ready");
    PWTS_SESSION_INFOW sessions=nullptr;DWORD count=0;if(WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE,0,1,&sessions,&count)){for(DWORD i=0;i<count;i++)if(sessions[i].SessionId&&sessions[i].State==WTSActive)enqueue(sessions[i].SessionId);WTSFreeMemory(sessions);}
    auto stamp=[](DWORD session){WTSINFOW* info=nullptr;DWORD bytes=0;LONGLONG time=0;if(WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSSessionInfo,(LPWSTR*)&info,&bytes)&&bytes>=sizeof(WTSINFOW))time=info->LogonTime.QuadPart;if(info)WTSFreeMemory(info);return time;};
    if(enabled()==Intent::Enabled)warm();else log(journal,L"warm-skipped-autostart-unavailable-or-disabled");std::map<DWORD,LONGLONG> processed;struct Retry{ULONGLONG end=0,next=0;HANDLE helper=nullptr;unsigned attempts=0;};std::map<DWORD,Retry> retries;
    while(!stopping){
        for(auto& entry:pending){DWORD session=(DWORD)InterlockedExchange(&entry,0);if(session&&(!processed.contains(session)||processed[session]!=stamp(session))&&!retries.contains(session))retries[session]={GetTickCount64()+120000,0};}
        for(auto it=retries.begin();it!=retries.end();){if(stopping)break;auto now=GetTickCount64();auto& retry=it->second;bool complete=false;
            if(retry.helper){if(WaitForSingleObject(retry.helper,0)==WAIT_TIMEOUT&&now<retry.end){++it;continue;}DWORD code=ERROR_TIMEOUT;if(WaitForSingleObject(retry.helper,0)==WAIT_OBJECT_0)GetExitCodeProcess(retry.helper,&code);CloseHandle(retry.helper);retry.helper=nullptr;log(journal,L"user-start-result",code,it->first);complete=code==0;retry.next=now+1000;}
            if(!complete&&now>=retry.next&&now<retry.end&&retry.attempts<2){bool accepted=launch(it->first,retry.helper);if(retry.helper)++retry.attempts;complete=accepted&&!retry.helper;retry.next=now+500;}
            if(complete||now>=retry.end||(!retry.helper&&retry.attempts>=2)){if(!complete)log(journal,L"session-launch-timeout",ERROR_TIMEOUT,it->first);processed[it->first]=stamp(it->first);it=retries.erase(it);}else ++it;
        }
        WaitForSingleObject(wake,retries.empty()?INFINITE:200);
    }
    for(auto& [session,retry]:retries)if(retry.helper)CloseHandle(retry.helper);
    log(journal,L"service-stopped-ui-untouched");CoUninitialize();report(SERVICE_STOPPED);
}
int userStart(){
    DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.h)||!userToken(token,config.sid,session))return ERROR_ACCESS_DENIED;
    wchar_t local[32768]={};if(!GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768))return ERROR_PATH_NOT_FOUND;auto folder=std::wstring(local)+L"\\OwnIsland";CreateDirectoryW(folder.c_str(),nullptr);auto file=folder+L"\\preload-start.log";log(file,L"user-helper-start",0,session);
    // This tiny user process can run while the secure sign-in desktop is active.
    // Integrated Island needs the native tray before it can arm restoration.
    // This distribution always needs Explorer, even with a legacy standalone profile.
    if(HWND panel=FindWindowW(L"OwnIsland.Surface",nullptr)){DWORD pid=0;GetWindowThreadProcessId(panel,&pid);Handle owner(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));wchar_t path[32768]={};DWORD length=32768;bool own=owner.h&&QueryFullProcessImageNameW(owner,0,path,&length)&&same(path,(std::filesystem::path(config.launcher).parent_path()/L"Island.exe").wstring());DWORD_PTR answer=0;bool ready=own&&GetPropW(panel,L"OwnIsland.StartupReady")&&GetPropW(panel,L"OwnIsland.ShellReady")&&SendMessageTimeoutW(panel,WM_NULL,0,0,SMTO_ABORTIFHUNG,150,&answer);log(file,ready?L"existing-ui-preserved":L"existing-ui-starting-or-busy",ready?0:ERROR_BUSY,session);return ready?0:ERROR_BUSY;}
    auto command=quote(config.launcher)+L" --logon --silent";STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION child{};auto root=std::filesystem::path(config.launcher).parent_path().wstring();
    if(!CreateProcessW(config.launcher.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&start,&child)){auto error=GetLastError();log(file,L"launcher-failed",error,session);return error;}
    log(file,L"launcher-started",0,session,child.dwProcessId);CloseHandle(child.hThread);DWORD result=ERROR_TIMEOUT;if(WaitForSingleObject(child.hProcess,90000)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&result);CloseHandle(child.hProcess);log(file,L"launcher-result",result,session);return result;
}
int selfTest(){Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&token.h))return 1;DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);auto sid=currentSid();Handle filtered;if(!validSid(sid)||!limited(token,filtered)||!userToken(filtered,sid,session)||userToken(filtered,L"S-1-5-21-1-2-3-4",session)||userToken(filtered,sid,session+1)||validSid(L"S-1-5-18")||localLauncher(L"C:\\*.exe")||localLauncher(L"\\\\server\\Island.Start.exe")||quote(L"C:\\path\\")!=L"\"C:\\path\\\\\"")return 2;return 0;}
}
int wmain(int argc,wchar_t** argv){SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);directory=std::filesystem::path(module()).parent_path().wstring();journal=directory+L"\\service.log";if(argc==2&&!wcscmp(argv[1],L"--self-test"))return selfTest();if(!config.load(directory+L"\\service.ini"))return ERROR_INVALID_DATA;name=serviceName(config.sid);if(argc==2&&!wcscmp(argv[1],L"--user-start"))return userStart();if(argc==2&&!wcscmp(argv[1],L"--check-intent")){CoInitializeEx(nullptr,COINIT_MULTITHREADED);auto intent=enabled();CoUninitialize();return intent==Intent::Enabled?0:intent==Intent::Disabled?ERROR_SERVICE_DISABLED:ERROR_NOT_READY;}SERVICE_TABLE_ENTRYW table[]={{name.data(),serviceMain},{nullptr,nullptr}};return StartServiceCtrlDispatcherW(table)?0:(int)GetLastError();}
