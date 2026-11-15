// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#include "../src/tray_layout.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

std::ofstream trace;
void require(bool ok,const char* message){trace<<message<<": "<<(ok?"pass":"FAIL")<<std::endl;if(!ok)throw std::runtime_error(message);}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
bool allButtonsFit(HWND toolbar){RECT client{};GetClientRect(toolbar,&client);int count=(int)SendMessageW(toolbar,TB_BUTTONCOUNT,0,0);for(int i=0;i<count;i++){TBBUTTON button{};SendMessageW(toolbar,TB_GETBUTTON,i,(LPARAM)&button);if(button.fsState&TBSTATE_HIDDEN)continue;RECT r{};if(!SendMessageW(toolbar,TB_GETITEMRECT,i,(LPARAM)&r)||r.left<0||r.top<0||r.right>client.right||r.bottom>client.bottom)return false;}return true;}
int child(const std::wstring& directory){
    trace.open(std::filesystem::path(directory)/L"result.txt");int result=1;HWND popup=nullptr;
    try{
        auto twelve=nativeTrayGrid(12,40,40,1920,1080),thirteen=nativeTrayGrid(13,40,40,1920,1080);require(twelve.columns==4&&twelve.rows==3&&thirteen.columns==4&&thirteen.rows==4,"13th icon adds the missing fourth row");
        auto many=nativeTrayGrid(512,40,40,1920,1040);require(many.columns*many.rows>=512&&many.width<=1920&&many.height<=1040,"large grid fits work area");require(!nativeTrayGrid(4097,40,40,1920,1080).columns&&!nativeTrayGrid(2,0,0,1920,1080).columns,"invalid metrics fail closed");
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES};InitCommonControlsEx(&common);WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=proc;cls.lpszClassName=L"NotifyIconOverflowWindow";RegisterClassW(&cls);popup=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,30,30,160,120,nullptr,nullptr,cls.hInstance,nullptr);require(popup!=nullptr,"isolated native-popup fixture");
        HWND toolbar=CreateWindowExW(0,TOOLBARCLASSNAMEW,L"",WS_CHILD|TBSTYLE_WRAPABLE|TBSTYLE_FLAT|CCS_NODIVIDER|CCS_NORESIZE|CCS_NOPARENTALIGN,0,0,160,120,popup,nullptr,cls.hInstance,nullptr);require(toolbar!=nullptr,"standard wrapping toolbar");SendMessageW(toolbar,TB_BUTTONSTRUCTSIZE,sizeof(TBBUTTON),0);SendMessageW(toolbar,TB_SETBUTTONSIZE,0,MAKELPARAM(40,40));
        auto add=[&](int from,int count){std::vector<TBBUTTON> buttons(count);for(int i=0;i<count;i++){buttons[i].idCommand=from+i;buttons[i].iBitmap=I_IMAGENONE;buttons[i].fsState=TBSTATE_ENABLED;buttons[i].fsStyle=BTNS_BUTTON;}require(SendMessageW(toolbar,TB_ADDBUTTONS,count,(LPARAM)buttons.data())!=FALSE,"add fixture icons");};
        add(0,13);require(!allButtonsFit(toolbar),"reproduced clipped 13th icon in 160x120");SendMessageW(toolbar,TB_HIDEBUTTON,0,TRUE);require(fitNativeTrayOverflow(popup,{0,0,1920,1040}),"hidden placeholders do not add an empty row");RECT initial{};GetClientRect(popup,&initial);require(initial.right==160&&initial.bottom==120,"12 visible icons retain 4x3 despite hidden placeholder");SendMessageW(toolbar,TB_HIDEBUTTON,0,FALSE);require(fitNativeTrayOverflow(popup,{0,0,1920,1040}),"resize hidden popup before showing");require(allButtonsFit(toolbar),"all 13 buttons are inside the toolbar viewport");require(!IsWindowVisible(popup),"layout does not show hidden popup");
        ShowWindow(popup,SW_SHOWNOACTIVATE);add(13,37);require(fitNativeTrayOverflow(popup,{0,0,1920,1040})&&allButtonsFit(toolbar),"50 icons remain accessible while popup is open");RECT larger{};GetWindowRect(popup,&larger);
        while(SendMessageW(toolbar,TB_BUTTONCOUNT,0,0)>2)SendMessageW(toolbar,TB_DELETEBUTTON,2,0);require(fitNativeTrayOverflow(popup,{0,0,1920,1040})&&allButtonsFit(toolbar),"deletion shrinks grid without hiding survivors");RECT smaller{};GetWindowRect(popup,&smaller);require(smaller.bottom-smaller.top<larger.bottom-larger.top,"popup height shrinks after deletion");
        SendMessageW(toolbar,TB_SETBUTTONSIZE,0,MAKELPARAM(60,60));add(2,23);require(fitNativeTrayOverflow(popup,{0,0,1920,1040})&&allButtonsFit(toolbar),"scaled 60px buttons use current native metrics");result=0;
    }catch(const std::exception& e){trace<<e.what()<<std::endl;}if(popup)DestroyWindow(popup);return result;
}
int wmain(int argc,wchar_t** argv){
    if(argc==3&&!wcscmp(argv[1],L"--child"))return child(argv[2]);if(argc!=2)return 2;std::wstring directory=std::filesystem::absolute(argv[1]).wstring();std::filesystem::create_directories(directory);auto name=L"IslandTrayLayout-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;if(!desktop||!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limit,sizeof(limit)))return 3;
    wchar_t self[32768];GetModuleFileNameW(nullptr,self,32768);auto command=L"\""+std::wstring(self)+L"\" --child \""+directory+L"\"";STARTUPINFOW si{sizeof(si)};auto desktopName=L"winsta0\\"+name;si.lpDesktop=desktopName.data();si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION p{};int result=4;
    if(CreateProcessW(self,command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&si,&p)){if(AssignProcessToJobObject(job,p.hProcess)){ResumeThread(p.hThread);if(WaitForSingleObject(p.hProcess,30000)==WAIT_OBJECT_0){DWORD code;GetExitCodeProcess(p.hProcess,&code);result=(int)code;}}CloseHandle(p.hThread);CloseHandle(p.hProcess);}CloseHandle(job);CloseDesktop(desktop);std::ifstream log(std::filesystem::path(directory)/L"result.txt");std::cout<<log.rdbuf();return result;
}
