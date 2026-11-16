#include "common.h"
#include "service_hash.h"
#include <aclapi.h>
#include <bcrypt.h>
#include <shellapi.h>

using namespace early;
namespace {
struct Service {SC_HANDLE h=nullptr;~Service(){if(h)CloseServiceHandle(h);}operator SC_HANDLE()const{return h;}};
void require(bool ok,DWORD error=0){if(!ok)throw error?error:(GetLastError()?GetLastError():ERROR_INVALID_DATA);}
bool admin(){SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID sid=nullptr;BOOL member=FALSE;if(AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)){CheckTokenMembership(nullptr,sid,&member);FreeSid(sid);}return member!=FALSE;}
bool hashMatches(const std::wstring& path){
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(file.h==INVALID_HANDLE_VALUE)return false;BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;BYTE buffer[65536],digest[32];DWORD size=0;
    while(ok){if(!ReadFile(file,buffer,sizeof(buffer),&size,nullptr)){ok=false;break;}if(!size)break;ok=BCryptHashData(hash,buffer,size,0)>=0;}
    if(ok)ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
    wchar_t text[65]={};if(ok){for(int i=0;i<32;i++)swprintf(text+i*2,3,L"%02X",digest[i]);ok=same(text,ServiceSHA256);}return ok;
}
void protect(const std::wstring& path,bool folder){
    DWORD attributes=GetFileAttributesW(path.c_str());require(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),ERROR_REPARSE_TAG_INVALID);
    PSECURITY_DESCRIPTOR descriptor=nullptr;const wchar_t* sddl=folder?L"O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)":L"O:BAG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)";
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&descriptor,nullptr));PSID owner=nullptr;PACL acl=nullptr;BOOL defaulted=FALSE,present=FALSE;GetSecurityDescriptorOwner(descriptor,&owner,&defaulted);GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted);
    DWORD error=SetNamedSecurityInfoW((LPWSTR)path.c_str(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,owner,nullptr,acl,nullptr);LocalFree(descriptor);require(error==ERROR_SUCCESS,error);
}
void makeFolder(const std::wstring& path){if(!CreateDirectoryW(path.c_str(),nullptr))require(GetLastError()==ERROR_ALREADY_EXISTS);protect(path,true);}
bool ownedService(SC_HANDLE service,const std::wstring& program){DWORD size=0;QueryServiceConfigW(service,nullptr,0,&size);std::vector<BYTE> bytes(size);if(!size||!QueryServiceConfigW(service,(QUERY_SERVICE_CONFIGW*)bytes.data(),size,&size))return false;auto c=(QUERY_SERVICE_CONFIGW*)bytes.data();return same(c->lpBinaryPathName,quote(program))&&same(c->lpServiceStartName,L"LocalSystem")&&c->dwServiceType==SERVICE_WIN32_OWN_PROCESS;}
void stopService(SC_HANDLE service){SERVICE_STATUS_PROCESS state{};DWORD size=0;require(QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(LPBYTE)&state,sizeof(state),&size)!=FALSE);if(state.dwCurrentState==SERVICE_STOPPED)return;SERVICE_STATUS ignored{};if(!ControlService(service,SERVICE_CONTROL_STOP,&ignored))require(GetLastError()==ERROR_SERVICE_NOT_ACTIVE);auto end=GetTickCount64()+15000;do{require(QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(LPBYTE)&state,sizeof(state),&size)!=FALSE);if(state.dwCurrentState==SERVICE_STOPPED)return;Sleep(50);}while(GetTickCount64()<end);throw (DWORD)ERROR_TIMEOUT;}
std::wstring desktopSid(){HWND shell=GetShellWindow();DWORD pid=0,session=0,current=0;GetWindowThreadProcessId(shell,&pid);ProcessIdToSessionId(GetCurrentProcessId(),&current);if(!pid||!ProcessIdToSessionId(pid,&session)||session!=current)return {};Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!process.h)return {};wchar_t path[32768]={},windows[32768]={};DWORD length=32768;GetWindowsDirectoryW(windows,32768);if(!QueryFullProcessImageNameW(process,0,path,&length)||!same(path,std::wstring(windows)+L"\\explorer.exe"))return {};Handle token;if(!OpenProcessToken(process,TOKEN_QUERY,&token.h))return {};return sidOf(token);}
bool authorizedSid(const std::wstring& sid){return validSid(sid)&&(same(sid,currentSid())||(admin()&&same(sid,desktopSid())));}
int run(bool remove,const std::wstring& sid,const std::wstring& launcher){
    if(!authorizedSid(sid))return ERROR_ACCESS_DENIED;
    auto folder=installedDirectory(sid),program=folder+L"\\Island.Preload.exe",ini=folder+L"\\service.ini",name=serviceName(sid);if(folder.empty())return ERROR_PATH_NOT_FOUND;
    auto source=std::filesystem::path(module()).parent_path()/L"Island.Preload.exe";
    Service manager;manager.h=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT|(remove?0:SC_MANAGER_CREATE_SERVICE));require(manager.h!=nullptr);
    Service service;service.h=OpenServiceW(manager,name.c_str(),SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS|SERVICE_STOP|DELETE|SERVICE_START|SERVICE_CHANGE_CONFIG);
    if(service.h)require(ownedService(service,program),ERROR_ACCESS_DENIED);else require(GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST);
    if(remove){if(service.h){stopService(service);require(DeleteService(service)!=FALSE);}log(folder+L"\\install.log",L"removed-ui-untouched");return 0;}
    require(localLauncher(launcher)&&hashMatches(source.wstring()),ERROR_INVALID_DATA);
    if(service.h)stopService(service);
    auto parent=std::filesystem::path(folder).parent_path().wstring();makeFolder(parent);makeFolder(folder);
    if(GetFileAttributesW(program.c_str())!=INVALID_FILE_ATTRIBUTES)protect(program,false);
    auto temporary=folder+L"\\service-new-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".tmp";
    require(CopyFileW(source.c_str(),temporary.c_str(),TRUE)!=FALSE);
    try{protect(temporary,false);require(hashMatches(temporary),ERROR_CRC);require(MoveFileExW(temporary.c_str(),program.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE);}catch(...){DeleteFileW(temporary.c_str());throw;}
    protect(program,false);
    if(GetFileAttributesW(ini.c_str())!=INVALID_FILE_ATTRIBUTES)protect(ini,false);
    require(WritePrivateProfileStringW(L"startup",L"sid",sid.c_str(),ini.c_str())!=FALSE);require(WritePrivateProfileStringW(L"startup",L"launcher",launcher.c_str(),ini.c_str())!=FALSE);WritePrivateProfileStringW(nullptr,nullptr,nullptr,ini.c_str());protect(ini,false);
    auto command=quote(program);
    if(!service.h){service.h=CreateServiceW(manager,name.c_str(),L"Island Early Startup",SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS|SERVICE_STOP|SERVICE_START|SERVICE_CHANGE_CONFIG|DELETE,SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,SERVICE_ERROR_NORMAL,command.c_str(),nullptr,nullptr,nullptr,L"LocalSystem",nullptr);require(service.h!=nullptr);}
    else require(ChangeServiceConfigW(service,SERVICE_NO_CHANGE,SERVICE_AUTO_START,SERVICE_NO_CHANGE,command.c_str(),nullptr,nullptr,nullptr,nullptr,nullptr,nullptr)!=FALSE);
    SERVICE_DESCRIPTIONW description{(LPWSTR)L"Prepares Island files and starts the configured user's non-elevated UI after logon."};ChangeServiceConfig2W(service,SERVICE_CONFIG_DESCRIPTION,&description);
    require(StartServiceW(service,0,nullptr)!=FALSE);auto end=GetTickCount64()+10000;SERVICE_STATUS_PROCESS state{};DWORD bytes=0;do{require(QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(LPBYTE)&state,sizeof(state),&bytes)!=FALSE);if(state.dwCurrentState==SERVICE_RUNNING){log(folder+L"\\install.log",L"installed-running");return 0;}if(state.dwCurrentState==SERVICE_STOPPED)throw state.dwWin32ExitCode?state.dwWin32ExitCode:(DWORD)ERROR_SERVICE_NOT_ACTIVE;Sleep(50);}while(GetTickCount64()<end);return ERROR_TIMEOUT;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    bool remove=count>=2&&!wcscmp(args[1],L"--remove"),install=count>=2&&!wcscmp(args[1],L"--install");
    if(!install&&!remove){LocalFree(args);return ERROR_INVALID_PARAMETER;}
    std::wstring sid=currentSid(),launcher=(std::filesystem::path(module()).parent_path()/L"Island.Start.exe").wstring();bool valid=true;bool gotSid=false,gotLauncher=false;
    for(int i=2;i<count&&valid;i++){if(!wcscmp(args[i],L"--sid")&&i+1<count&&!gotSid){sid=args[++i];gotSid=true;}else if(!wcscmp(args[i],L"--launcher")&&i+1<count&&!gotLauncher){launcher=args[++i];gotLauncher=true;}else valid=false;}
    LocalFree(args);if(!valid||!authorizedSid(sid))return ERROR_ACCESS_DENIED;
    if(!admin()){
        auto parameters=std::wstring(remove?L"--remove":L"--install")+L" --sid "+quote(sid)+L" --launcher "+quote(launcher);auto self=module();SHELLEXECUTEINFOW execute{sizeof(execute)};execute.fMask=SEE_MASK_NOCLOSEPROCESS;execute.lpVerb=L"runas";execute.lpFile=self.c_str();execute.lpParameters=parameters.c_str();execute.nShow=SW_HIDE;
        if(!ShellExecuteExW(&execute))return GetLastError();Handle process(execute.hProcess);WaitForSingleObject(process,INFINITE);DWORD code=ERROR_GEN_FAILURE;GetExitCodeProcess(process,&code);return code;
    }
    try{return run(remove,sid,launcher);}catch(DWORD error){log(installedDirectory(sid)+L"\\install.log",L"install-failed",error);return (int)error;}catch(...){return ERROR_UNHANDLED_EXCEPTION;}
}
