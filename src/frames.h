// Copyright 2026 Shizuka
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <algorithm>

class Frames {
    HANDLE wake=nullptr,timer=nullptr;
    std::thread worker;
    std::atomic<bool> active=false,stopping=false,pending=false;
    std::atomic<int> rate=60;
public:
    void start(HWND target) {
        wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
        if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        worker=std::thread([this,target]{
            HANDLE handles[]={wake,timer};
            while(!stopping){
                if(!active){WaitForSingleObject(wake,INFINITE);continue;}
                LARGE_INTEGER due;due.QuadPart=-10000000/std::max(1,rate.load());
                SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE);
                if(WaitForMultipleObjects(2,handles,FALSE,INFINITE)==WAIT_OBJECT_0+1 && !pending.exchange(true))
                    if(!PostMessageW(target,WM_APP+3,0,0))pending=false;
            }
        });
    }
    void update(bool enabled,int fps=60) {
        bool changed=active.exchange(enabled)!=enabled;
        changed=(rate.exchange(fps)!=fps)||changed;
        if(changed&&wake)SetEvent(wake);
    }
    void received() {pending=false;}
    ~Frames(){stopping=true;if(wake)SetEvent(wake);if(worker.joinable())worker.join();if(timer)CloseHandle(timer);if(wake)CloseHandle(wake);}
};
