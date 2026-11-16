#pragma once
#include "settings_transport.h"
#include "graphics.h"
#include <taskschd.h>
#include <sddl.h>
namespace island_settings {
inline bool installedStartupEnabled(){
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;DWORD bytes=0;GetTokenInformation(token,TokenUser,nullptr,0,&bytes);std::vector<BYTE> data(bytes);bool ok=bytes&&GetTokenInformation(token,TokenUser,data.data(),bytes,&bytes);CloseHandle(token);if(!ok)return false;LPWSTR text=nullptr;if(!ConvertSidToStringSidW(((TOKEN_USER*)data.data())->User.Sid,&text))return false;std::wstring name=L"OwnIsland.Logon."+std::wstring(text);LocalFree(text);
    Com<ITaskService> service;VARIANT empty{};Com<ITaskFolder> folder;Com<IRegisteredTask> task;BSTR root=SysAllocString(L"\\"),id=SysAllocString(name.c_str());
    HRESULT hr=CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(service.put()));if(SUCCEEDED(hr))hr=service->Connect(empty,empty,empty,empty);if(SUCCEEDED(hr))hr=service->GetFolder(root,folder.put());if(SUCCEEDED(hr))hr=folder->GetTask(id,task.put());SysFreeString(root);SysFreeString(id);
    auto launcher=(std::filesystem::path(expectedPanelPath()).parent_path()/L"Island.Start.exe").wstring();
    if(SUCCEEDED(hr)){
        VARIANT_BOOL enabled=VARIANT_FALSE;if(FAILED(task->get_Enabled(&enabled))||enabled==VARIANT_FALSE)return false;
        Com<ITaskDefinition> definition;Com<IActionCollection> actions;Com<IAction> action;Com<IExecAction> exec;LONG count=0;if(FAILED(task->get_Definition(definition.put()))||FAILED(definition->get_Actions(actions.put()))||FAILED(actions->get_Count(&count))||count!=1||FAILED(actions->get_Item(1,action.put()))||FAILED(action->QueryInterface(IID_PPV_ARGS(exec.put()))))return false;
        BSTR path=nullptr;exec->get_Path(&path);bool own=path&&!_wcsicmp(path,launcher.c_str());SysFreeString(path);return own;
    }
    HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&key))return false;wchar_t value[32768]={};DWORD size=sizeof(value),type=0;auto error=RegQueryValueExW(key,L"OwnIsland",nullptr,&type,(BYTE*)value,&size);RegCloseKey(key);if(error||type!=REG_SZ||size>sizeof(value))return false;int count=0;auto argv=CommandLineToArgvW(value,&count);bool own=argv&&count&&(!_wcsicmp(argv[0],launcher.c_str())||!_wcsicmp(argv[0],expectedPanelPath().c_str()));if(argv)LocalFree(argv);return own;
}
}
