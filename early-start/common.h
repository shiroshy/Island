#pragma once
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <filesystem>

namespace early {
struct Handle {HANDLE h=nullptr;Handle()=default;explicit Handle(HANDLE value):h(value){}~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;operator HANDLE()const{return h;}};
template<class T> struct Com {T* p=nullptr;~Com(){if(p)p->Release();}T** put(){return &p;}T* operator->()const{return p;}};
struct Bstr {BSTR p;Bstr(const std::wstring& s):p(SysAllocString(s.c_str())){}~Bstr(){SysFreeString(p);}operator BSTR()const{return p;}};
inline std::wstring module(){wchar_t text[32768]={};GetModuleFileNameW(nullptr,text,32768);return text;}
inline std::wstring sidOf(HANDLE token){DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);if(!size||!GetTokenInformation(token,TokenUser,bytes.data(),size,&size))return {};LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(((TOKEN_USER*)bytes.data())->User.Sid,&sid))return {};std::wstring text=sid;LocalFree(sid);return text;}
inline std::wstring currentSid(){Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.h))return {};return sidOf(token);}
inline bool same(const std::wstring& a,const std::wstring& b){return !_wcsicmp(a.c_str(),b.c_str());}
inline bool accountMatches(const wchar_t* account,const std::wstring& expected){if(!account)return false;if(same(account,expected))return true;DWORD bytes=0,domainSize=0;SID_NAME_USE use{};LookupAccountNameW(nullptr,account,nullptr,&bytes,nullptr,&domainSize,&use);if(!bytes)return false;std::vector<BYTE> resolved(bytes);std::vector<wchar_t> domain(domainSize?domainSize:1);if(!LookupAccountNameW(nullptr,account,resolved.data(),&bytes,domain.data(),&domainSize,&use))return false;LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(resolved.data(),&sid))return false;bool result=same(sid,expected);LocalFree(sid);return result;}
inline std::wstring quote(const std::wstring& s){std::wstring result=L"\"";size_t slashes=0;for(auto c:s){if(c==L'\\'){slashes++;continue;}if(c==L'\"'){result.append(slashes*2+1,L'\\');result+=c;}else{result.append(slashes,L'\\');result+=c;}slashes=0;}result.append(slashes*2,L'\\');return result+L"\"";}
inline bool validSid(const std::wstring& text){PSID sid=nullptr;bool ok=ConvertStringSidToSidW(text.c_str(),&sid)&&IsValidSid(sid);if(sid)LocalFree(sid);return ok&&text.rfind(L"S-1-5-21-",0)==0;}
inline std::wstring serviceName(const std::wstring& sid){return L"OwnIslandPreload_"+sid;}
inline std::wstring installedDirectory(const std::wstring& sid){PWSTR folder=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles,0,nullptr,&folder)))return {};std::wstring result=std::wstring(folder)+L"\\OwnIslandStartup\\"+sid;CoTaskMemFree(folder);return result;}
inline bool localLauncher(const std::wstring& path){if(path.size()<4||path[1]!=L':'||path[2]!=L'\\'||path.find_first_of(L"*?\r\n\"")!=std::wstring::npos||!same(std::filesystem::path(path).filename().wstring(),L"Island.Start.exe"))return false;auto attributes=GetFileAttributesW(path.c_str());return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));}
inline DWORD integrity(HANDLE token){DWORD size=0;GetTokenInformation(token,TokenIntegrityLevel,nullptr,0,&size);std::vector<BYTE> bytes(size);if(!size||!GetTokenInformation(token,TokenIntegrityLevel,bytes.data(),size,&size))return MAXDWORD;auto sid=((TOKEN_MANDATORY_LABEL*)bytes.data())->Label.Sid;auto count=*GetSidSubAuthorityCount(sid);return count?*GetSidSubAuthority(sid,count-1):MAXDWORD;}
inline bool userToken(HANDLE token,const std::wstring& sid,DWORD session){DWORD current=MAXDWORD,bytes=0;TOKEN_TYPE type{};return same(sidOf(token),sid)&&GetTokenInformation(token,TokenSessionId,&current,sizeof(current),&bytes)&&current==session&&session!=0&&integrity(token)<=SECURITY_MANDATORY_MEDIUM_RID&&GetTokenInformation(token,TokenType,&type,sizeof(type),&bytes)&&type==TokenPrimary;}
inline bool limited(HANDLE original,Handle& result){TOKEN_ELEVATION_TYPE type{};DWORD size=0;if(!GetTokenInformation(original,TokenElevationType,&type,sizeof(type),&size))return false;if(type==TokenElevationTypeFull){TOKEN_LINKED_TOKEN linked{};if(!GetTokenInformation(original,TokenLinkedToken,&linked,sizeof(linked),&size))return false;result.h=linked.LinkedToken;return true;}return DuplicateTokenEx(original,MAXIMUM_ALLOWED,nullptr,SecurityImpersonation,TokenPrimary,&result.h)!=FALSE;}
struct Config {
    std::wstring sid,launcher;
    bool load(const std::wstring& file){wchar_t user[256]={},program[32768]={};GetPrivateProfileStringW(L"startup",L"sid",L"",user,256,file.c_str());GetPrivateProfileStringW(L"startup",L"launcher",L"",program,32768,file.c_str());sid=user;launcher=program;return validSid(sid)&&localLauncher(launcher);}
};
inline void log(const std::wstring& file,const wchar_t* stage,DWORD error=0,DWORD session=0,DWORD value=0){WIN32_FILE_ATTRIBUTE_DATA a{};if(GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&a)&&a.nFileSizeLow>65536)MoveFileExW(file.c_str(),(file+L".previous").c_str(),MOVEFILE_REPLACE_EXISTING);Handle h(CreateFileW(file.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(h.h==INVALID_HANDLE_VALUE)return;BY_HANDLE_FILE_INFORMATION target{};if(!GetFileInformationByHandle(h,&target)||(target.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return;wchar_t text[240];int count=swprintf(text,240,L"tick=%llu pid=%lu %ls error=%lu session=%lu value=%lu\r\n",GetTickCount64(),GetCurrentProcessId(),stage,error,session,value);char utf8[500];int size=WideCharToMultiByte(CP_UTF8,0,text,count,utf8,sizeof(utf8),nullptr,nullptr);DWORD written=0;if(size>0)WriteFile(h,utf8,size,&written,nullptr);}
}
