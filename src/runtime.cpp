// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "runtime.h"
#include <windows.h>
bool StandaloneMode=false;
std::wstring StateDirectoryOverride;
namespace {std::wstring quote(const std::wstring& value){std::wstring out=L"\"";size_t slashes=0;for(auto c:value){if(c==L'\\'){++slashes;continue;}if(c==L'"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}out.append(slashes*2,L'\\');out+=L'"';return out;}}
std::wstring runtimeArguments(){std::wstring args=StandaloneMode?L" --standalone":L" --integrated";if(!StateDirectoryOverride.empty())args+=L" --state-dir "+quote(StateDirectoryOverride);return args;}
bool runtimeDesktopVisible(){
    static thread_local ULONGLONG until=0;static thread_local bool visible=true;auto now=GetTickCount64();if(now<until)return visible;until=now+250;
    HDESK input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);if(!input){visible=false;return false;}
    wchar_t currentName[256]={},inputName[256]={};DWORD bytes=0;bool ok=GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,currentName,sizeof(currentName),&bytes)&&GetUserObjectInformationW(input,UOI_NAME,inputName,sizeof(inputName),&bytes);CloseDesktop(input);visible=ok&&!_wcsicmp(currentName,inputName);return visible;
}
