#pragma once
#include "../../early-start/common.h"
#include <aclapi.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <stdexcept>
#include <exception>
#include <algorithm>
#include <map>
#include <functional>

namespace setup {
using namespace early;
using Bytes=std::vector<BYTE>;
constexpr wchar_t ProductTag[]=L"OwnIsland.Setup.v1";
constexpr wchar_t UninstallKey[]=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OwnIsland";
inline void check(bool value,DWORD error=0){if(!value)throw error?error:(GetLastError()?GetLastError():ERROR_INVALID_DATA);}
#ifdef ISLAND_SETUP_FIXTURE
inline std::wstring fixtureWindows,fixtureRoot;
inline bool fixtureStartup=false,fixtureService=false,fixtureFailService=false;
#endif
inline std::wstring windows(){
#ifdef ISLAND_SETUP_FIXTURE
    check(!fixtureWindows.empty(),ERROR_INVALID_PARAMETER);return fixtureWindows;
#else
    wchar_t path[32768]={};check(GetWindowsDirectoryW(path,32768)!=0);return path;
#endif
}
inline std::wstring targetRoot(){
#ifdef ISLAND_SETUP_FIXTURE
check(!fixtureRoot.empty());return fixtureRoot;
#endif
PWSTR path=nullptr;check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles,0,nullptr,&path)),ERROR_PATH_NOT_FOUND);auto result=std::wstring(path)+L"\\Island";CoTaskMemFree(path);return result;}
inline bool exists(const std::wstring& path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
inline void noLink(const std::wstring& path){auto attributes=GetFileAttributesW(path.c_str());check(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),ERROR_REPARSE_TAG_INVALID);}
inline Bytes read(const std::wstring& path,size_t maximum=32*1024*1024){Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));check(file.h!=INVALID_HANDLE_VALUE);BY_HANDLE_FILE_INFORMATION info{};check(GetFileInformationByHandle(file,&info)!=FALSE);check(!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),ERROR_REPARSE_TAG_INVALID);LARGE_INTEGER size{};check(GetFileSizeEx(file,&size)&&size.QuadPart>=0&&(ULONGLONG)size.QuadPart<=maximum,ERROR_FILE_TOO_LARGE);Bytes data((size_t)size.QuadPart);DWORD count=0;check(data.empty()||(ReadFile(file,data.data(),(DWORD)data.size(),&count,nullptr)&&count==data.size()));return data;}
inline std::wstring sha(const Bytes& data){BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;BYTE digest[32]{};check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0);bool ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0&&BCryptHashData(hash,(PUCHAR)data.data(),(ULONG)data.size(),0)>=0&&BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);check(ok,ERROR_CRC);wchar_t text[65]={};for(int i=0;i<32;i++)swprintf(text+i*2,3,L"%02X",digest[i]);return text;}
inline std::wstring hash(const std::wstring& file){return sha(read(file));}
inline void protect(const std::wstring& path,bool directory){noLink(path);
#ifdef ISLAND_SETUP_FIXTURE
return;
#endif
PSECURITY_DESCRIPTOR descriptor=nullptr;check(ConvertStringSecurityDescriptorToSecurityDescriptorW(directory?L"O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)":L"O:BAG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)",SDDL_REVISION_1,&descriptor,nullptr));PSID owner=nullptr;PACL acl=nullptr;BOOL present=FALSE,defaulted=FALSE;GetSecurityDescriptorOwner(descriptor,&owner,&defaulted);GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted);auto error=SetNamedSecurityInfoW((LPWSTR)path.c_str(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,owner,nullptr,acl,nullptr);LocalFree(descriptor);check(error==0,error);}
inline void folder(const std::wstring& path){if(!CreateDirectoryW(path.c_str(),nullptr))check(GetLastError()==ERROR_ALREADY_EXISTS);protect(path,true);}
inline void write(const std::wstring& path,const Bytes& data,bool secure=true){noLink(std::filesystem::path(path).parent_path().wstring());if(exists(path))noLink(path);auto temporary=path+L".new."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));check(file.h!=INVALID_HANDLE_VALUE);DWORD count=0;bool ok=data.empty()||(WriteFile(file,data.data(),(DWORD)data.size(),&count,nullptr)&&count==data.size());if(ok)ok=FlushFileBuffers(file)!=FALSE;CloseHandle(file.h);file.h=nullptr;if(!ok){DeleteFileW(temporary.c_str());throw (DWORD)ERROR_WRITE_FAULT;}try{if(secure)protect(temporary,false);check(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE);}catch(...){DeleteFileW(temporary.c_str());throw;}}
inline std::wstring ini(const std::wstring& file,const wchar_t* key){wchar_t value[32768]={};GetPrivateProfileStringW(L"install",key,L"",value,32768,file.c_str());return value;}
inline void put(const std::wstring& file,const wchar_t* key,const std::wstring& value){if(exists(file))noLink(file);check(WritePrivateProfileStringW(L"install",key,value.c_str(),file.c_str())!=FALSE);}
inline Bytes resource(int id){auto module=GetModuleHandleW(nullptr);auto item=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);check(item!=nullptr,ERROR_RESOURCE_DATA_NOT_FOUND);auto size=SizeofResource(module,item);auto data=(BYTE*)LockResource(LoadResource(module,item));check(data&&size,ERROR_RESOURCE_DATA_NOT_FOUND);return Bytes(data,data+size);}
inline Bytes patch(const Bytes& source,const Bytes& delta){check(delta.size()>=20&&!memcmp(delta.data(),"ISLPRI01",8),ERROR_INVALID_DATA);size_t position=8;auto integer=[&](){check(position+4<=delta.size(),ERROR_INVALID_DATA);DWORD value=0;memcpy(&value,delta.data()+position,4);position+=4;return value;};auto original=integer(),length=integer(),count=integer();check(original==source.size()&&length<=16*1024*1024&&count<=100000,ERROR_INVALID_DATA);Bytes result;result.reserve(length);for(DWORD i=0;i<count;i++){check(position<delta.size(),ERROR_INVALID_DATA);BYTE kind=delta[position++];if(kind==0){auto offset=integer(),size=integer();check(offset<=source.size()&&size<=source.size()-offset&&size<=length-result.size(),ERROR_INVALID_DATA);result.insert(result.end(),source.begin()+offset,source.begin()+offset+size);}else{check(kind==1,ERROR_INVALID_DATA);auto size=integer();check(size<=delta.size()-position&&size<=length-result.size(),ERROR_INVALID_DATA);result.insert(result.end(),delta.begin()+position,delta.begin()+position+size);position+=size;}}check(result.size()==length&&position==delta.size(),ERROR_INVALID_DATA);return result;}
inline bool admin(){
#ifdef ISLAND_SETUP_FIXTURE
return true;
#endif
SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID group=nullptr;BOOL enabled=FALSE;if(AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&group)){CheckTokenMembership(nullptr,group,&enabled);FreeSid(group);}return enabled!=FALSE;}
inline void privilege(const wchar_t* name){
#ifdef ISLAND_SETUP_FIXTURE
return;
#endif
Handle token;check(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token.h)!=FALSE);TOKEN_PRIVILEGES p{};p.PrivilegeCount=1;check(LookupPrivilegeValueW(nullptr,name,&p.Privileges[0].Luid)!=FALSE);p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;SetLastError(0);check(AdjustTokenPrivileges(token,FALSE,&p,0,nullptr,nullptr)&&GetLastError()==0);}
inline bool settingsRunning(){
#ifdef ISLAND_SETUP_FIXTURE
return false;
#endif
Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));check(snapshot.h!=INVALID_HANDLE_VALUE);PROCESSENTRY32W process{sizeof(process)};if(Process32FirstW(snapshot,&process))do{if(!_wcsicmp(process.szExeFile,L"SystemSettings.exe"))return true;}while(Process32NextW(snapshot,&process));return false;}
struct Owner {
    Handle original,token;std::wstring sid;DWORD session=0;
    void load(){
#ifdef ISLAND_SETUP_FIXTURE
sid=L"S-1-5-21-100-200-300-1001";session=1;return;
#endif
DWORD pid=0;GetWindowThreadProcessId(GetShellWindow(),&pid);DWORD ownSession=0;ProcessIdToSessionId(GetCurrentProcessId(),&ownSession);check(pid&&ProcessIdToSessionId(pid,&session)&&session==ownSession,ERROR_NO_SUCH_LOGON_SESSION);Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));check(process.h!=nullptr);wchar_t path[32768]={};DWORD length=32768;check(QueryFullProcessImageNameW(process,0,path,&length)&&same(path,windows()+L"\\explorer.exe"),ERROR_INVALID_OWNER);check(OpenProcessToken(process,TOKEN_QUERY|TOKEN_DUPLICATE,&original.h)!=FALSE);sid=sidOf(original);check(limited(original,token)&&userToken(token,sid,session),ERROR_ACCESS_DENIED);}
    DWORD execute(const std::wstring& file,const std::wstring& args,DWORD timeout=30000){
#ifdef ISLAND_SETUP_FIXTURE
if(args==L"--user-probe")return fixtureStartup?10:0;
if(args==L"--configure-autostart on")fixtureStartup=true;
if(args==L"--configure-autostart off")fixtureStartup=false;
return 0;
#endif
check(userToken(token,sid,session),ERROR_ACCESS_DENIED);LPVOID environment=nullptr;check(CreateEnvironmentBlock(&environment,token,FALSE)!=FALSE);auto command=quote(file)+L" "+args;auto directory=std::filesystem::path(file).parent_path().wstring();STARTUPINFOW start{sizeof(start)};start.lpDesktop=(LPWSTR)L"winsta0\\default";start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};BOOL launched=CreateProcessWithTokenW(token,0,file.c_str(),command.data(),CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,environment,directory.c_str(),&start,&process);auto error=GetLastError();DestroyEnvironmentBlock(environment);check(launched!=FALSE,error);Handle running(process.hProcess),thread(process.hThread);check(WaitForSingleObject(running,timeout)==WAIT_OBJECT_0,ERROR_TIMEOUT);DWORD code=1;check(GetExitCodeProcess(running,&code)!=FALSE);return code;}
};
inline DWORD execute(const std::wstring& file,const std::wstring& args,DWORD timeout=30000){
#ifdef ISLAND_SETUP_FIXTURE
if(args.rfind(L"--install",0)==0){if(fixtureFailService){fixtureFailService=false;return ERROR_GEN_FAILURE;}fixtureService=true;}
if(args.rfind(L"--remove",0)==0)fixtureService=false;
return 0;
#endif
auto command=quote(file)+L" "+args;STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};auto directory=std::filesystem::path(file).parent_path().wstring();check(CreateProcessW(file.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&start,&process)!=FALSE);Handle child(process.hProcess),thread(process.hThread);check(WaitForSingleObject(child,timeout)==WAIT_OBJECT_0,ERROR_TIMEOUT);DWORD code=1;GetExitCodeProcess(child,&code);return code;}
inline std::wstring errorText(DWORD code){if(code==ERROR_INSTALL_ALREADY_RUNNING)return L"Уже запущен установщик Island. Закройте другое окно установки.";if(code==ERROR_REVISION_MISMATCH)return L"Эта версия Параметров Windows не поддерживается.";if(code==ERROR_BUSY)return L"Закройте Параметры и окна Island. Дождитесь окончания операций с файлами.";if(code==ERROR_INVALID_OWNER||code==ERROR_ALREADY_EXISTS)return L"Обнаружена другая установка Island. Её файлы и настройки не изменены.";if(code==ERROR_DEPENDENT_SERVICES_RUNNING)return L"Сначала удалите прежнюю интеграцию Island в Параметры её штатным средством восстановления.";if(code==ERROR_PRODUCT_VERSION)return L"Обнаружена установленная версия Island. Для смены варианта сначала удалите её через Приложения и возможности. Понижение версии не поддерживается.";if(code==ERROR_NOT_FOUND)return L"Подтверждённые остатки старой версии не найдены.";if(code==ERROR_CANCELLED)return L"Установка отменена.";wchar_t value[512]={};FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,value,512,nullptr);return *value?value:L"Не удалось завершить установку.";}
}
