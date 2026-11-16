#pragma once
#include "support.h"
#include "../../src/settings_catalog.h"
#include "package_manifest.h"

namespace setup {
inline std::wstring nativeTarget(){return windows()+L"\\SystemResources\\Windows.UI.SettingsAppThreshold\\Windows.UI.SettingsAppThreshold.pri";}
inline std::wstring nativeKey(const wchar_t* id){return std::wstring(L"SOFTWARE\\Microsoft\\SystemSettings\\SettingId\\")+id;}
inline std::vector<std::wstring> managedIds(const std::wstring& root){
    std::vector<std::wstring> ids;for(auto& def:island_settings::definitions)ids.emplace_back(def.keyId);
    auto previous=ini(root+L"\\installed.ini",L"setting-ids");size_t start=0;
    while(start<previous.size()){auto end=previous.find(L';',start);auto id=previous.substr(start,end==std::wstring::npos?end:end-start);check(!id.empty()&&id.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::wstring::npos,ERROR_INVALID_DATA);if(std::find(ids.begin(),ids.end(),id)==ids.end())ids.push_back(id);if(end==std::wstring::npos)break;start=end+1;}
    return ids;
}
inline std::wstring packagedIds(){std::wstring ids;for(auto& def:island_settings::definitions){if(!ids.empty())ids+=L';';ids+=def.keyId;}return ids;}
inline const wchar_t* typeName(island_settings::Kind kind){using K=island_settings::Kind;return kind==K::Boolean?L"Boolean":kind==K::Range?L"Range":kind==K::List?L"List":kind==K::DisplayString?L"DisplayString":L"Action";}
inline bool ownKey(const wchar_t* id,const std::wstring& handler){HKEY key=nullptr;auto error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,nativeKey(id).c_str(),0,KEY_QUERY_VALUE,&key);if(error==ERROR_FILE_NOT_FOUND)return false;check(error==0,error);wchar_t path[32768]={};DWORD size=sizeof(path),type=0;error=RegQueryValueExW(key,L"DllPath",nullptr,&type,(BYTE*)path,&size);RegCloseKey(key);check(error==0&&type==REG_SZ&&size<=sizeof(path)&&same(path,handler),ERROR_INVALID_OWNER);return true;}
inline void writeKeyType(const wchar_t* id,const wchar_t* type,const std::wstring& handler){HKEY key=nullptr;DWORD disposition=0;auto error=RegCreateKeyExW(HKEY_LOCAL_MACHINE,nativeKey(id).c_str(),0,nullptr,
#ifdef ISLAND_SETUP_FIXTURE
0,KEY_SET_VALUE
#else
REG_OPTION_BACKUP_RESTORE,KEY_SET_VALUE
#endif
,nullptr,&key,&disposition);check(error==0,error);for(auto value:{std::pair{L"DllPath",handler.c_str()},std::pair{L"Type",type},std::pair{L"SyncType",L"None"}}){error=RegSetValueExW(key,value.first,0,REG_SZ,(const BYTE*)value.second,(DWORD)((wcslen(value.second)+1)*sizeof(wchar_t)));if(error)break;}RegCloseKey(key);check(error==0,error);}
inline void writeKey(const island_settings::Definition& def,const std::wstring& handler){writeKeyType(def.keyId,typeName(def.kind),handler);}
inline void eraseKey(const wchar_t* id,const std::wstring& handler){if(!ownKey(id,handler))return;auto path=nativeKey(id);
#ifdef ISLAND_SETUP_FIXTURE
check(RegDeleteKeyW(HKEY_LOCAL_MACHINE,path.c_str())==0);return;
#endif
HKEY key=nullptr;check(RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,WRITE_DAC,&key)==0);PSECURITY_DESCRIPTOR descriptor=nullptr;check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;KA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr));auto error=RegSetKeySecurity(key,DACL_SECURITY_INFORMATION,descriptor);LocalFree(descriptor);RegCloseKey(key);check(error==0,error);error=RegDeleteKeyW(HKEY_LOCAL_MACHINE,path.c_str());check(error==0,error);}
inline void supportedOS(){
#ifdef ISLAND_SETUP_FIXTURE
check(!fixtureWindows.empty());return;
#endif
using RtlVersion=LONG(WINAPI*)(OSVERSIONINFOW*);auto rtl=(RtlVersion)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion");OSVERSIONINFOW version{sizeof(version)};check(rtl&&rtl(&version)==0&&version.dwMajorVersion==10&&version.dwBuildNumber==19045,ERROR_REVISION_MISMATCH);DWORD revision=0,size=sizeof(revision);check(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",L"UBR",RRF_RT_REG_DWORD,nullptr,&revision,&size)==0&&revision==6456,ERROR_REVISION_MISMATCH);check(hash(windows()+L"\\ImmersiveControlPanel\\SystemSettings.exe")==SystemSettingsSHA,ERROR_REVISION_MISMATCH);}
inline bool ownedInstall(const std::wstring& root){return ini(root+L"\\installed.ini",L"product")==ProductTag||ini(root+L"\\transaction.ini",L"product")==ProductTag;}
inline std::wstring originalFile(const std::wstring& root){return root+L"\\recovery\\"+std::wstring(OriginalPriSHA)+L".pri";}
inline void preflightNative(const std::wstring& root){supportedOS();auto handler=root+L"\\Island.SettingsHandler.dll";for(auto& id:managedIds(root))ownKey(id.c_str(),handler);auto current=hash(nativeTarget());if(current!=OriginalPriSHA){auto previous=ini(root+L"\\installed.ini",L"resource-hash");auto pending=ini(root+L"\\transaction.ini",L"resource-hash");check(ownedInstall(root)&&((!previous.empty()&&current==previous)||(!pending.empty()&&current==pending)),ERROR_INVALID_OWNER);check(hash(originalFile(root))==OriginalPriSHA,ERROR_CRC);}check(!settingsRunning(),ERROR_BUSY);}
inline Bytes security(const std::wstring& target){PSECURITY_DESCRIPTOR descriptor=nullptr;auto error=GetNamedSecurityInfoW((LPWSTR)target.c_str(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr,&descriptor);check(error==0,error);auto size=GetSecurityDescriptorLength(descriptor);Bytes result((BYTE*)descriptor,(BYTE*)descriptor+size);LocalFree(descriptor);return result;}
inline void restoreSecurity(const std::wstring& target,const Bytes& descriptor){check(!descriptor.empty()&&IsValidSecurityDescriptor((PSECURITY_DESCRIPTOR)descriptor.data()),ERROR_INVALID_SECURITY_DESCR);check(SetFileSecurityW(target.c_str(),OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,(PSECURITY_DESCRIPTOR)descriptor.data())!=FALSE);}
inline void replaceNative(const Bytes& data,const std::wstring& expected){
    auto target=nativeTarget();check(hash(target)==expected,ERROR_REVISION_MISMATCH);
#ifdef ISLAND_SETUP_FIXTURE
write(target,data,false);return;
#endif
auto descriptor=security(target);auto temporary=target+L".Island."+std::to_wstring(GetCurrentProcessId())+L".tmp",previous=temporary+L".previous";check(!exists(temporary)&&!exists(previous),ERROR_ALREADY_EXISTS);
    write(temporary,data,false);bool accessChanged=false,replaced=false;
    try{
        PSID admins=nullptr;check(ConvertStringSidToSidW(L"S-1-5-32-544",&admins));auto error=SetNamedSecurityInfoW((LPWSTR)target.c_str(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,admins,nullptr,nullptr,nullptr);LocalFree(admins);check(error==0,error);accessChanged=true;
        PSECURITY_DESCRIPTOR writable=nullptr;check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;FA;;;SY)(A;;FA;;;BA)",SDDL_REVISION_1,&writable,nullptr));BOOL present=FALSE,defaulted=FALSE;PACL acl=nullptr;GetSecurityDescriptorDacl(writable,&present,&acl,&defaulted);error=SetNamedSecurityInfoW((LPWSTR)target.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);LocalFree(writable);check(error==0,error);
        check(hash(target)==expected,ERROR_REVISION_MISMATCH);check(ReplaceFileW(target.c_str(),temporary.c_str(),previous.c_str(),REPLACEFILE_IGNORE_MERGE_ERRORS,nullptr,nullptr)!=FALSE);replaced=true;check(hash(previous)==expected,ERROR_REVISION_MISMATCH);restoreSecurity(target,descriptor);restoreSecurity(previous,descriptor);check(hash(target)==sha(data),ERROR_CRC);DeleteFileW(previous.c_str());
    }catch(...){if(replaced&&exists(previous)){ReplaceFileW(target.c_str(),previous.c_str(),nullptr,REPLACEFILE_IGNORE_MERGE_ERRORS,nullptr,nullptr);}if(accessChanged){try{restoreSecurity(target,descriptor);}catch(...){}}DeleteFileW(temporary.c_str());throw;}
}
struct NativeTransaction {
    std::wstring root,priorHash;Bytes prior;std::map<std::wstring,std::wstring> oldKeys;bool changed=false;
    explicit NativeTransaction(std::wstring value):root(std::move(value)){}
    void apply(){
        preflightNative(root);privilege(SE_TAKE_OWNERSHIP_NAME);privilege(SE_RESTORE_NAME);auto target=nativeTarget();prior=read(target);priorHash=sha(prior);auto handler=root+L"\\Island.SettingsHandler.dll";
        for(auto& id:managedIds(root))if(ownKey(id.c_str(),handler)){wchar_t type[64]={};DWORD bytes=sizeof(type);check(RegGetValueW(HKEY_LOCAL_MACHINE,nativeKey(id.c_str()).c_str(),L"Type",RRF_RT_REG_SZ,nullptr,type,&bytes)==0);oldKeys[id]=type;}
        auto backup=originalFile(root);if(priorHash==OriginalPriSHA){folder(root+L"\\recovery");if(exists(backup))check(hash(backup)==OriginalPriSHA,ERROR_CRC);else write(backup,prior);auto sd=backup+L".sd";if(!exists(sd))write(sd,security(target));}check(hash(backup)==OriginalPriSHA,ERROR_CRC);
        auto original=read(backup),output=patch(original,resource(SettingsPatchResource));check(sha(output)==PatchedPriSHA,ERROR_CRC);
        if(priorHash!=PatchedPriSHA)replaceNative(output,priorHash);changed=true;
        for(auto& def:island_settings::definitions)writeKey(def,handler);
        for(auto& [id,type]:oldKeys){bool current=false;for(auto& def:island_settings::definitions)if(id==def.keyId)current=true;if(!current)eraseKey(id.c_str(),handler);}
    }
    void rollback(){if(!changed)return;auto handler=root+L"\\Island.SettingsHandler.dll";if(hash(nativeTarget())==PatchedPriSHA&&sha(prior)!=PatchedPriSHA)replaceNative(prior,PatchedPriSHA);for(auto& id:managedIds(root))eraseKey(id.c_str(),handler);for(auto& [id,type]:oldKeys)writeKeyType(id.c_str(),type.c_str(),handler);changed=false;}
};
inline void removeNative(const std::wstring& root){auto handler=root+L"\\Island.SettingsHandler.dll";for(auto& id:managedIds(root))ownKey(id.c_str(),handler);check(!settingsRunning(),ERROR_BUSY);privilege(SE_TAKE_OWNERSHIP_NAME);privilege(SE_RESTORE_NAME);auto current=hash(nativeTarget());auto installed=ini(root+L"\\installed.ini",L"resource-hash");if(current==installed){auto backup=read(originalFile(root));check(sha(backup)==ini(root+L"\\installed.ini",L"original-hash"),ERROR_CRC);replaceNative(backup,current);} // A Windows update is never overwritten with an older resource.
    for(auto& id:managedIds(root))eraseKey(id.c_str(),handler);
}
}
