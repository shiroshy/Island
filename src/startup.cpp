#include "startup.h"
#include "graphics.h"
#include <taskschd.h>
#include <sddl.h>
#include <filesystem>
#include <string>

namespace startup {
namespace {
constexpr wchar_t RunKey[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t LogonArguments[]=L"--logon --silent";
struct Bstr {BSTR value;Bstr(const wchar_t* s):value(SysAllocString(s)){}~Bstr(){SysFreeString(value);}operator BSTR()const{return value;}};
struct Variant {VARIANT value;Variant(){VariantInit(&value);}Variant(const std::wstring& text):Variant(){value.vt=VT_BSTR;value.bstrVal=SysAllocString(text.c_str());}~Variant(){VariantClear(&value);}operator VARIANT()const{return value;}};
void check(HRESULT hr){if(FAILED(hr))throw hr;}
std::wstring directory(){wchar_t path[32768]={};GetModuleFileNameW(nullptr,path,32768);return std::filesystem::path(path).parent_path().wstring();}
std::wstring launcher(){return directory()+L"\\Island.Start.exe";}
std::wstring sid(){HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return {};DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE> buffer(size);bool ok=size&&GetTokenInformation(token,TokenUser,buffer.data(),size,&size);CloseHandle(token);if(!ok)return {};LPWSTR text=nullptr;if(!ConvertSidToStringSidW(((TOKEN_USER*)buffer.data())->User.Sid,&text))return {};std::wstring result=text;LocalFree(text);return result;}
std::wstring taskName(){
#ifdef ISLAND_STARTUP_TEST
    return L"OwnIsland.Logon.Test."+std::to_wstring(GetCurrentProcessId());
#else
    return L"OwnIsland.Logon."+sid();
#endif
}
bool same(const std::wstring& a,const std::wstring& b){return !_wcsicmp(a.c_str(),b.c_str());}
bool currentAccount(const wchar_t* account){
    if(!account)return false;auto current=sid();if(same(account,current))return true;
    DWORD bytes=0,domainSize=0;SID_NAME_USE use{};LookupAccountNameW(nullptr,account,nullptr,&bytes,nullptr,&domainSize,&use);if(!bytes)return false;
    std::vector<BYTE> resolved(bytes);std::vector<wchar_t> domain(domainSize?domainSize:1);
    if(!LookupAccountNameW(nullptr,account,resolved.data(),&bytes,domain.data(),&domainSize,&use))return false;
    LPWSTR text=nullptr;if(!ConvertSidToStringSidW(resolved.data(),&text))return false;bool matches=same(text,current);LocalFree(text);return matches;
}
bool disabledRun(){HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return false;BYTE data[32]={};DWORD size=sizeof(data),type=0;auto status=RegQueryValueExW(key,L"OwnIsland",nullptr,&type,data,&size);RegCloseKey(key);return status==ERROR_SUCCESS&&type==REG_BINARY&&size>=1&&(data[0]==3||data[0]==7);}
bool ownRun(){HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,RunKey,0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return false;wchar_t text[32768]={};DWORD size=sizeof(text),type=0;auto hr=RegQueryValueExW(key,L"OwnIsland",nullptr,&type,(BYTE*)text,&size);RegCloseKey(key);if(hr!=ERROR_SUCCESS||type!=REG_SZ||size<2||size>sizeof(text)||text[size/2-1])return false;
    int count=0;auto args=CommandLineToArgvW(text,&count);if(!args)return false;bool own=count>=1&&(same(args[0],launcher())||same(args[0],directory()+L"\\Island.exe"));
    for(int i=1;own&&i<count;i++)own=!wcscmp(args[i],L"--logon")||!wcscmp(args[i],L"--silent");LocalFree(args);return own;}
struct Scheduler {
    Com<ITaskService> service;Com<ITaskFolder> folder;
    Scheduler(){check(CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(service.put())));Variant empty;check(service->Connect(empty,empty,empty,empty));check(service->GetFolder(Bstr(L"\\"),folder.put()));}
    bool own(ITaskDefinition* definition){
        Com<IActionCollection> actions;Com<IAction> action;Com<IExecAction> exec;Com<IPrincipal> principal;BSTR path=nullptr,args=nullptr,user=nullptr;
        if(FAILED(definition->get_Actions(actions.put())))return false;LONG count=0;actions->get_Count(&count);if(count!=1||FAILED(actions->get_Item(1,action.put()))||FAILED(action->QueryInterface(IID_PPV_ARGS(exec.put()))))return false;
        exec->get_Path(&path);exec->get_Arguments(&args);bool result=path&&args&&same(path,launcher())&&!wcscmp(args,LogonArguments);SysFreeString(path);SysFreeString(args);
        if(FAILED(definition->get_Principal(principal.put())))return false;principal->get_UserId(&user);TASK_LOGON_TYPE type{};TASK_RUNLEVEL_TYPE level{};principal->get_LogonType(&type);principal->get_RunLevel(&level);result=result&&currentAccount(user)&&type==TASK_LOGON_INTERACTIVE_TOKEN&&level==TASK_RUNLEVEL_LUA;SysFreeString(user);return result;
    }
    bool existing(bool* active=nullptr){Com<IRegisteredTask> task;if(FAILED(folder->GetTask(Bstr(taskName().c_str()),task.put())))return false;Com<ITaskDefinition> definition;if(FAILED(task->get_Definition(definition.put()))||!own(definition.p))return false;VARIANT_BOOL on=VARIANT_FALSE;task->get_Enabled(&on);if(active)*active=on!=VARIANT_FALSE;return true;}
    HRESULT install(){
        auto user=sid();if(user.empty()||GetFileAttributesW(launcher().c_str())==INVALID_FILE_ATTRIBUTES)return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        Com<IRegisteredTask> existingTask;if(SUCCEEDED(folder->GetTask(Bstr(taskName().c_str()),existingTask.put()))&&!existing())return E_ACCESSDENIED;
        Com<ITaskDefinition> task;check(service->NewTask(0,task.put()));Com<IRegistrationInfo> info;check(task->get_RegistrationInfo(info.put()));check(info->put_Author(Bstr(L"OwnIsland")));
        Com<IPrincipal> principal;check(task->get_Principal(principal.put()));check(principal->put_UserId(Bstr(user.c_str())));check(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));check(principal->put_RunLevel(TASK_RUNLEVEL_LUA));
        Com<ITaskSettings> settings;check(task->get_Settings(settings.put()));check(settings->put_Enabled(VARIANT_TRUE));check(settings->put_Priority(5));check(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));check(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));check(settings->put_RunOnlyIfIdle(VARIANT_FALSE));check(settings->put_RunOnlyIfNetworkAvailable(VARIANT_FALSE));check(settings->put_AllowHardTerminate(VARIANT_FALSE));check(settings->put_ExecutionTimeLimit(Bstr(L"PT0S")));check(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));check(settings->put_StartWhenAvailable(VARIANT_FALSE));
        Com<ITriggerCollection> triggers;Com<ITrigger> trigger;Com<ILogonTrigger> logon;check(task->get_Triggers(triggers.put()));check(triggers->Create(TASK_TRIGGER_LOGON,trigger.put()));check(trigger->QueryInterface(IID_PPV_ARGS(logon.put())));check(logon->put_UserId(Bstr(user.c_str())));check(logon->put_Delay(Bstr(L"PT0S")));check(logon->put_Enabled(VARIANT_TRUE));
        Com<IActionCollection> actions;Com<IAction> action;Com<IExecAction> exec;check(task->get_Actions(actions.put()));check(actions->Create(TASK_ACTION_EXEC,action.put()));check(action->QueryInterface(IID_PPV_ARGS(exec.put())));check(exec->put_Path(Bstr(launcher().c_str())));check(exec->put_Arguments(Bstr(LogonArguments)));check(exec->put_WorkingDirectory(Bstr(directory().c_str())));
        Com<IRegisteredTask> registered;Variant account(user),empty;return folder->RegisterTaskDefinition(Bstr(taskName().c_str()),task.p,TASK_CREATE_OR_UPDATE,account,empty,TASK_LOGON_INTERACTIVE_TOKEN,empty,registered.put());
    }
};
bool removeLegacy(){if(!ownRun())return true;HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,RunKey,0,KEY_SET_VALUE,&key)!=ERROR_SUCCESS)return false;auto status=RegDeleteValueW(key,L"OwnIsland");RegCloseKey(key);return status==ERROR_SUCCESS||status==ERROR_FILE_NOT_FOUND;}
}
bool enabled(){try{Scheduler scheduler;bool active=false;if(scheduler.existing(&active))return active;}catch(...){}return ownRun()&&!disabledRun();}
HRESULT prepareEarly(){
    try{Scheduler scheduler;bool active=false;if(scheduler.existing(&active)){if(!removeLegacy())return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);return active?S_OK:S_FALSE;}if(!ownRun()||disabledRun())return S_FALSE;auto result=scheduler.install();if(SUCCEEDED(result)&&!removeLegacy())return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);return result;}catch(HRESULT hr){return hr;}catch(...){return E_FAIL;}
}
bool setEnabled(bool value){
    if(value){
        HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,RunKey,0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS){bool exists=RegQueryValueExW(key,L"OwnIsland",nullptr,nullptr,nullptr,nullptr)==ERROR_SUCCESS;RegCloseKey(key);if(exists&&!ownRun())return false;}
        try{Scheduler scheduler;check(scheduler.install());return removeLegacy();}catch(...){return false;}
    }
    try{Scheduler scheduler;if(scheduler.existing())check(scheduler.folder->DeleteTask(Bstr(taskName().c_str()),0));}catch(...){return false;}
    if(ownRun()){HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,RunKey,0,KEY_SET_VALUE,&key)!=ERROR_SUCCESS)return false;auto status=RegDeleteValueW(key,L"OwnIsland");RegCloseKey(key);if(status!=ERROR_SUCCESS)return false;}return true;
}
}
